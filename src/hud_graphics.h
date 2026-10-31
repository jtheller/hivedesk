#pragma once
#include <windows.h>
#include <objidl.h>
#include <usp10.h>
#include <gdiplus.h>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>
#pragma comment(lib,"usp10.lib")
#pragma comment(lib,"gdiplus.lib")

namespace hive {
enum class HudIcon { None, Frames, Bitrate, HardwareEncode, SoftwareEncode, HardwareDecode, SoftwareDecode, Unknown, Link, Warning };
struct HudMask {int width=0,height=0;std::vector<uint8_t> alpha;};

// CPU-only shaped text. Font linking covers non-Latin names; the cache is
// bounded and retains only raster masks, never per-frame GPU resources.
inline std::shared_ptr<const HudMask> hud_text(const std::wstring& text,int dpi){
    using Key=std::pair<int,std::wstring>;
    struct Cached {std::shared_ptr<const HudMask> mask;uint64_t used;};
    static thread_local std::map<Key,Cached> cache;static thread_local uint64_t tick=0;
    Key key{dpi,text};++tick;if(auto i=cache.find(key);i!=cache.end()){i->second.used=tick;return i->second.mask;}
    auto out=std::make_shared<HudMask>();out->height=MulDiv(28,dpi,96);
    if(text.empty())return out;
    HDC dc=CreateCompatibleDC(nullptr);
    HFONT font=CreateFontW(-MulDiv(13,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Bahnschrift");
    if(!dc||!font){if(font)DeleteObject(font);if(dc)DeleteDC(dc);return out;}
    auto old_font=SelectObject(dc,font);
    // Uniscribe's automatic links can leave CJK punctuation missing in a Latin
    // variable font. Select a covering Windows font before shaping such names.
    auto missing=[&]{std::vector<WORD> glyphs(text.size());
        if(GetGlyphIndicesW(dc,text.data(),int(text.size()),glyphs.data(),GGI_MARK_NONEXISTING_GLYPHS)==GDI_ERROR)return text.size();
        size_t count=0;for(size_t i=0;i<text.size();++i)if(glyphs[i]==0xffff&&!(text[i]>=0xd800&&text[i]<=0xdfff))++count;return count;};
    auto absent=missing();
    if(absent)for(const auto* face:{L"Microsoft YaHei UI",L"Yu Gothic UI",L"Segoe UI",L"Segoe UI Symbol"}){
        HFONT candidate=CreateFontW(-MulDiv(13,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,face);
        if(!candidate)continue;SelectObject(dc,candidate);const auto count=missing();
        if(count<absent){DeleteObject(font);font=candidate;absent=count;}else {SelectObject(dc,font);DeleteObject(candidate);}
        if(!absent)break;
    }
    SCRIPT_STRING_ANALYSIS analysis{};
    const auto status=ScriptStringAnalyse(dc,text.data(),int(text.size()),int(text.size()*3+32),-1,
        SSA_GLYPHS|SSA_FALLBACK|SSA_LINK,0,nullptr,nullptr,nullptr,nullptr,nullptr,&analysis);
    SIZE extent{};
    if(SUCCEEDED(status)&&analysis)extent=*ScriptString_pSize(analysis);else GetTextExtentPoint32W(dc,text.data(),int(text.size()),&extent);
    const int pad=MulDiv(4,dpi,96),width=std::clamp(int(extent.cx)+pad*2,1,32768),height=out->height;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;uint32_t* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
    if(bitmap&&pixels){
        auto old=SelectObject(dc,bitmap);PatBlt(dc,0,0,width,height,BLACKNESS);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
        if(analysis)ScriptStringOut(analysis,pad,std::max(0,(height-int(extent.cy))/2),0,nullptr,0,0,FALSE);
        else {RECT bounds{pad,0,width-pad,height};DrawTextW(dc,text.data(),int(text.size()),&bounds,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);}
        GdiFlush();int left=width,right=0;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)if((pixels[size_t(y)*width+x]&255)>0){left=std::min(left,x);right=std::max(right,x+1);}
        if(right>left){out->width=right-left;out->alpha.resize(size_t(out->width)*height);
            for(int y=0;y<height;++y)for(int x=left;x<right;++x)out->alpha[size_t(y)*out->width+x-left]=uint8_t(pixels[size_t(y)*width+x]&255);}
        SelectObject(dc,old);DeleteObject(bitmap);
    }
    if(analysis)ScriptStringFree(&analysis);SelectObject(dc,old_font);DeleteObject(font);DeleteDC(dc);
    if(cache.size()>=256){auto oldest=std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});cache.erase(oldest);}
    cache.emplace(std::move(key),Cached{out,tick});return out;
}

inline std::shared_ptr<const HudMask> hud_icon(HudIcon icon,int dpi){
    using namespace Gdiplus;
    struct Runtime{ULONG_PTR token{};Runtime(){GdiplusStartupInput options;GdiplusStartup(&token,&options,nullptr);}~Runtime(){if(token)GdiplusShutdown(token);}};
    static Runtime runtime;
    static thread_local std::map<std::pair<int,int>,std::shared_ptr<const HudMask>> cache;
    const auto key=std::make_pair(int(icon),dpi);if(auto i=cache.find(key);i!=cache.end())return i->second;
    auto out=std::make_shared<HudMask>();out->width=out->height=MulDiv(16,dpi,96);out->alpha.resize(size_t(out->width)*out->height);
    if(!runtime.token||icon==HudIcon::None)return out;
    // Four-times sampling keeps fine strokes clean at 100/125/150% DPI.
    const int scale=4,side=out->width*scale;
    Bitmap bitmap(side,side,PixelFormat32bppPARGB);Graphics g(&bitmap);g.Clear(Color(0,0,0,0));g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.ScaleTransform(side/24.0f,side/24.0f);Pen pen(Color(255,255,255,255),1.6f);pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);pen.SetLineJoin(LineJoinRound);
    SolidBrush white(Color(255,255,255,255)),clear(Color(0,0,0,0));
    auto line=[&](std::initializer_list<PointF> points){std::vector<PointF> v(points);g.DrawLines(&pen,v.data(),INT(v.size()));};
    auto rounded=[&](float x,float y,float w,float h,float r,bool solid){GraphicsPath p;p.AddArc(x,y,2*r,2*r,180,90);p.AddArc(x+w-2*r,y,2*r,2*r,270,90);p.AddArc(x+w-2*r,y+h-2*r,2*r,2*r,0,90);p.AddArc(x,y+h-2*r,2*r,2*r,90,90);p.CloseFigure();if(solid)g.FillPath(&white,&p);else g.DrawPath(&pen,&p);};
    auto cut=[&](auto draw){g.SetCompositingMode(CompositingModeSourceCopy);pen.SetColor(Color(0,0,0,0));draw();pen.SetColor(Color(255,255,255,255));g.SetCompositingMode(CompositingModeSourceOver);};
    switch(icon){
    case HudIcon::Frames:
        line({{14,2},{4,2}});g.DrawArc(&pen,2.0f,2.0f,4.0f,4.0f,180.0f,90.0f);line({{2,4},{2,14}});
        line({{17,5},{7,5}});g.DrawArc(&pen,5.0f,5.0f,4.0f,4.0f,180.0f,90.0f);line({{5,7},{5,17}});rounded(8,8,14,14,2,false);break;
    case HudIcon::Bitrate:{PointF v[]={{1,11},{5.3f,11},{8,3},{10,3},{14,16},{16,11},{23,11},{23,14},{18,14},{15,22},{13,22},{9,9},{7,14},{1,14}};g.FillPolygon(&white,v,INT(std::size(v)));break;}
    case HudIcon::HardwareEncode:
        rounded(5,5,14,14,3,true);for(float a:{8.0f,16.0f}){line({{a,2},{a,5}});line({{a,19},{a,22}});line({{2,a},{5,a}});line({{19,a},{22,a}});}
        cut([&]{line({{8,12},{16,12}});line({{13,9},{16,12},{13,15}});});break;
    case HudIcon::SoftwareEncode:
        rounded(2,3,16,13,2,false);line({{5,7},{5,12},{10,9.5f},{5,7}});line({{11,20},{21,20}});line({{18,17},{21,20},{18,23}});break;
    case HudIcon::HardwareDecode:
        rounded(2,5,17,13,2,true);cut([&]{g.DrawEllipse(&pen,5.0f,8.5f,6.0f,6.0f);});line({{2,3},{2,20}});line({{6,18},{6,21}});line({{10,18},{10,21}});line({{20,12},{23,12}});line({{21,10},{23,12},{21,14}});break;
    case HudIcon::SoftwareDecode:
        line({{9,3},{10,1},{14,1},{15,3},{18,5},{20,5.3f},{22,8.3f},{21,10.3f},{21,13.3f},{22,15.3f},{20,18.3f},{18,18.6f},{15,20.6f},{14,22.6f},{10,22.6f},{9,20.6f},{6,18.6f},{4,18.3f},{2,15.3f},{3,13.3f},{3,10.3f},{2,8.3f},{4,5.3f},{6,5},{9,3}});line({{9,8},{15,12},{9,16},{9,8}});break;
    case HudIcon::Warning:line({{12,2},{23,21},{1,21},{12,2}});line({{12,8},{12,13}});g.FillEllipse(&white,11.0f,16.0f,2.0f,2.0f);break;
    case HudIcon::Link:rounded(2,7,11,8,4,false);rounded(11,9,11,8,4,false);line({{8,12},{16,12}});break;
    default:rounded(3,3,18,18,3,false);line({{8,12},{16,12}});break;
    }
    g.Flush(FlushIntentionSync);BitmapData data{};Rect area(0,0,side,side);
    if(bitmap.LockBits(&area,ImageLockModeRead,PixelFormat32bppPARGB,&data)==Ok){
        for(int y=0;y<out->height;++y)for(int x=0;x<out->width;++x){unsigned sum=0;for(int yy=0;yy<scale;++yy)for(int xx=0;xx<scale;++xx)sum+=*(static_cast<uint8_t*>(data.Scan0)+(y*scale+yy)*data.Stride+(x*scale+xx)*4+3);out->alpha[size_t(y)*out->width+x]=uint8_t((sum+8)/16);}
        bitmap.UnlockBits(&data);
    }
    if(cache.size()>128)cache.clear();cache.emplace(key,out);return out;
}
}
