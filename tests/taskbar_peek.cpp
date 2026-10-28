#include "../src/taskbar_peek.h"
#include <cstdio>
#include <stdexcept>
#include <string>
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int probe(){
    USEROBJECTFLAGS flags{};DWORD bytes=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),"Refuse shell manipulation on interactive station");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW c{};c.hInstance=instance;c.lpfnWndProc=DefWindowProcW;c.lpszClassName=L"HiveDesk.FakeTaskbar";RegisterClassW(&c);
    const int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
    const auto make=[&](DWORD style,int x,int y,int w,int h){return CreateWindowExW(style,c.lpszClassName,L"Generated",WS_POPUP|WS_VISIBLE,x,y,w,h,nullptr,nullptr,instance,nullptr);};
    HWND viewer=make(0,0,0,width,height),tray=make(0,0,height-48,width,48),panel=make(WS_EX_TOPMOST,0,0,200,height);
    require(viewer&&tray&&panel,"fixture window creation");
    hive::TaskbarPeek peek;
    for(int dpi:{96,144,192}){
        require(hive::TaskbarPeek::corner({0,height-1},width,height,dpi),"bottom corner missing");
        require(!hive::TaskbarPeek::corner({0,0},width,height,dpi)&&!hive::TaskbarPeek::corner({-1,height-1},width,height,dpi)&&
            !hive::TaskbarPeek::corner({20,height-1},width,height,dpi),"corner overreached screen/DPI boundary");
    }
    for(int cycle=0;cycle<8;++cycle){
        const bool topmost=cycle%2!=0;
        SetWindowPos(tray,topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetWindowPos(panel,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        const HWND previous=GetWindow(tray,GW_HWNDPREV);RECT before{};GetWindowRect(tray,&before);
        require(peek.enter(panel,viewer,tray)&&peek.active(),"corner did not expose taskbar");
        require(GetWindowLongPtrW(tray,GWL_EXSTYLE)&WS_EX_TOPMOST,"taskbar not raised");
        require(peek.contains({width/2,height-24},96),"centered Start button not reachable");
        require(!peek.contains({width/2,height/2},96),"taskbar lease captured desktop");
        peek.leave(cycle%2==0);require(!peek.active(),"leaving taskbar retained input lease");
        require(bool(GetWindowLongPtrW(tray,GWL_EXSTYLE)&WS_EX_TOPMOST)==topmost,"Explorer topmost state leaked");
        RECT after{};GetWindowRect(tray,&after);require(EqualRect(&before,&after),"taskbar geometry changed");
        if(previous&&previous!=panel)require(GetWindow(tray,GW_HWNDPREV)==previous,"taskbar original predecessor not restored");
        // Reverse the exit while its snapshot may still be fading.
        require(peek.enter(panel,viewer,tray)&&peek.active(),"rapid reversal lost the taskbar lease");
        peek.reset();require(!peek.fading()&&!peek.active(),"reset left a transition visible");
        require(bool(GetWindowLongPtrW(tray,GWL_EXSTYLE)&WS_EX_TOPMOST)==topmost,"rapid reversal leaked topmost state");
    }
    require(peek.enter(panel,viewer,tray),"final lease failed");DestroyWindow(tray);peek.tick();
    require(!peek.active()&&!peek.fading(),"Explorer destruction leaked local input/overlay");
    DestroyWindow(panel);DestroyWindow(viewer);return 0;
}
}
int wmain(int argc,wchar_t**){
    if(argc>1){try{return probe();}catch(const std::exception& e){std::fprintf(stderr,"FAIL taskbar: %s\n",e.what());return 1;}}
    const std::wstring name=L"HiveDeskTaskbarTest-"+std::to_wstring(GetCurrentProcessId());
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
    std::printf("%s taskbar UX: isolated station, scoped z-order, rapid reversal and teardown (exit %lu).\n",code?"FAIL":"PASS",code);
    return code?1:0;
}
