#pragma once
#include "debug_model.h"
#include "hud_graphics.h"
#include <cmath>
#include <optional>
#include <algorithm>

namespace hive {
enum class HudTone { Normal, Muted, Blue, Amber, Red };
struct HudCell {
    int x=0,width=0;std::wstring text;HudTone tone=HudTone::Normal;
    HudIcon icon=HudIcon::None;std::wstring reserve;
    bool operator==(const HudCell&)const=default;
};
struct CompactHud {
    int width=352;HudTone tone=HudTone::Blue;std::vector<HudCell> cells;
    bool automatic=false,divider=false;int separator=-1;
    bool operator==(const CompactHud&)const=default;
};
inline HudTone hud_tone(DiagnosticTone tone){
    return tone==DiagnosticTone::Muted?HudTone::Muted:tone==DiagnosticTone::Warning?HudTone::Amber:tone==DiagnosticTone::Critical?HudTone::Red:HudTone::Normal;
}
// Metadata is supplied by the active remote encoder, never inferred from GPU names.
inline const wchar_t* hud_encoder(EncoderBackend backend){
    switch(backend){case EncoderBackend::Nvenc:return L"NVENC";case EncoderBackend::Intel:return L"QSV";
    case EncoderBackend::Amd:return L"AMF";case EncoderBackend::Software:return L"FFmpeg";default:return L"Encoder";}
}
inline CompactHud compact_hud(const DebugOverlayState& s,bool stats,bool warning,const std::wstring& name=L"Remote computer"){
    const bool health=s.health_time&&s.now-s.health_time<2000;
    const auto pressure=health?s.health.pressure:0;
    const auto label=pressure&PressureNetwork?L"Network slow":pressure&PressureEncode?L"Encode slow":pressure&PressureDecode?L"Decode slow":L"Stream slow";
    CompactHud hud;hud.automatic=true;hud.tone=warning?HudTone::Amber:HudTone::Normal;
    if(!stats){hud.cells={{0,0,label,HudTone::Amber,HudIcon::Warning}};return hud;}
    hud.divider=true;
    const bool source=s.source_time&&s.now-s.source_time<500;
    const bool encode=source&&s.source.encode_age_us!=UINT32_MAX&&uint64_t(s.source.encode_age_us)+(s.now-s.source_time)*1000<500000;
    const bool view=s.viewer.session&&s.viewer.service_sample_age_us<500000;
    const bool receive=s.viewer.session&&s.viewer.receive_sample_age_us<500000;
    const auto budget=diagnostic_budget(s.health.target_millihz);
    const auto number=[](double value,int precision){std::wostringstream out;out<<std::fixed<<std::setprecision(precision)<<value;return out.str();};
    const auto ms=[&](const wchar_t* label,uint32_t us,bool fresh){return std::wstring(label)+L" "+(fresh?number(us/1000.0,1):L"--")+L"ms";};
    const auto target=s.health.target_millihz?number(s.health.target_millihz/1000.0,0):L"--";
    const auto backend=s.source.backend;
    const auto encode_icon=backend==EncoderBackend::Unknown?HudIcon::Unknown:backend==EncoderBackend::Software?HudIcon::SoftwareEncode:HudIcon::HardwareEncode;
    const auto decode_name=s.viewer.software_decode?L"FFmpeg":L"NVDEC";
    hud.cells={{0,0,name.empty()?L"Remote computer":name,HudTone::Normal},
        {0,0,(view?number(s.viewer.presented_millihz/1000.0,0):L"--")+L" / "+target+L" fps",view?(warning?HudTone::Amber:HudTone::Normal):HudTone::Muted,HudIcon::Frames,target+L" / "+target+L" fps"},
        {0,0,ms(hud_encoder(backend),s.source.encode_us,encode),hud_tone(diagnostic_threshold(s.source.encode_us,budget*3/4,budget,encode)),encode_icon,std::wstring(hud_encoder(backend))+L" 88.8ms"},
        {0,0,ms(decode_name,s.viewer.decode_us,view),hud_tone(diagnostic_threshold(s.viewer.decode_us,budget*3/4,budget,view)),s.viewer.software_decode?HudIcon::SoftwareDecode:HudIcon::HardwareDecode,std::wstring(decode_name)+L" 88.8ms"},
        {0,0,(receive?number(s.viewer.receive_kbps/1000.0,0):L"--")+L" Mbps",receive?(warning&&(pressure&PressureNetwork)?HudTone::Amber:HudTone::Normal):HudTone::Muted,HudIcon::Bitrate,L"888 Mbps"}};
    return hud;
}
inline CompactHud connecting_hud(bool retry,const std::wstring& name){
    CompactHud hud;hud.automatic=true;hud.tone=HudTone::Blue;
    hud.cells={{0,0,retry?L"Reconnecting":L"Connecting",HudTone::Blue,retry?HudIcon::Retry:HudIcon::Link},
    {0,0,name,HudTone::Normal},{0,0,L"Ctrl+Alt+`",HudTone::Muted}};return hud;
}
inline CompactHud unavailable_hud(const std::wstring& message){
    CompactHud hud;hud.automatic=true;hud.tone=HudTone::Red;
    hud.cells={{0,0,message,HudTone::Red,HudIcon::Unavailable}};return hud;
}
// Layout uses the same cached Unicode masks as painting. Gaps belong to groups;
// the separator occupies their midpoint rather than adding another pair of gaps.
inline CompactHud layout_hud(CompactHud hud,int dpi,int maximum=32768){
    const auto px=[&](int v){return MulDiv(v,dpi,96);};
    if(!hud.automatic){hud.width=std::min(maximum,px(hud.width));for(auto& c:hud.cells){c.x=px(c.x);c.width=px(c.width);}return hud;}
    const int gap=px(20),inner=px(6),icon=px(16),margin=px(12);
    std::vector<int> widths;int total=2*margin;
    for(const auto& cell:hud.cells){const int w=std::max(hud_text(cell.text,dpi)->width,hud_text(cell.reserve,dpi)->width);widths.push_back(w);total+=w+(cell.icon!=HudIcon::None?icon+inner:0);}
    if(!widths.empty())total+=gap*int(widths.size()-1);
    // Keep every metric reachable in normal windows. Only constrain the name
    // when the actual viewport cannot hold the full original Unicode spelling.
    const size_t name=hud.divider?0:1;
    if(total>maximum&&name<widths.size()){const int take=std::min(total-maximum,std::max(0,widths[name]-px(24)));widths[name]-=take;total-=take;}
    int x=margin;
    for(size_t i=0;i<hud.cells.size();++i){auto& c=hud.cells[i];if(i)x+=gap;
        if(i==1&&hud.divider)hud.separator=x-gap/2;
        if(c.icon!=HudIcon::None)x+=icon+inner;c.x=x;c.width=widths[i];x+=widths[i];}
    hud.width=std::max(1,std::min(maximum,total));return hud;
}
inline COLORREF hud_color(HudTone tone){
    switch(tone){case HudTone::Muted:return RGB(170,170,170);case HudTone::Blue:return RGB(148,196,255);
    case HudTone::Amber:return RGB(239,196,124);case HudTone::Red:return RGB(247,147,154);default:return RGB(236,236,236);}
}
// Small CPU-only premultiplied surface. No capture readback, blur shader, or
// additional GPU device competes with video. Unicode GDI masks use grayscale AA.
class CompactHudSurface {
    HDC dc_{};HBITMAP bitmap_{};HGDIOBJ old_{};
    uint32_t *pixels_{};int width_=0,height_=0,dpi_=0;std::optional<CompactHud> cached_;
    uint64_t revision_=0,presented_revision_=0;HWND presented_window_{};RECT presented_rect_{};BYTE presented_alpha_=0;
    void clear(){
        if(dc_&&old_)SelectObject(dc_,old_);if(bitmap_)DeleteObject(bitmap_);if(dc_)DeleteDC(dc_);
        dc_=nullptr;bitmap_=nullptr;old_=nullptr;pixels_=nullptr;width_=height_=dpi_=0;cached_.reset();invalidate_present();
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
    bool render(const CompactHud& input,int width,int height,int dpi){
        if(width<=0||height<=0||dpi<=0)return false;
        const auto hud=layout_hud(input,dpi,width);
        if(width_!=width||height_!=height||dpi_!=dpi){
            clear();BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
            info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
            dc_=CreateCompatibleDC(nullptr);
            bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels_),nullptr,0);
            if(!dc_||!bitmap_){clear();return false;}
            old_=SelectObject(dc_,bitmap_);width_=width;height_=height;dpi_=dpi;
        }
        if(cached_&&*cached_==hud)return true;
        const auto px=[&](int value){return MulDiv(value,dpi,96);};
        const double radius=px(8),half_width=width/2.0,half_height=height/2.0;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            const double dx=std::abs(x+.5-half_width)-(half_width-radius),dy=std::abs(y+.5-half_height)-(half_height-radius);
            const double distance=std::hypot(std::max(dx,0.0),std::max(dy,0.0))+std::min(std::max(dx,dy),0.0)-radius;
            const double coverage=std::clamp(.5-distance,0.0,1.0);
            uint32_t pixel=over(0,RGB(21,27,36),uint32_t(171*coverage));
            if(distance> -1.0)pixel=over(pixel,RGB(184,203,229),uint32_t(35*coverage));

            pixels_[size_t(y)*width+x]=pixel;
        }
        auto stamp=[&](const HudMask& mask,int left,int top,COLORREF color,int clip_left,int clip_right){
            for(int y=0;y<mask.height;++y)for(int x=0;x<mask.width;++x){const int xx=left+x,yy=top+y;
                if(xx<clip_left||xx>=clip_right||xx<0||xx>=width||yy<0||yy>=height)continue;
                auto& dst=pixels_[size_t(yy)*width+xx];dst=over(dst,color,mask.alpha[size_t(y)*mask.width+x]);}
        };
        if(hud.separator>=0&&hud.separator<width)for(int y=px(8);y<std::min(height,px(20));++y)
            for(int x=hud.separator;x<std::min(width,hud.separator+std::max(1,px(1)));++x){auto& dst=pixels_[size_t(y)*width+x];dst=over(dst,RGB(236,236,236),40);}
        for(const auto& cell:hud.cells){
            auto mask=hud_text(cell.text,dpi);const int right=std::min(width-px(8),cell.x+cell.width);
            if(right<=cell.x)continue;
            if(mask->width>cell.width){
                // The complete name remains in the model and all identity paths.
                // Shape the ellipsis too, without splitting UTF-16 surrogate pairs.
                std::wstring short_text=cell.text;
                while(!short_text.empty()&&hud_text(short_text+L"\x2026",dpi)->width>cell.width){short_text.pop_back();if(!short_text.empty()&&short_text.back()>=0xd800&&short_text.back()<=0xdbff)short_text.pop_back();}
                mask=hud_text(short_text+L"\x2026",dpi);
            }
            const int offset=cell.reserve.empty()?0:std::max(0,cell.width-mask->width);
            stamp(*mask,cell.x+offset,(height-mask->height)/2,hud_color(cell.tone),cell.x,right);
            if(cell.icon!=HudIcon::None){const auto icon=hud_icon(cell.icon,dpi);stamp(*icon,cell.x-px(22),(height-icon->height)/2,hud_color(cell.tone),0,width);}
        }
        cached_=hud;++revision_;return true;
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
