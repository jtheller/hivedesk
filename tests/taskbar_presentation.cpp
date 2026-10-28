// Explicit generated-content DWM check. No real Explorer manipulation, input,
// foreground activation or cursor movement. Never part of normal CTest.
#include "../src/taskbar_peek.h"
#include <cstdio>
#include <stdexcept>
namespace {
constexpr COLORREF Blue=RGB(20,60,220),Orange=RGB(240,150,20);
LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(m==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);
        HBRUSH brush=CreateSolidBrush(COLORREF(GetWindowLongPtrW(h,GWLP_USERDATA)));FillRect(dc,&r,brush);DeleteObject(brush);EndPaint(h,&ps);return 0;}
    return DefWindowProcW(h,m,w,l);
}
void pump(DWORD ms){const auto until=GetTickCount64()+ms;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(1);}while(GetTickCount64()<until);}
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
}
int main(){
    const HWND foreground=GetForegroundWindow();HWND panel=nullptr,tray=nullptr;int result=1;
    try{
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW c{};c.hInstance=GetModuleHandleW(nullptr);c.lpfnWndProc=proc;c.lpszClassName=L"HiveDesk.GeneratedTaskbar";RegisterClassW(&c);
        const int x=GetSystemMetrics(SM_CXSCREEN)-330,y=80;
        const auto make=[&](COLORREF color){auto h=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,c.lpszClassName,L"Generated taskbar fade",WS_POPUP,x,y,320,48,nullptr,nullptr,c.hInstance,nullptr);
            SetWindowLongPtrW(h,GWLP_USERDATA,color);ShowWindow(h,SW_SHOWNOACTIVATE);UpdateWindow(h);return h;};
        tray=make(Orange);panel=make(Blue);require(tray&&panel,"generated windows unavailable");
        SetWindowPos(panel,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);pump(70);
        const auto pixel=[&]{HDC screen=GetDC(nullptr);const auto color=GetPixel(screen,x+160,y+24);ReleaseDC(nullptr,screen);return color;};
        require(pixel()==Blue,"fixture is obscured");
        hive::TaskbarPeek peek;require(peek.enter(panel,nullptr,tray,false),"generated taskbar entry failed");
        const HWND layer=FindWindowW(L"HiveDesk.TaskbarFade",nullptr);
        require(layer&&(GetWindowLongPtrW(layer,GWL_EXSTYLE)&(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE))==(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE),"fade intercepts native mouse/activation");
        BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        pump(35);peek.tick();pump(15);const auto entering=pixel();
        if(animations)require(GetRValue(entering)>GetRValue(Blue)+10&&GetRValue(entering)<GetRValue(Orange)-10,"entry did not visibly crossfade");
        pump(180);peek.tick();pump(25);require(pixel()==Orange,"native taskbar did not become fully visible");
        peek.leave();pump(35);peek.tick();pump(15);const auto leaving=pixel();
        if(animations)require(GetRValue(leaving)>GetRValue(Blue)+10&&GetRValue(leaving)<GetRValue(Orange)-10,"exit did not visibly crossfade");
        pump(180);peek.tick();pump(25);require(pixel()==Blue&&!peek.fading(),"Hive did not restore or transition survived");
        require(GetForegroundWindow()==foreground,"generated fade stole focus");
        std::printf("PASS real DWM taskbar fade: entry=%06lx exit=%06lx, both directions visible, transparent overlay, focus unchanged\n",entering,leaving);result=0;
    }catch(const std::exception& e){std::printf("FAIL generated taskbar fade: %s\n",e.what());}
    if(panel)DestroyWindow(panel);if(tray)DestroyWindow(tray);return result;
}
