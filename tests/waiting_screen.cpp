#include "hivedesk/shell.h"
#include "../src/viewer_geometry.h"
#include <cmath>
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <future>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
HWND find(const wchar_t* name){return FindWindowW(name,nullptr);}
// The isolated station itself is invisible; inspect the child window show state.
bool shown(HWND window){return (GetWindowLongW(window,GWL_STYLE)&WS_VISIBLE)!=0;}
std::wstring title(HWND window){wchar_t text[1024]{};GetWindowTextW(window,text,1024);return text;}
void pump(DWORD ms){
    const auto end=GetTickCount64()+ms;
    do{MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(1);}while(GetTickCount64()<end);
}
hive::Thumbnail image(uint8_t value){
    hive::Thumbnail result;result.bgra.resize(size_t(hive::SnapshotWidth)*hive::SnapshotHeight*4,value);return result;
}
int probe(){
    // The entire child runs on a noninteractive window station, including its
    // input worker. No SwitchDesktop, SendInput, or physical cursor movement.
    wchar_t station[256]{};DWORD size=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,station,sizeof(station),&size)!=FALSE,"Read test station");
    USEROBJECTFLAGS flags{};require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&size)!=FALSE&&!(flags.dwFlags&WSF_VISIBLE),"Refuse UI probe on an interactive window station");
    std::atomic<unsigned> clipboard_echoes=0;
    hive::Shell shell;hive::ShellConfig config;config.device_name=L"Waiting test";config.desktop.tray_icon=false;
    hive::ShellCallbacks callbacks;unsigned selected=0;std::vector<std::string> saved_order;std::string last_selected;bool allow_save=true;
    callbacks.clipboard_changed=[&](const std::wstring&){++clipboard_echoes;};
    callbacks.select_peer=[&](const std::string& id){++selected;last_selected=id;};
    callbacks.save_order=[&](const std::vector<std::string>& order){if(!allow_save)return false;saved_order=order;return true;};
    std::string error;require(shell.create(GetModuleHandleW(nullptr),config,std::move(callbacks),error),error.c_str());
    shell.update_peers({{"a","Computer A","",47980,true},{"b","Computer B","",47980,true},{"c","Computer C","",47980,true}});
    const auto owner=find(L"HiveDesk.Owner"),panel=find(L"HiveDesk.Switcher");
    HWND waiting=find(L"HiveDesk.Waiting");
    require(owner&&panel&&waiting,"Missing shell windows");
    KillTimer(owner,1); // Disable automatic hot corner sampling in the isolated probe.
    const int dpi=int(GetDpiForWindow(panel));const auto px=[&](int n){return MulDiv(n,dpi,96);};
    const auto select=[&](int index){
        SendMessageW(owner,WM_HOTKEY,81,0);
        const int tile=(px(200)-2*px(12))*9/16;
        SendMessageW(panel,WM_LBUTTONDOWN,0,MAKELPARAM(px(100),px(12)+index*(tile+px(16))+tile/2));
        SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(px(100),px(12)+index*(tile+px(16))+tile/2));
        waiting=find(L"HiveDesk.Waiting");
    };
    shell.taskbar_interaction_self_test();
    // This station owns its own clipboard; the user's clipboard is untouched.
    for(const std::wstring value:{L"First desktop\n\u4e2d\u6587 \U0001f642",L"",L"Second desktop\r\nUS keyboard"}){
        auto apply=std::async(std::launch::async,[&]{shell.apply_clipboard(value);});
        while(apply.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump(1);
        apply.get();require(shell.cached_clipboard()==value,"Clipboard handoff returned before updating the cache");
        require(OpenClipboard(owner)!=FALSE,"Open isolated clipboard");
        const HANDLE memory=GetClipboardData(CF_UNICODETEXT);const auto* text=memory?static_cast<const wchar_t*>(GlobalLock(memory)):nullptr;
        const bool same=text&&std::wstring(text)==value;if(text)GlobalUnlock(memory);CloseClipboard();
        require(same,"Clipboard handoff did not replace native Unicode text");
        SendMessageW(owner,WM_CLIPBOARDUPDATE,0,0);pump(10);
        require(clipboard_echoes==0,"Remote clipboard handoff echoed back through the listener");
    }
    shell.update_thumbnail("a",image(80));select(1);
    const auto initial_revision=GetPropW(waiting,L"HiveDesk.SurfaceRevision");
    SendMessageW(shell.viewer_window(),WM_SIZE,SIZE_RESTORED,0);
    require(GetPropW(waiting,L"HiveDesk.SurfaceRevision")==initial_revision,"Synchronous DXGI size notification reentered GPU composition");
    require(selected==1&&shown(waiting)&&title(waiting).find(L"Computer A")!=std::wstring::npos&&title(waiting).find(L"Snapshot")!=std::wstring::npos,"A cached snapshot was not shown immediately");
    select(2);
    require(selected==2&&shown(waiting)&&title(waiting).find(L"Computer B")!=std::wstring::npos&&title(waiting).find(L"Snapshot")==std::wstring::npos,"B reused A's snapshot");
    std::thread late([&]{shell.update_thumbnail("a",image(130));shell.show_remote("a",L"Computer A",1);});late.join();pump(20);
    require(shown(waiting)&&title(waiting).find(L"Computer B")!=std::wstring::npos&&title(waiting).find(L"Snapshot")==std::wstring::npos,"Late A completion changed B's waiting screen");
    shell.update_thumbnail("b",image(180));
    require(title(waiting).find(L"Snapshot")!=std::wstring::npos,"Fresh B snapshot did not update while connecting");
    shell.show_remote("b",L"Computer B",2); // Simulates the prepared-first-frame notification.
    select(3);SendMessageW(owner,WM_TIMER,4,0);
    require(selected==3&&shown(waiting)&&title(waiting).find(L"Computer C")!=std::wstring::npos,"Previous fade timer hid the next machine");
    shell.show_remote("c",L"Computer C",3);pump(260);
    require(!shown(waiting)&&shown(shell.viewer_window()),"First frame did not dismiss the waiting layer");
    shell.show_reconnecting("c");
    require(shown(waiting)&&title(waiting).find(L"Reconnecting")!=std::wstring::npos&&title(waiting).find(L"Computer C")!=std::wstring::npos,"Reconnection reused stale video instead of a waiting screen");
    shell.show_local();shell.show_remote("c",L"Computer C",4);shell.update_thumbnail("c",image(220));pump(260);
    require(!shown(waiting)&&!shown(shell.viewer_window()),"Canceled connection reopened on a late completion");
    // DEVG -> VI -> DEVG -> 6Re: keep the VI renderer lease alive to
    // simulate an old worker/flip surface that has not finished shutting down.
    select(1);auto vi=shell.viewer_surface();
    require(vi&&vi->peer=="a","VI surface binding missing");
    shell.show_remote("a",L"Computer A",10,vi->window);pump(260);
    require(shown(vi->window)&&!shown(waiting),"VI first frame not revealed");
    const auto vi_waiting=waiting;
    select(0);
    require(IsWindow(vi->window)&&!shown(vi->window),"Return to local exposed or destroyed a leased video window");
    select(3);auto six=shell.viewer_surface();
    require(six&&six->peer=="c"&&six->window!=vi->window&&shown(six->window)&&
        IsWindow(vi->window)&&!shown(vi->window)&&shown(waiting)&&waiting!=vi_waiting,
        "6Re reused or exposed VI's retained video/waiting surface");
    SendMessageW(vi->window,WM_CLOSE,0,0);
    require(IsWindow(vi->window)&&!shown(vi->window),"Late close destroyed a renderer's leased surface");
    shell.show_remote("c",L"Computer C",11,vi->window);pump(260);
    require(shown(waiting),"A stale surface completion dismissed the new cover");
    shell.show_remote("c",L"Computer C",12,six->window);pump(260);
    require(!shown(waiting)&&shown(six->window)&&!shown(vi->window),"6Re failed to reveal its own surface");
    select(1);shell.show_remote("a",L"Computer A",10,vi->window);pump(260);
    require(shown(waiting)&&shell.viewer_window()!=vi->window,"Same-peer reselection accepted a previous renderer completion");
    const auto old_vi=vi->window;vi.reset();select(2);
    require(!IsWindow(old_vi)&&IsWindow(six->window)&&!shown(six->window),"Retired video lifetime ignored renderer leases");
    const auto old_six=six->window;six.reset();select(1);
    require(!IsWindow(old_six),"Retired renderer window leaked after lease release");
    shell.show_local();
    // No first frame ever arrives. Reopen the switcher repeatedly while waiting
    // and verify a fresh, correctly sized GPU upload before the panel is shown.
    hive::Thumbnail native;native.width=1920;native.height=1200;
    native.bgra.resize(size_t(native.width)*native.height*4,160);
    shell.update_thumbnail("b",std::move(native));select(2);
    require(ULONG_PTR(GetPropW(waiting,L"HiveDesk.SnapshotWidth"))==1920&&
        ULONG_PTR(GetPropW(waiting,L"HiveDesk.SnapshotHeight"))==1200,"Full-screen snapshot reused the sidebar thumbnail");
    for(unsigned i=0;i<3;++i){
        const auto revision=ULONG_PTR(GetPropW(panel,L"HiveDesk.SurfaceRevision"));
        ValidateRect(panel,nullptr);SendMessageW(owner,WM_HOTKEY,81,0);
        RECT bounds{};GetClientRect(panel,&bounds);
        require(shown(panel)&&ULONG_PTR(GetPropW(panel,L"HiveDesk.SurfaceRevision"))>revision&&
            ULONG_PTR(GetPropW(panel,L"HiveDesk.SurfaceWidth"))==ULONG_PTR(bounds.right)&&
            ULONG_PTR(GetPropW(panel,L"HiveDesk.SurfaceHeight"))==ULONG_PTR(bounds.bottom),
            "Switcher reused a stale/mis-sized surface without a successful video connection");
        SendMessageW(owner,WM_HOTKEY,81,0);pump(260);
    }
    shell.show_local();
    const int tile=(px(200)-2*px(12))*9/16,stride=tile+px(16);
    const auto center=[&](int row){return px(12)+row*stride+tile/2;};
    const auto drag=[&](int row,int x,int y){
        SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(100),center(row)));
        SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,y));
        const auto ghost=find(L"HiveDesk.Drag");RECT ghost_rect{};GetWindowRect(ghost,&ghost_rect);
        require(ghost&&shown(ghost)&&ghost_rect.right-ghost_rect.left==px(200)-2*px(12),"Dragging has no visible snapshot ghost");
        require((GetWindowLongW(ghost,GWL_EXSTYLE)&(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT))==(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT),"Drag ghost intercepts focus/input");
        SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(x,y));
        require(!IsWindow(ghost),"Drop leaked the drag visual");
    };
    SendMessageW(owner,WM_HOTKEY,81,0);
    BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
    if(animations)require(ULONG_PTR(GetPropW(panel,L"HiveDesk.CardAnimationMs"))==300&&ULONG_PTR(GetPropW(panel,L"HiveDesk.AnimatedCards"))==4,"Hotkey cards did not use the bounded stagger animation");
    const auto before_drag=selected;
    drag(3,px(100),center(1)-tile/2); // C before A.
    require(selected==before_drag&&saved_order==std::vector<std::string>({"c","a","b"}),"Reordering selected a computer or saved the wrong order");
    // Discovery returns map/ID order again; the saved visual order must win.
    shell.update_peers({{"a","Computer A","",47980,true},{"b","Computer B","",47980,true},{"c","Computer C","",47980,true}});
    SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(100),center(1)));
    SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(px(100),center(1)));
    require(last_selected=="c","Discovery reset the user's computer order");shell.show_local();
    SendMessageW(owner,WM_HOTKEY,81,0);allow_save=false;drag(1,px(100),center(3)+tile/2);
    require(saved_order==std::vector<std::string>({"c","a","b"}),"Failed persistence changed saved order");allow_save=true;
    // Escape/capture loss must cancel without opening the drop target.
    const auto before_cancel=selected;
    SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(100),center(1)));
    SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(400),center(1)));
    SendMessageW(panel,WM_CANCELMODE,0,0);SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(px(100),center(1)));
    require(selected==before_cancel,"Canceled drag became a click");
    // B has a real 16:10 snapshot, and dropping outside creates a fresh lease.
    const auto old=shell.viewer_surface();drag(3,px(520),px(260));
    const auto floating=shell.viewer_surface();waiting=find(L"HiveDesk.Waiting");
    require(floating&&floating->peer=="b"&&floating->window!=old->window&&shown(waiting),"Drag out failed to open the chosen machine on a fresh surface");
    const auto style=GetWindowLongW(floating->window,GWL_STYLE);
    require((style&WS_THICKFRAME)&&(style&WS_CAPTION)&&(style&WS_MAXIMIZEBOX),"Floating viewer is not a resizable/maximizable framed window");
    RECT client{},outer{};GetClientRect(floating->window,&client);GetWindowRect(floating->window,&outer);
    require(std::abs(double(client.right)/client.bottom-1.6)<.01,"Window did not use the native snapshot aspect");
    RECT work_area{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work_area,0);
    require(client.right>=(work_area.right-work_area.left)*.6&&client.bottom>=(work_area.bottom-work_area.top)*.6,"Floating viewer opened as a tiny window");
    SendMessageW(floating->window,WM_SYSCOMMAND,SC_MAXIMIZE,0);pump(20);
    GetClientRect(floating->window,&client);
    require(IsZoomed(floating->window)&&client.right>=(work_area.right-work_area.left)*.95&&client.bottom>=(work_area.bottom-work_area.top)*.90,"Maximize was blocked or constrained to the source aspect");
    const auto maximized=client;shell.set_remote_size(1280,1024);pump(20);GetClientRect(floating->window,&client);
    require(IsZoomed(floating->window)&&EqualRect(&client,&maximized),"Source resolution change resized a maximized window");
    SendMessageW(floating->window,WM_SYSCOMMAND,SC_RESTORE,0);pump(20);GetClientRect(floating->window,&client);
    require(!IsZoomed(floating->window)&&std::abs(double(client.right)/client.bottom-1.25)<.01,"Restore lost the updated source aspect ratio");
    ShowWindow(floating->window,SW_MAXIMIZE);pump(20);
    require(IsZoomed(floating->window),"Direct native maximize failed");
    ShowWindow(floating->window,SW_RESTORE);pump(20);
    shell.set_remote_size(1080,1920);GetClientRect(floating->window,&client);GetWindowRect(floating->window,&outer);
    require(std::abs(double(client.right)/client.bottom-.5625)<.01,"First frame did not correct the provisional window aspect");
    const LONG fx=outer.right-outer.left-client.right,fy=outer.bottom-outer.top-client.bottom;
    for(UINT edge=WMSZ_LEFT;edge<=WMSZ_BOTTOMRIGHT;++edge){
        RECT proposal=outer;proposal.right+=px(170);proposal.bottom+=px(90);
        const RECT before=proposal;SendMessageW(floating->window,WM_SIZING,edge,reinterpret_cast<LPARAM>(&proposal));
        const double w=proposal.right-proposal.left-fx,h=proposal.bottom-proposal.top-fy;
        require(std::abs(h-w/.5625)<=1,"A resize edge broke the source aspect ratio");
        if(edge==WMSZ_LEFT||edge==WMSZ_TOPLEFT||edge==WMSZ_BOTTOMLEFT)require(proposal.right==before.right,"Left resize moved the opposite edge");
        if(edge==WMSZ_TOP||edge==WMSZ_TOPLEFT||edge==WMSZ_TOPRIGHT)require(proposal.bottom==before.bottom,"Top resize moved the opposite edge");
    }
    SetWindowPos(floating->window,nullptr,px(300),px(140),px(480),px(400),SWP_NOZORDER|SWP_NOACTIVATE);
    GetClientRect(floating->window,&client);require(std::abs(double(client.right)/client.bottom-.5625)<.01,"Programmatic/snap resize bypassed aspect constraint");
    const auto revision=GetPropW(waiting,L"HiveDesk.SurfaceRevision");
    SendMessageW(floating->window,WM_SIZE,SIZE_RESTORED,0);
    require(GetPropW(waiting,L"HiveDesk.SurfaceRevision")==revision,"Synchronous DXGI size notification reentered GPU composition");
    pump(20); // Layout is deliberately deferred out of synchronous DXGI messages.
    POINT origin{};ClientToScreen(floating->window,&origin);RECT cover{};GetWindowRect(waiting,&cover);GetClientRect(floating->window,&client);
    require(cover.left==origin.x&&cover.top==origin.y&&cover.right-cover.left==client.right&&cover.bottom-cover.top==client.bottom,"Waiting snapshot did not follow the floating client area");
    shell.show_remote("b",L"Computer B",30,floating->window);pump(260);
    GetClientRect(floating->window,&client);require(std::abs(double(client.right)/client.bottom-.5625)<.01&&!shown(waiting),"First-frame reveal changed the floating size or left its cover");
    SendMessageW(floating->window,WM_CLOSE,0,0);require(!shown(floating->window),"Closing floating viewer did not return locally");
    SendMessageW(owner,WM_HOTKEY,81,0);SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(100),center(1)));SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(px(100),center(1)));
    require(!(GetWindowLongW(shell.viewer_window(),GWL_STYLE)&WS_THICKFRAME),"Normal click inherited floating mode");shell.show_local();
    return 0;
}
}
int wmain(int argc,wchar_t**){
    if(argc>1){try{return probe();}catch(const std::exception& e){std::fprintf(stderr,"FAIL waiting screen: %s\n",e.what());return 1;}}
    const std::wstring name=L"HiveDeskWaitingTest-"+std::to_wstring(GetCurrentProcessId());
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
    std::printf("%s waiting UI: isolated station, snapshot identity, late replies, first-frame fade, rapid reselection, reconnect and cancel (exit %lu).\n",code?"FAIL":"PASS",code);
    return code?1:0;
}
