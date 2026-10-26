#include "hivedesk/host_bridge.h"
#include "hivedesk/config.h"
#include <windows.h>
#include <atomic>
#include <stdexcept>

// Explicit installed-broker probe, invoked only by HiveDesk.exe --host-check.
// Hooks consume every test event before it reaches an application. No clipboard,
// password, physical cursor position or display mode is read/written by the test.
namespace hive { namespace {
constexpr ULONG_PTR Marker=0x48495645;
struct Observed {int down=0,up=0,mouse_down=0,mouse_up=0;};
thread_local Observed* observed=nullptr;
LRESULT CALLBACK keyboard(int code,WPARAM message,LPARAM value){
    if(code>=0&&observed){const auto& k=*reinterpret_cast<KBDLLHOOKSTRUCT*>(value);
        if(k.dwExtraInfo==Marker){if(k.scanCode==0x1e){if(message==WM_KEYDOWN)++observed->down;if(message==WM_KEYUP)++observed->up;}return 1;}}
    return CallNextHookEx(nullptr,code,message,value);
}
LRESULT CALLBACK mouse(int code,WPARAM message,LPARAM value){
    if(code>=0&&observed){const auto& m=*reinterpret_cast<MSLLHOOKSTRUCT*>(value);
        if(m.dwExtraInfo==Marker){if(message==WM_LBUTTONDOWN)++observed->mouse_down;if(message==WM_LBUTTONUP)++observed->mouse_up;return 1;}}
    return CallNextHookEx(nullptr,code,message,value);
}
template<class Predicate> void wait(Predicate ready,const char* error){
    const auto deadline=GetTickCount64()+6000;
    do {MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}if(ready())return;Sleep(5);}while(GetTickCount64()<deadline);
    throw std::runtime_error(error);
}
}
int host_check(){
    Observed counts;observed=&counts;
    HHOOK keys=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard,GetModuleHandleW(nullptr),0);
    HHOOK buttons=SetWindowsHookExW(WH_MOUSE_LL,mouse,GetModuleHandleW(nullptr),0);
    int result=1;HostBridge host;std::atomic<int> frames=0;
    try {
        if(!keys||!buttons)throw std::runtime_error("Cannot install event-consuming test hooks");
        std::string error;
        if(!host.start({0,0,60000,123456},[&](EncodedFrame f){if(!f.data.empty())++frames;},error))throw std::runtime_error(error);
        wait([&]{return frames.load()>0;},"No SYSTEM capture frame");
        if(!host.input({InputKind::Key,0x1e,0,0})||!host.input({InputKind::Button,1,0,0}))throw std::runtime_error("Input pipe send failed");
        wait([&]{return counts.down==1&&counts.mouse_down==1;},"SYSTEM key/button down did not arrive at hooks");
        // Closing the local connection must release the two held inputs.
        host.stop();
        wait([&]{return counts.up==1&&counts.mouse_up==1;},"Pipe close did not release held key/button");
        if(!host.start({0,0,60000,123457},[&](EncodedFrame f){if(!f.data.empty())++frames;},error))throw std::runtime_error("Second host connection: "+error);
        if(!host.input({InputKind::Key,0x1e,0,0})||!host.input({InputKind::Button,1,0,0})||!host.input({InputKind::ReleaseAll,0,0,0}))throw std::runtime_error("Explicit release send failed");
        wait([&]{return counts.down==2&&counts.up==2&&counts.mouse_down==2&&counts.mouse_up==2;},"Explicit release did not release held key/button");
        host.stop();log("PASS installed desktop broker: SYSTEM video capture, keyboard/button injection, explicit release, pipe-close release, and second capture session. All test input consumed by hooks; clipboard unchanged.");result=0;
    }catch(const std::exception& e){host.stop();log(std::string("FAIL installed desktop broker: ")+e.what());
        // Keep hooks pumping briefly so any teardown release is consumed too.
        const auto until=GetTickCount64()+300;while(GetTickCount64()<until){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5);}
    }
    if(keys)UnhookWindowsHookEx(keys);if(buttons)UnhookWindowsHookEx(buttons);observed=nullptr;return result;
}
}
