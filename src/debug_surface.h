#pragma once
#include "hivedesk/diagnostic_window.h"
#include <windows.h>
#include <vector>
#include <string>

namespace hive {
struct DebugSpan {std::wstring text;DiagnosticTone tone=DiagnosticTone::Normal;};
using DebugRows=std::vector<std::vector<DebugSpan>>;
inline COLORREF debug_color(DiagnosticTone tone){
    switch(tone){case DiagnosticTone::Muted:return RGB(157,173,191);case DiagnosticTone::Warning:return RGB(255,199,83);
    case DiagnosticTone::Critical:return RGB(255,104,116);default:return RGB(236,244,252);}
}
// Two reusable top-down DIBs: a grayscale glyph mask and premultiplied output.
// GDI does not preserve alpha. Composite coverage explicitly so the background
// is translucent while glyph interiors remain opaque (no ClearType fringes).
class DebugSurface {
    HDC dc_=nullptr,mask_dc_=nullptr;HBITMAP bitmap_=nullptr,mask_bitmap_=nullptr;
    HGDIOBJ old_=nullptr,mask_old_=nullptr;uint32_t* pixels_=nullptr;uint32_t* mask_=nullptr;
    int width_=0,height_=0,background_dpi_=0;std::vector<uint32_t> background_;
    struct Glyph {int advance=0,width=0,height=0;std::vector<uint8_t> coverage;};
    std::array<Glyph,95> glyphs_;HFONT glyph_font_=nullptr;int glyph_dpi_=0;
    const Glyph& glyph(wchar_t character,HFONT font,int dpi){
        if(glyph_font_!=font||glyph_dpi_!=dpi){glyphs_={};glyph_font_=font;glyph_dpi_=dpi;}
        auto& result=glyphs_[character>=32&&character<=126?character-32:L'?'-32];
        if(!result.coverage.empty())return result;
        if(character<32||character>126)character=L'?';
        const auto old_font=SelectObject(mask_dc_,font);SIZE extent{};GetTextExtentPoint32W(mask_dc_,&character,1,&extent);
        result.advance=extent.cx;result.width=std::min(width_,int(extent.cx)+2);result.height=std::min(height_,MulDiv(18,dpi,96));
        PatBlt(mask_dc_,0,0,result.width,result.height,BLACKNESS);SetBkMode(mask_dc_,TRANSPARENT);SetTextColor(mask_dc_,RGB(255,255,255));
        RECT rect{1,0,result.width,result.height};DrawTextW(mask_dc_,&character,1,&rect,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(mask_dc_,old_font);GdiFlush();result.coverage.resize(size_t(result.width)*result.height);
        for(int y=0;y<result.height;++y)for(int x=0;x<result.width;++x)result.coverage[size_t(y)*result.width+x]=uint8_t(mask_[size_t(y)*width_+x]);
        return result;
    }
    void clear(){
        if(dc_){SelectObject(dc_,old_);DeleteObject(bitmap_);DeleteDC(dc_);}
        if(mask_dc_){SelectObject(mask_dc_,mask_old_);DeleteObject(mask_bitmap_);DeleteDC(mask_dc_);}
        dc_=mask_dc_=nullptr;bitmap_=mask_bitmap_=nullptr;pixels_=mask_=nullptr;width_=height_=0;background_.clear();
    }
public:
    ~DebugSurface(){clear();}
    bool size(int width,int height){
        if(width==width_&&height==height_&&pixels_&&mask_)return true;
        clear();if(width<=0||height<=0)return false;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        dc_=CreateCompatibleDC(nullptr);mask_dc_=CreateCompatibleDC(nullptr);
        if(!dc_||!mask_dc_){clear();return false;}
        bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels_),nullptr,0);
        mask_bitmap_=CreateDIBSection(mask_dc_,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&mask_),nullptr,0);
        if(!bitmap_||!mask_bitmap_){clear();return false;}
        old_=SelectObject(dc_,bitmap_);mask_old_=SelectObject(mask_dc_,mask_bitmap_);width_=width;height_=height;return true;
    }
    const uint32_t* pixels()const{return pixels_;}
    bool render(int width,int height,int dpi,HFONT font,const DebugRows& rows,bool compact=false){
        if(!size(width,height))return false;
        const auto px=[dpi](int n){return MulDiv(n,dpi,96);};
        if(background_.empty()||background_dpi_!=dpi){
            PatBlt(mask_dc_,0,0,width,height,BLACKNESS);
            const auto brush=SelectObject(mask_dc_,GetStockObject(WHITE_BRUSH)),pen=SelectObject(mask_dc_,GetStockObject(NULL_PEN));
            RoundRect(mask_dc_,0,0,width,height,px(12),px(12));SelectObject(mask_dc_,brush);SelectObject(mask_dc_,pen);GdiFlush();
            constexpr uint32_t alpha=158; // 62% dark body; independent glyph alpha.
            background_.resize(size_t(width)*height);background_dpi_=dpi;
            for(size_t i=0;i<background_.size();++i)background_[i]=mask_[i]?((alpha<<24)|((22*alpha/255)<<16)|((27*alpha/255)<<8)|(35*alpha/255)):0;
        }
        std::copy(background_.begin(),background_.end(),pixels_);
        // Cache monochrome glyph coverage once per font/DPI. Numeric changes
        // now do CPU compositing only, not dozens of GDI text calls per tick.
        int y=compact?px(6):px(12);
        for(const auto& row:rows){
            int x=px(14);
            for(const auto& span:row){
                const auto color=debug_color(span.tone);const uint32_t rgb[3]={GetBValue(color),GetGValue(color),GetRValue(color)};
                for(const auto character:span.text){
                    const auto& shape=glyph(character,font,dpi);
                    for(int yy=0;yy<shape.height&&y+yy<height;++yy)for(int xx=0;xx<shape.width;++xx){
                        const int destination=x+xx-1;if(destination<px(14)||destination>=width-px(10))continue;
                        const uint32_t coverage=shape.coverage[size_t(yy)*shape.width+xx];if(!coverage)continue;
                        const size_t i=size_t(y+yy)*width+destination;const uint32_t before=pixels_[i];
                        uint32_t after=(coverage+((before>>24)*(255-coverage)+127)/255)<<24;
                        for(int c=0;c<3;++c)after|=((rgb[c]*coverage+((before>>(c*8))&255)*(255-coverage)+127)/255)<<(c*8);
                        pixels_[i]=after;
                    }
                    x+=shape.advance;if(x>=width-px(10))break;
                }
            }
            y+=px(18);if(y>=height)break;
        }
        return true;
    }
    bool present(HWND window){
        RECT rect{};GetWindowRect(window,&rect);POINT destination{rect.left,rect.top},source{};SIZE size{width_,height_};
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        return UpdateLayeredWindow(window,nullptr,&destination,&size,dc_,&source,0,&blend,ULW_ALPHA)!=FALSE;
    }
};
}
