#include "hivedesk/shell.h"
#include "hivedesk/media.h"
#include <future>
#include <cstdio>
#include <stdexcept>
#include <string>
namespace hive { EncodedFrame media_generated_test_frame(); }
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void pump(DWORD ms){const auto end=GetTickCount64()+ms;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(1);}while(GetTickCount64()<end);}
int probe(){
    USEROBJECTFLAGS flags{};DWORD bytes=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),"Refuse synthetic UI events on interactive station");
    auto encoded=hive::media_generated_test_frame();
    hive::Shell shell;hive::ShellConfig config;config.device_name=L"Generated video test";config.desktop.tray_icon=false;
    hive::ShellCallbacks callbacks;std::string selected;callbacks.select_peer=[&](const std::string& peer){selected=peer;};
    std::string error;require(shell.create(GetModuleHandleW(nullptr),config,std::move(callbacks),error),error.c_str());
    shell.update_peers({{"a","Generated A","",47980,true},{"b","Generated B","",47980,true}});
    const auto owner=FindWindowW(L"HiveDesk.Owner",nullptr),panel=FindWindowW(L"HiveDesk.Switcher",nullptr);KillTimer(owner,1);
    const auto px=[&](int n){return MulDiv(n,int(GetDpiForWindow(panel)),96);};
    const int tile=(px(200)-2*px(12))*9/16,stride=tile+px(16);
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
