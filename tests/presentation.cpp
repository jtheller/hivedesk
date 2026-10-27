#include "hivedesk/media.h"
#include "hivedesk/experience_trace.h"
#include "../src/panel_composition.h"
#include <future>
#include <cstdio>
#include <vector>
namespace hive { EncodedFrame media_generated_test_frame(); }
static void pump(DWORD ms){const auto end=GetTickCount64()+ms;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(1);}while(GetTickCount64()<end);}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_NCHITTEST)return HTTRANSPARENT;if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;return DefWindowProcW(h,m,w,l);}
int main(){
    // Explicit generated-content probe. Never inject input, activate a window,
    // capture desktop content, or register hooks. Not part of normal CTest.
    const auto foreground=GetForegroundWindow();HWND video=nullptr,cover=nullptr;int result=1;
    try{
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"HiveDeskGeneratedPresentation";RegisterClassW(&c);
        const int x=GetSystemMetrics(SM_CXSCREEN)-330,y=80,w=320,h=180;
        video=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,c.lpszClassName,L"HiveDesk generated video check",WS_POPUP,x,y,w,h,nullptr,nullptr,c.hInstance,nullptr);
        cover=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT,c.lpszClassName,L"HiveDesk generated cover check",WS_POPUP,x,y,w,h,video,nullptr,c.hInstance,nullptr);
        PanelComposition composition;composition.create(cover);std::vector<uint32_t> pixels(w*h,0xff162030);
        for(int row=0;row<3;++row)for(int yy=8+row*58;yy<54+row*58;++yy)for(int xx=12;xx<308;++xx)pixels[yy*w+xx]=0xff2080e0;
        composition.present(pixels.data(),w,h);composition.show(w,h,0,true,false);composition.reveal_cards({{12,8,308,54},{12,66,308,112},{12,124,308,170}},RGB(22,32,48));
        SetWindowPos(video,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);SetWindowPos(cover,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        pump(80);
        HDC early_screen=GetDC(nullptr);const auto early_first=GetPixel(early_screen,x+160,y+30),early_last=GetPixel(early_screen,x+160,y+146);ReleaseDC(nullptr,early_screen);
        BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        std::printf("Stagger samples first=%06lx last=%06lx\n",(unsigned long)early_first,(unsigned long)early_last);
        if(animations&&GetBValue(early_first)<=GetBValue(early_last)+20)throw std::runtime_error("No visible staggered card entrance");
        pump(220);
        // Sample only pixels occupied by our generated cover.
        HDC screen=GetDC(nullptr);const auto card=GetPixel(screen,x+160,y+30);ReleaseDC(nullptr,screen);
        std::printf("DComp card sample=%06lx expected=%06lx\n",(unsigned long)card,(unsigned long)RGB(32,128,224));
        if(card!=RGB(32,128,224))throw std::runtime_error("Cards are hidden behind the backing visual");
        composition.reveal_cards({{12,8,308,54},{12,66,308,112},{12,124,308,170}},RGB(22,32,48),2);
        composition.drag_slot(2,1,0,58);pump(170);
        screen=GetDC(nullptr);const auto gap=GetPixel(screen,x+160,y+90);ReleaseDC(nullptr,screen);
        if(gap!=RGB(22,32,48))throw std::runtime_error("Drag did not animate the adjacent card out of the insertion slot");
        composition.finish_cards();
        hive::experience_enabled(true);hive::MediaViewer viewer;auto open=std::async(std::launch::async,[&]{std::string error;if(!viewer.open(video,error))throw std::runtime_error(error);});
        while(open.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);open.get();
        auto frame=hive::media_generated_test_frame();
        for(unsigned i=0;i<100&&!viewer.has_presented_frame();++i){frame.sequence=i;frame.capture_time_us=frame.source_ready_us=0;viewer.submit(frame);pump(25);}
        const bool presented=viewer.has_presented_frame();std::printf("Real covered viewer: decoded=%d presented=%d status=%s\n",viewer.has_frame(),presented,viewer.stats().c_str());
        auto close=std::async(std::launch::async,[&]{viewer.close();});while(close.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);close.get();
        if(!presented)throw std::runtime_error("No real first Present under cover");
        if(GetForegroundWindow()!=foreground)throw std::runtime_error("Probe changed foreground window");
        result=0;
    }catch(const std::exception& e){std::printf("FAIL generated presentation: %s\n",e.what());}
    if(cover)DestroyWindow(cover);if(video)DestroyWindow(video);std::printf("%s generated presentation (no input)\n",result?"FAIL":"PASS");return result;
}
