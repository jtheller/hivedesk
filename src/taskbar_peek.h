#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace hive {
// UI-thread-owned, temporary z-order lease. Explorer's opacity, size, auto-hide
// and alignment settings are never changed. Only our snapshot layer fades.
class TaskbarPeek {
    HWND tray_=nullptr,previous_=nullptr,focus_=nullptr,panel_=nullptr,viewer_=nullptr,layer_=nullptr;
    bool active_=false,was_topmost_=false,viewer_foreground_=false;
    HDC dc_=nullptr;HBITMAP bitmap_=nullptr;HGDIOBJ old_=nullptr;
    RECT bounds_{};ULONGLONG fade_started_=0;
    static constexpr UINT Flags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS;
    static LRESULT CALLBACK layer_proc(HWND h,UINT m,WPARAM w,LPARAM l){
        if(m==WM_NCHITTEST)return HTTRANSPARENT;
        if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        return DefWindowProcW(h,m,w,l);
    }
    void clear_layer(){
        fade_started_=0;if(layer_)ShowWindow(layer_,SW_HIDE);
        if(dc_&&old_)SelectObject(dc_,old_);old_=nullptr;
        if(bitmap_)DeleteObject(bitmap_);bitmap_=nullptr;
        if(dc_)DeleteDC(dc_);dc_=nullptr;
    }
    bool snapshot(RECT bounds){
        // Read the currently composed strip, including an interrupted fade,
        // before replacing our layer. No images are persisted or transmitted.
        const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
        if(width<=0||height<=0||uint64_t(width)*height>8*1024*1024)return false;
        HDC screen=GetDC(nullptr),next=CreateCompatibleDC(screen);void* pixels=nullptr;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
        info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        HBITMAP image=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        HGDIOBJ old=next&&image?SelectObject(next,image):nullptr;
        const bool copied=screen&&next&&image&&BitBlt(next,0,0,width,height,screen,bounds.left,bounds.top,SRCCOPY|CAPTUREBLT);
        if(screen)ReleaseDC(nullptr,screen);
        if(!copied){if(old)SelectObject(next,old);if(image)DeleteObject(image);if(next)DeleteDC(next);clear_layer();return false;}
        for(size_t i=0;i<size_t(width)*height;++i)static_cast<uint32_t*>(pixels)[i]|=0xff000000;
        clear_layer();dc_=next;bitmap_=image;old_=old;bounds_=bounds;
        if(!layer_){
            WNDCLASSW c{};c.hInstance=GetModuleHandleW(nullptr);c.lpfnWndProc=layer_proc;c.lpszClassName=L"HiveDesk.TaskbarFade";
            RegisterClassW(&c);
            layer_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,
                c.lpszClassName,L"HiveDesk taskbar transition",WS_POPUP,0,0,0,0,nullptr,nullptr,c.hInstance,nullptr);
        }
        return layer_!=nullptr;
    }
    void draw(BYTE alpha){
        if(!dc_||!layer_)return;
        POINT position{bounds_.left,bounds_.top},source{};SIZE size{bounds_.right-bounds_.left,bounds_.bottom-bounds_.top};
        BLENDFUNCTION blend{AC_SRC_OVER,0,alpha,AC_SRC_ALPHA};
        UpdateLayeredWindow(layer_,nullptr,&position,&size,dc_,&source,0,&blend,ULW_ALPHA);
    }
    void fade(){
        BOOL animation=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animation,0);
        if(!animation||!dc_||!layer_){clear_layer();return;}
        draw(255);SetWindowPos(layer_,HWND_TOPMOST,0,0,0,0,Flags|SWP_SHOWWINDOW);fade_started_=GetTickCount64();
    }
public:
    ~TaskbarPeek(){reset();if(layer_)DestroyWindow(layer_);}
    TaskbarPeek()=default;
    TaskbarPeek(const TaskbarPeek&)=delete;TaskbarPeek& operator=(const TaskbarPeek&)=delete;
    bool active()const{return active_;}
    bool fading()const{return fade_started_!=0;}
    static bool corner(POINT p,int width,int height,int dpi){
        const int edge=std::max(1,MulDiv(4,dpi,96));
        return width>0&&height>0&&p.x>=0&&p.x<edge&&p.y>=height-edge&&p.y<height;
    }
    bool enter(HWND panel,HWND viewer,HWND tray=nullptr,bool activate=true){
        if(active_)return true;
        tray_=tray?tray:FindWindowW(L"Shell_TrayWnd",nullptr);
        if(!tray_||!(GetWindowLongPtrW(tray_,GWL_STYLE)&WS_VISIBLE)){tray_=nullptr;return false;}
        RECT r{};if(!GetWindowRect(tray_,&r)){tray_=nullptr;return false;}
        const int height=GetSystemMetrics(SM_CYSCREEN),width=GetSystemMetrics(SM_CXSCREEN);
        if(r.top>=height-2)r={0,height-MulDiv(48,int(GetDpiForWindow(panel)),96),width,height};
        RECT screen{0,0,width,height},visible{};if(!IntersectRect(&visible,&r,&screen)){tray_=nullptr;return false;}
        snapshot(visible);
        panel_=panel;viewer_=viewer;focus_=GetForegroundWindow();viewer_foreground_=focus_==viewer;
        previous_=GetWindow(tray_,GW_HWNDPREV);was_topmost_=(GetWindowLongPtrW(tray_,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
        active_=true;
        // Foreground activation lets Explorer expose an auto-hidden taskbar.
        SetWindowPos(tray_,HWND_TOPMOST,0,0,0,0,Flags);if(activate)SetForegroundWindow(tray_);
        fade();return true;
    }
    void leave(bool animate=true){
        if(!active_){if(!animate)clear_layer();return;}
        if(animate)snapshot(bounds_);else clear_layer();
        // Restoring foreground can synchronously send WM_ACTIVATE to Hive.
        // Revoke the lease first so that activation cannot reenter leave().
        active_=false;
        const HWND foreground=GetForegroundWindow();
        if(IsWindow(tray_)){
            // Restore the original topmost band first, then its predecessor.
            SetWindowPos(tray_,was_topmost_?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,Flags);
            if(previous_&&IsWindow(previous_)&&previous_!=layer_)SetWindowPos(tray_,previous_,0,0,0,0,Flags);
        }
        if(panel_&&IsWindowVisible(panel_))SetWindowPos(panel_,HWND_TOPMOST,0,0,0,0,Flags);
        // Do not steal focus from an application launched through the taskbar.
        if(foreground==tray_&&focus_&&IsWindow(focus_))SetForegroundWindow(focus_);
        if(viewer_foreground_&&GetForegroundWindow()==viewer_&&IsWindowVisible(viewer_))SetWindowPos(viewer_,HWND_TOP,0,0,0,0,Flags);
        active_=false;tray_=previous_=focus_=panel_=viewer_=nullptr;
        if(animate)fade();
    }
    void tick(){
        if(active_&&!IsWindow(tray_)){leave(false);return;}
        if(!fade_started_)return;
        const float t=std::clamp(float(GetTickCount64()-fade_started_)/180.f,0.f,1.f);
        if(t>=1){clear_layer();return;}
        const float remaining=1-t;draw(BYTE(255*remaining*remaining*remaining));
    }
    void reset(){leave(false);clear_layer();}
};
}
