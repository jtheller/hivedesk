#include "hivedesk/media.h"
#include "hivedesk/experience_trace.h"
#include "../src/panel_composition.h"
#include <future>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
namespace hive { EncodedFrame media_generated_test_frame(); std::vector<EncodedFrame> media_generated_cadence_frames(); }
static void pump(DWORD ms){const auto end=GetTickCount64()+ms;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(1);}while(GetTickCount64()<end);}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_NCHITTEST)return HTTRANSPARENT;if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;return DefWindowProcW(h,m,w,l);}
static bool took_foreground=false;
static void CALLBACK foreground_event(HWINEVENTHOOK,DWORD,HWND window,LONG,LONG,DWORD,DWORD){
    DWORD process=0;GetWindowThreadProcessId(window,&process);if(process==GetCurrentProcessId())took_foreground=true;
}
int main(int argc,char** argv){
    // Explicit generated-content probe. Never inject input, activate a window,
    // capture desktop content, or register input hooks. Not part of normal CTest.
    const auto foreground=GetForegroundWindow();HWND video=nullptr,cover=nullptr;int result=1;
    // Observe only this probe's focus events. The user may change applications
    // during verification; that is not evidence that a generated window activated.
    const auto focus_hook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,foreground_event,GetCurrentProcessId(),0,WINEVENT_OUTOFCONTEXT);
    const bool peek_only=argc==2&&std::strcmp(argv[1],"--peek-only")==0;
    const bool warm_only=argc==2&&std::strcmp(argv[1],"--warm-only")==0;
    try{
        if(!focus_hook)throw std::runtime_error("Cannot observe probe foreground events");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"HiveDeskGeneratedPresentation";RegisterClassW(&c);
        const int x=GetSystemMetrics(SM_CXSCREEN)-330,y=80,w=320,h=180;
        video=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|(peek_only?WS_EX_NOREDIRECTIONBITMAP:0),c.lpszClassName,L"HiveDesk generated video check",WS_POPUP,x,y,w,h,nullptr,nullptr,c.hInstance,nullptr);
        cover=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT,c.lpszClassName,L"HiveDesk generated cover check",WS_POPUP,x,y,w,h,video,nullptr,c.hInstance,nullptr);
        PanelComposition composition;composition.create(cover);std::vector<uint32_t> pixels(w*h,0xff162030);
        for(int row=0;row<3;++row)for(int yy=8+row*58;yy<54+row*58;++yy)for(int xx=12;xx<308;++xx)pixels[yy*w+xx]=0xff2080e0;
        composition.present(pixels.data(),w,h);composition.show(w,h,0,true,false);composition.reveal_cards({{12,8,308,54},{12,66,308,112},{12,124,308,170}},RGB(22,32,48));
        SetWindowPos(video,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);SetWindowPos(cover,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        if(!warm_only){
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
        }
        if(peek_only)SetPropW(video,L"HiveDesk.PeekSurface",reinterpret_cast<HANDLE>(1));
        hive::experience_enabled(true);hive::MediaViewer viewer;auto open=std::async(std::launch::async,[&]{std::string error;if(!viewer.open(video,error,{},true))throw std::runtime_error(error);});
        while(open.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);open.get();
        auto frame=hive::media_generated_test_frame();
        frame.capture_time_us=frame.source_ready_us=0;
        viewer.submit(frame);unsigned warm_refreshes=0;
        for(unsigned i=0;i<300&&!viewer.has_frame();++i){
            pump(10);
            // Match the real feedback lane: a cold GPU may expire its first
            // decoded picture, and the paused host must provide another IDR.
            if(i%25==24&&viewer.feedback().recover){++frame.sequence;viewer.submit(frame);++warm_refreshes;}
        }
        std::printf("Warm fixture recovery requests=%u status=%s\n",warm_refreshes,viewer.stats().c_str());
        if(!viewer.has_frame()||viewer.has_presented_frame())throw std::runtime_error("Warm preparation touched presentation or failed to decode");
        // A real Hive selection promotes an image prepared while the panel
        // was open. It can be older than the presentation freshness budget.
        // The next live frame arrives only after the remote host is activated.
        pump(300);
        if(peek_only)viewer.preview(true);else viewer.activate();pump(100);
        for(unsigned i=0;i<100&&!viewer.has_presented_frame();++i){++frame.sequence;viewer.submit(frame);pump(25);}
        const bool presented=viewer.has_presented_frame();std::printf("Real covered viewer: decoded=%d presented=%d status=%s\n",viewer.has_frame(),presented,viewer.stats().c_str());
        if(peek_only){
            // Put our generated opaque surface below the transparent preview.
            // Pixel reads remain entirely inside these two generated windows.
            SetWindowLongPtrW(cover,GWLP_HWNDPARENT,0);
            composition.present(pixels.data(),w,h);composition.cover(w,h,false);composition.ready();
            SetWindowPos(cover,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            SetWindowPos(video,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);pump(100);
            const auto sample=[&]{HDC screen=GetDC(nullptr);const auto color=GetPixel(screen,x+160,y+30);ReleaseDC(nullptr,screen);return color;};
            const auto backdrop=sample();if(backdrop!=RGB(32,128,224))throw std::runtime_error("Unready preview is not transparent over generated background");
            viewer.peek_opacity(true);pump(240);const auto live=sample();
            std::printf("Peek visibility pixels: backdrop=%06lx live=%06lx status=%s\n",(unsigned long)backdrop,(unsigned long)live,viewer.stats().c_str());
            if(live==backdrop||live==CLR_INVALID||live==RGB(0,0,0))throw std::runtime_error("Live preview did not become visible through DWM");
            viewer.peek_opacity(false);pump(240);if(sample()!=backdrop)throw std::runtime_error("Preview fade-out did not restore underlying image");
            viewer.peek_opacity(true);pump(50);viewer.activate();pump(80);if(sample()!=live)throw std::runtime_error("Preview promotion changed/lost its presented pixels");
            std::printf("PASS real DWM peek: transparent readiness, live fade-in/out, same-surface promotion; pixels=%06lx\n",(unsigned long)live);
        }
        ShowWindow(cover,SW_HIDE);pump(100);
        const auto samples=[&]{std::array<COLORREF,3> colors{};HDC screen=GetDC(nullptr);
            colors[0]=GetPixel(screen,x+160,y+30);colors[1]=GetPixel(screen,x+60,y+90);colors[2]=GetPixel(screen,x+260,y+146);ReleaseDC(nullptr,screen);return colors;};
        const auto stable=samples();
        viewer.set_display_cadence(true);++frame.sequence;viewer.submit(frame);pump(200);
        const auto repeats=viewer.diagnostics().repeated_presentations;
        if(!repeats)throw std::runtime_error("Cadence did not repeat the final displayed image without new input");
        unsigned mismatches=0;
        for(unsigned i=0;i<100;++i){
            if(i%10==0){++frame.sequence;viewer.submit(frame);}
            pump(3);const auto actual=samples();
            if(actual!=stable){if(!mismatches)std::printf("Cadence pixel mismatch: expected=%06lx,%06lx,%06lx actual=%06lx,%06lx,%06lx\n",stable[0],stable[1],stable[2],actual[0],actual[1],actual[2]);++mismatches;}
        }
        std::printf("Cadence continuous pixel samples: %u/100 mismatches\n",mismatches);
        if(mismatches)throw std::runtime_error("Cadence flashed or replaced a static picture during repeated presentation");
        // Increasing red values identify old-picture replay while fresh images
        // alternate with gaps that must retain the latest displayed picture.
        auto ramp=hive::media_generated_cadence_frames();int previous_red=GetRValue(stable[0]);unsigned dynamic_samples=0;
        const auto check_dynamic_pixel=[&]{const auto actual=samples();++dynamic_samples;
                const int red=GetRValue(actual[0]);
                if(red+5<previous_red||std::abs(int(GetBValue(actual[0]))-int(GetBValue(stable[0])))>5||std::abs(int(GetGValue(actual[0]))-int(GetGValue(stable[0])))>5)
                {
                    DWORD foreground_pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);
                    std::printf("Cadence failure sample=%u previous_red=%d stable=%06lx,%06lx,%06lx actual=%06lx,%06lx,%06lx foreground_pid=%lu own_pid=%lu visible=%d iconic=%d status=%s\n",
                        dynamic_samples,previous_red,stable[0],stable[1],stable[2],actual[0],actual[1],actual[2],foreground_pid,GetCurrentProcessId(),IsWindowVisible(video),IsIconic(video),viewer.stats().c_str());
                    throw std::runtime_error("Cadence flashed or replayed older pixels during fresh/repeated handoff");
                }
                previous_red=std::max(previous_red,red);
        };
        for(auto& next:ramp){
            next.sequence=++frame.sequence;next.capture_time_us=next.source_ready_us=0;viewer.submit(next);
            for(unsigned i=0;i<12;++i){pump(i==5?80:3);check_dynamic_pixel();}
        }
        // A generated final IDR can expire under scheduling pressure just like
        // a static host's final update. Honor feedback with a fresh IDR rather
        // than assuming scanout within the fixed 113 ms sampling interval.
        // Every additional pixel is checked: no black/old-frame retry is hidden.
        const auto final_deadline=GetTickCount64()+2000;uint64_t last_refresh=0;
        while(previous_red<215&&GetTickCount64()<final_deadline){
            const auto now=GetTickCount64();
            if(viewer.feedback().recover&&now-last_refresh>=250){auto next=ramp.back();next.sequence=++frame.sequence;viewer.submit(std::move(next));last_refresh=now;}
            pump(3);check_dynamic_pixel();
        }
        if(previous_red<215){std::printf("Cadence final red=%d status=%s\n",previous_red,viewer.stats().c_str());throw std::runtime_error("Cadence did not reach the last changing picture");}
        std::printf("Cadence fresh/repeated handoff: %u pixel samples, monotonic red reached %d\n",dynamic_samples,previous_red);
        viewer.set_display_cadence(false);pump(40);const auto stopped=viewer.diagnostics().repeated_presentations;pump(100);
        if(viewer.diagnostics().repeated_presentations!=stopped)throw std::runtime_error("Cadence kept repeating after disable");
        std::printf("Cadence: %llu repeats with no additional encoded frames; live disable passed\n",static_cast<unsigned long long>(repeats));
        if(peek_only){
            // Keep a live preview behind each replacement. Exercise actual
            // DComp targets, CUDA registration, Present and asynchronous close
            // repeatedly without activating windows or touching user input.
            for(unsigned cycle=0;cycle<12;++cycle){
                HWND next=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT,c.lpszClassName,
                    L"HiveDesk generated preview replacement",WS_POPUP,x,y,w,h,nullptr,nullptr,c.hInstance,nullptr);
                if(!next)throw std::runtime_error("Create replacement preview");
                struct WindowLease{HWND value;~WindowLease(){DestroyWindow(value);}} lease{next};
                SetPropW(next,L"HiveDesk.PeekSurface",reinterpret_cast<HANDLE>(1));
                hive::MediaViewer replacement;std::string error;
                auto opening=std::async(std::launch::async,[&]{return replacement.open(next,error,{},true);});
                while(opening.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(2);
                if(!opening.get())throw std::runtime_error(error);
                SetWindowPos(next,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);replacement.preview(true);
                auto fresh=ramp[cycle%ramp.size()];fresh.sequence=0;fresh.session=100+cycle;fresh.capture_time_us=fresh.source_ready_us=0;
                for(unsigned attempt=0;attempt<100&&!replacement.has_presented_frame();++attempt){++fresh.sequence;replacement.submit(fresh);pump(15);}
                if(!replacement.has_presented_frame())throw std::runtime_error("Replacement preview never completed a real Present");
                replacement.peek_opacity(true);pump(35);replacement.peek_opacity(false);pump(25);replacement.activate();pump(70);
                const auto actual=samples();const int expected_red=80+20*(cycle%ramp.size());
                if(std::abs(int(GetRValue(actual[0]))-expected_red)>5||std::abs(int(GetGValue(actual[0]))-int(GetGValue(stable[0])))>5)
                    throw std::runtime_error("Rapid preview replacement showed black or stale pixels");
                replacement.request_stop();ShowWindow(next,SW_HIDE);
                auto retirement=std::async(std::launch::async,[&]{replacement.close();});
                while(retirement.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(2);retirement.get();
            }
            std::puts("PASS real DWM: 12 replacement preview lifecycles, rapid fade reversal/promotion, distinct final pixels and retirement");
        }
        auto close=std::async(std::launch::async,[&]{viewer.close();});while(close.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);close.get();
        if(!presented)throw std::runtime_error("No real first Present under cover");
        pump(1);DWORD foreground_process=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground_process);
        if(took_foreground||foreground_process==GetCurrentProcessId())throw std::runtime_error("Generated probe window took foreground");
        if(GetForegroundWindow()!=foreground)std::puts("External foreground changed; no probe activation observed.");
        result=0;
    }catch(const std::exception& e){std::printf("FAIL generated presentation: %s\n",e.what());}
    if(cover)DestroyWindow(cover);if(video)DestroyWindow(video);if(focus_hook)UnhookWinEvent(focus_hook);std::printf("%s generated presentation (no input)\n",result?"FAIL":"PASS");return result;
}
