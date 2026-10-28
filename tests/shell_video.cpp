#include "hivedesk/shell.h"
#include "hivedesk/media.h"
#include "hivedesk/experience_trace.h"
#include <future>
#include <cstdio>
#include <stdexcept>
#include <string>
namespace hive { EncodedFrame media_generated_test_frame(); }
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void pump(DWORD ms){const auto end=GetTickCount64()+ms;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(1);}while(GetTickCount64()<end);}
void prepare_warm(hive::MediaViewer& viewer,hive::EncodedFrame image){
    // Real hosts answer recovery feedback with a fresh IDR (300 ms debounce).
    // A one-shot fake source otherwise strands the viewer when cold GPU setup
    // exceeds the unchanged 50 ms frame-age policy. Keep the 2 s readiness bound.
    uint64_t sequence=0;ULONGLONG sent=0;
    auto send=[&]{image.sequence=sequence++;image.capture_time_us=image.source_ready_us=0;viewer.submit(image);sent=GetTickCount64();};send();
    for(unsigned i=0;i<80&&!viewer.has_frame();++i){
        pump(25);const auto feedback=viewer.feedback();
        if(!viewer.has_frame()&&feedback.recover&&GetTickCount64()-sent>=300){
            std::printf("Generated warm source: responding to recovery; age_expired=%llu\n",static_cast<unsigned long long>(feedback.age_expired));send();
        }
    }
}
int probe(){
    USEROBJECTFLAGS flags{};DWORD bytes=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),"Refuse synthetic UI events on interactive station");
    hive::experience_enabled(true);
    auto encoded=hive::media_generated_test_frame();
    hive::ShellConfig config;config.device_name=L"Generated video test";config.desktop.tray_icon=false;
    std::vector<std::shared_ptr<const hive::ViewerSurface>> visible;
    hive::ShellCallbacks callbacks;callbacks.visible_peers=[&](const auto& surfaces){visible=surfaces;};std::string selected;callbacks.select_peer=[&](const std::string& peer){selected=peer;};
    hive::Shell shell; // Callback state must outlive native/worker teardown.
    std::string error;require(shell.create(GetModuleHandleW(nullptr),config,std::move(callbacks),error),error.c_str());
    shell.update_peers({{"a","Generated A","",47980,true},{"b","Generated B","",47980,true}});
    const auto owner=FindWindowW(L"HiveDesk.Owner",nullptr),panel=FindWindowW(L"HiveDesk.Switcher",nullptr);KillTimer(owner,1);
    const auto px=[&](int n){return MulDiv(n,int(GetDpiForWindow(panel)),96);};
    const int tile=(px(200)-2*px(12))*9/16,stride=tile+px(16);
    // Prewarm every visible tile before selection; real NVDEC state and HWND
    // must survive promotion. All UI events remain on this isolated station.
    shell.update_peers({{"a","Generated A","",47980,true,hive::FeatureWarmStream},{"b","Generated B","",47980,true,hive::FeatureWarmStream}});pump(10);
    SendMessageW(owner,WM_HOTKEY,81,0);pump(10);
    require(visible.size()==2,"Opening Hive did not prepare every visible remote");
    const auto warm_surface=visible.front();require(!IsWindowVisible(warm_surface->window),"Warm viewer became foreground/visible");
    hive::MediaViewer warm_viewer;
    auto warm_open=std::async(std::launch::async,[&]{std::string why;require(warm_viewer.open(warm_surface->window,why,{},true),why.c_str());});
    while(warm_open.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);warm_open.get();
    prepare_warm(warm_viewer,encoded);
    if(!warm_viewer.has_frame()){
        const auto d=warm_viewer.diagnostics();const auto f=warm_viewer.feedback();
        std::fprintf(stderr,"Warm failure diagnostics: %s; expired=%llu discarded=%llu skipped=%llu recover=%d map_us=%u convert_us=%u arrival_age_us=%u\n",
            warm_viewer.stats().c_str(),static_cast<unsigned long long>(d.age_expired),static_cast<unsigned long long>(d.discarded),static_cast<unsigned long long>(d.skipped),f.recover,d.nvdec_map_us,d.convert_us,f.arrival_age_us);
    }
    require(warm_viewer.has_frame(),"Hidden warm viewer failed to decode its first picture");
    pump(30);require(!warm_viewer.has_presented_frame(),"Unselected warm viewer touched its swapchain");
    const int warm_y=px(12)+stride+tile/2;
    SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(100),warm_y));SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(px(100),warm_y));
    require(shell.viewer_surface()==warm_surface&&warm_viewer.has_frame(),"Selection recreated warmed media surface or lost decoded picture");
    require(visible.empty(),"Closing Hive did not revoke the visible warm set");
    warm_viewer.activate();
    shell.show_local();auto warm_close=std::async(std::launch::async,[&]{warm_viewer.close();});
    while(warm_close.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);warm_close.get();
    // Cancellation must also wake a presenter which was never activated.
    {
        hive::MediaViewer unselected;
        auto open=std::async(std::launch::async,[&]{std::string why;require(unselected.open(warm_surface->window,why,{},true),why.c_str());});
        while(open.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);open.get();
        prepare_warm(unselected,encoded);
        require(unselected.has_frame()&&!unselected.has_presented_frame(),"Unselected preparation failed");
        auto close=std::async(std::launch::async,[&]{unselected.close();});
        while(close.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);close.get();
    }
    shell.update_peers({{"a","Generated A","",47980,true},{"b","Generated B","",47980,true}});pump(10);
    for(int cycle=0;cycle<4;++cycle){
        const int row=1+cycle%2,y=px(12)+row*stride+tile/2;
        SendMessageW(owner,WM_HOTKEY,81,0);SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(100),y));
        if(cycle%2)SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(600),px(240)));
        SendMessageW(panel,WM_LBUTTONUP,0,cycle%2?MAKELPARAM(px(600),px(240)):MAKELPARAM(px(100),y));
        const auto surface=shell.viewer_surface();require(surface&&surface->peer==selected,"Selection did not bind the real renderer's HWND");
        hive::MediaViewer viewer;
        auto open=std::async(std::launch::async,[&]{std::string why;require(viewer.open(surface->window,why),why.c_str());});
        while(open.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);open.get();
        for(unsigned i=0;i<80&&!viewer.has_frame();++i){encoded.sequence=i;encoded.capture_time_us=encoded.source_ready_us=0;viewer.submit(encoded);pump(25);}
        require(viewer.has_frame(),"Real HEVC444 frame did not decode in Shell viewer");
        if(cycle%2){shell.set_remote_size(640,360);SetWindowPos(surface->window,nullptr,180,150,800,500,SWP_NOACTIVATE|SWP_NOZORDER);pump(25);}
        // Invisible stations cannot scan out. Actual DWM Present is checked by
        // the explicit generated-content PresentationCheck, without user input.
        shell.show_local();auto close=std::async(std::launch::async,[&]{viewer.close();});
        while(close.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(5);close.get();
    }
    return 0;
}
}
int wmain(int argc,wchar_t**){
    if(argc>1){try{return probe();}catch(const std::exception& e){std::fprintf(stderr,"FAIL shell video: %s\n",e.what());return 1;}}
    const std::wstring name=L"HiveDeskShellVideoTest-"+std::to_wstring(GetCurrentProcessId());
    HWINSTA original=GetProcessWindowStation(),station=CreateWindowStationW(nullptr,0,WINSTA_ALL_ACCESS,nullptr);
    if(!station){std::printf("FAIL create noninteractive window station: %lu\n",GetLastError());return 1;}
    HDESK desktop=nullptr;
    if(SetProcessWindowStation(station)){desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);SetProcessWindowStation(original);}
    if(!desktop){CloseWindowStation(station);std::puts("FAIL create isolated desktop");return 1;}
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    wchar_t station_name[256]{};DWORD needed=0;GetUserObjectInformationW(station,UOI_NAME,station_name,sizeof(station_name),&needed);
    std::wstring command=L"\""+std::wstring(executable)+L"\" --child",location=std::wstring(station_name)+L"\\"+name;
    STARTUPINFOW startup{sizeof(startup)};startup.lpDesktop=location.data();startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION child{};DWORD code=1;
    if(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)){
        if(WaitForSingleObject(child.hProcess,20000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&code);
        else{TerminateProcess(child.hProcess,124);WaitForSingleObject(child.hProcess,5000);code=124;}
        CloseHandle(child.hThread);CloseHandle(child.hProcess);
    }
    CloseDesktop(desktop);CloseWindowStation(station);
    std::printf("%s real shell video: isolated station, async DXGI startup, NVENC/NVDEC, resize and teardown (exit %lu).\n",code?"FAIL":"PASS",code);
    return code?1:0;
}
