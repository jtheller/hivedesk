#pragma once
#include "debug_model.h"
#include <cmath>
#include <optional>
#include <algorithm>

namespace hive {
enum class HudTone { Normal, Muted, Blue, Amber, Red };
struct HudCell {
    int x=0,width=0;std::wstring text;HudTone tone=HudTone::Normal;
    bool operator==(const HudCell&)const=default;
};
struct CompactHud {
    int width=352;HudTone tone=HudTone::Blue;std::vector<HudCell> cells;
    bool operator==(const CompactHud&)const=default;
};
inline HudTone hud_tone(DiagnosticTone tone){
    return tone==DiagnosticTone::Muted?HudTone::Muted:tone==DiagnosticTone::Warning?HudTone::Amber:tone==DiagnosticTone::Critical?HudTone::Red:HudTone::Normal;
}
// Observational only: no pacing/rate feedback, no invented input-to-photon time.
inline CompactHud compact_hud(const DebugOverlayState& s,bool stats,bool warning){
    const bool health=s.health_time&&s.now-s.health_time<2000;
    const auto pressure=health?s.health.pressure:0;
    const auto label=warning?(pressure&PressureNetwork?L"Network":pressure&PressureEncode?L"Encode":pressure&PressureDecode?L"Decode":L"Slow"):L"Live";
    CompactHud hud;hud.tone=warning?HudTone::Amber:HudTone::Blue;
    if(!stats){hud.width=184;hud.cells={{22,150,std::wstring(label)+L" \x00b7 Slow",HudTone::Amber}};return hud;}
    const bool source=s.source_time&&s.now-s.source_time<500;
    const bool encode=source&&s.source.encode_age_us!=UINT32_MAX&&uint64_t(s.source.encode_age_us)+(s.now-s.source_time)*1000<500000;
    const bool view=s.viewer.session&&s.viewer.service_sample_age_us<500000;
    const bool receive=s.viewer.session&&s.viewer.receive_sample_age_us<500000;
    const auto budget=diagnostic_budget(s.health.target_millihz);
    const auto number=[](double value,int precision){std::wostringstream out;out<<std::fixed<<std::setprecision(precision)<<value;return out.str();};
    const auto ms=[&](const wchar_t* label,uint32_t us,bool fresh){return std::wstring(label)+(fresh?number(us/1000.0,1):L"--")+L"ms";};
    hud.cells={{22,58,label,warning?HudTone::Amber:HudTone::Muted},
        {84,50,(view?number(s.viewer.presented_millihz/1000.0,0):L"--")+L" fps",view?(warning?HudTone::Amber:HudTone::Normal):HudTone::Muted},
        {140,62,ms(L"E ",s.source.encode_us,encode),hud_tone(diagnostic_threshold(s.source.encode_us,budget*3/4,budget,encode))},
        {208,62,ms(L"D ",s.viewer.decode_us,view),hud_tone(diagnostic_threshold(s.viewer.decode_us,budget*3/4,budget,view))},
        {276,66,(receive?number(s.viewer.receive_kbps/1000.0,0):L"--")+L" Mbps",receive?(warning&&(pressure&PressureNetwork)?HudTone::Amber:HudTone::Normal):HudTone::Muted}};
    return hud;
}
inline CompactHud connecting_hud(bool retry,const std::wstring& name){
    return {352,HudTone::Blue,{{22,86,retry?L"Reconnecting":L"Connecting",HudTone::Blue},{114,148,name,HudTone::Normal},{270,72,L"Ctrl+Alt+`",HudTone::Muted}}};
}
inline COLORREF hud_color(HudTone tone){
    switch(tone){case HudTone::Muted:return RGB(179,188,201);case HudTone::Blue:return RGB(148,196,255);
    case HudTone::Amber:return RGB(239,196,124);case HudTone::Red:return RGB(247,147,154);default:return RGB(239,243,249);}
}
// Small CPU-only premultiplied surface. No capture readback, blur shader, or
// additional GPU device competes with video. Unicode GDI masks use grayscale AA.
class CompactHudSurface {
    HDC dc_{},mask_dc_{};HBITMAP bitmap_{},mask_bitmap_{};HGDIOBJ old_{},mask_old_{};HFONT font_{};
    uint32_t *pixels_{},*mask_{};int width_=0,height_=0,dpi_=0;std::optional<CompactHud> cached_;
    uint64_t revision_=0,presented_revision_=0;HWND presented_window_{};RECT presented_rect_{};BYTE presented_alpha_=0;
    void clear(){
        if(dc_&&old_)SelectObject(dc_,old_);if(bitmap_)DeleteObject(bitmap_);if(dc_)DeleteDC(dc_);
        if(mask_dc_&&mask_old_)SelectObject(mask_dc_,mask_old_);if(mask_bitmap_)DeleteObject(mask_bitmap_);if(mask_dc_)DeleteDC(mask_dc_);
        if(font_)DeleteObject(font_);dc_=mask_dc_=nullptr;bitmap_=mask_bitmap_=nullptr;old_=mask_old_=nullptr;font_=nullptr;
        pixels_=mask_=nullptr;width_=height_=dpi_=0;cached_.reset();invalidate_present();
    }
    static uint32_t over(uint32_t before,COLORREF color,uint32_t alpha){
        const uint32_t inverse=255-alpha;
        return ((alpha+((before>>24)*inverse+127)/255)<<24)|
            (((GetRValue(color)*alpha+((before>>16)&255)*inverse+127)/255)<<16)|
            (((GetGValue(color)*alpha+((before>>8)&255)*inverse+127)/255)<<8)|
            ((GetBValue(color)*alpha+(before&255)*inverse+127)/255);
    }
public:
    CompactHudSurface()=default;~CompactHudSurface(){clear();}
    CompactHudSurface(const CompactHudSurface&)=delete;CompactHudSurface& operator=(const CompactHudSurface&)=delete;
    void invalidate_present(){presented_window_=nullptr;}
    const uint32_t* pixels()const{return pixels_;}
    bool render(const CompactHud& hud,int width,int height,int dpi){
        if(width<=0||height<=0||dpi<=0)return false;
        if(width_!=width||height_!=height||dpi_!=dpi){
            clear();BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
            info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
            dc_=CreateCompatibleDC(nullptr);mask_dc_=CreateCompatibleDC(nullptr);
            bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels_),nullptr,0);
            mask_bitmap_=CreateDIBSection(mask_dc_,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&mask_),nullptr,0);
            font_=CreateFontW(-MulDiv(12,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            if(!dc_||!mask_dc_||!bitmap_||!mask_bitmap_||!font_){clear();return false;}
            old_=SelectObject(dc_,bitmap_);mask_old_=SelectObject(mask_dc_,mask_bitmap_);width_=width;height_=height;dpi_=dpi;
        }
        if(cached_&&*cached_==hud)return true;
        const auto px=[&](int value){return MulDiv(value,dpi,96);};
        const double radius=px(8),half_width=width/2.0,half_height=height/2.0;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            const double dx=std::abs(x+.5-half_width)-(half_width-radius),dy=std::abs(y+.5-half_height)-(half_height-radius);
            const double distance=std::hypot(std::max(dx,0.0),std::max(dy,0.0))+std::min(std::max(dx,dy),0.0)-radius;
            const double coverage=std::clamp(.5-distance,0.0,1.0);
            uint32_t pixel=over(0,RGB(21,27,36),uint32_t(150*coverage));
            if(distance> -1.0)pixel=over(pixel,RGB(184,203,229),uint32_t(35*coverage));
            const double dot=std::hypot(x+.5-px(12),y+.5-half_height);
            pixel=over(pixel,hud_color(hud.tone),uint32_t(255*std::clamp(px(2)+.5-dot,0.0,1.0)));
            pixels_[size_t(y)*width+x]=pixel;
        }
        auto old_font=SelectObject(mask_dc_,font_);SetTextColor(mask_dc_,RGB(255,255,255));SetBkMode(mask_dc_,TRANSPARENT);
        for(const auto& cell:hud.cells){
            RECT bounds{px(cell.x),0,std::min(width-px(8),px(cell.x+cell.width)),height};if(bounds.right<=bounds.left)continue;
            PatBlt(mask_dc_,0,0,width,height,BLACKNESS);
            DrawTextW(mask_dc_,cell.text.c_str(),int(cell.text.size()),&bounds,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            GdiFlush();for(int y=0;y<height;++y)for(int x=bounds.left;x<bounds.right;++x){const auto i=size_t(y)*width+x;pixels_[i]=over(pixels_[i],hud_color(cell.tone),mask_[i]&255);}
        }
        SelectObject(mask_dc_,old_font);cached_=hud;++revision_;return true;
    }
    bool present(HWND window,BYTE alpha=255){
        RECT rect{};GetWindowRect(window,&rect);POINT destination{rect.left,rect.top},source{};SIZE size{width_,height_};
        if(presented_window_==window&&presented_revision_==revision_&&presented_alpha_==alpha&&EqualRect(&rect,&presented_rect_))return true;
        BLENDFUNCTION blend{AC_SRC_OVER,0,alpha,AC_SRC_ALPHA};
        if(!pixels_||!UpdateLayeredWindow(window,nullptr,&destination,&size,dc_,&source,0,&blend,ULW_ALPHA))return false;
        presented_window_=window;presented_revision_=revision_;presented_rect_=rect;presented_alpha_=alpha;return true;
    }
    void composite(uint32_t* target,int width,int height,int left,int top)const{
        if(!pixels_)return;
        for(int y=0;y<height_&&top+y<height;++y)for(int x=0;x<width_&&left+x<width;++x){
            if(top+y<0||left+x<0)continue;
            const auto src=pixels_[size_t(y)*width_+x];auto& dst=target[size_t(top+y)*width+left+x];const auto inv=255-(src>>24);
            dst=0xff000000u|((((src>>16)&255)+(((dst>>16)&255)*inv+127)/255)<<16)|
                ((((src>>8)&255)+(((dst>>8)&255)*inv+127)/255)<<8)|((src&255)+((dst&255)*inv+127)/255);
        }
    }
};
}
