#include "hivedesk/desktop_session.h"
#include "capture_sequence.h"
#include "viewer_queue.h"
#include <future>
#include <cstdio>
#include <stdexcept>

void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void stream_restarts(){
    hive::CaptureSequence sequence;hive::ViewerQueue viewer;
    sequence.begin(7);uint64_t now=1000000;
    auto emit=[&](bool idr){
        hive::EncodedFrame frame;frame.session=7;frame.sequence=sequence.next();frame.keyframe=idr;
        frame.capture_time_us=now;frame.data={1};sequence.submitted();
        require(viewer.push(std::move(frame),now+1000),"Restart frame discarded by viewer");
        auto ready=viewer.pop(now+1000);require(ready&&viewer.mark_ready(*ready,now+2000),"Restart frame not presentable");now+=10000;
    };
    for(unsigned desktop=0;desktop<4;++desktop){
        const auto next=sequence.next();sequence.begin(7);require(sequence.next()==next,"Same-stream capture restart reset sequence");
        require(sequence.first(),"Capture restart did not require an IDR");
        for(unsigned timeout=0;timeout<4;++timeout)require(sequence.first(),"Capture timeout consumed first-IDR requirement");
        now+=500000;emit(sequence.first());require(!sequence.first(),"Accepted first submit did not clear IDR requirement");for(unsigned i=0;i<300;++i)emit(false);
        // A frame accepted by NVENC but retired at stop still consumes an ID.
        sequence.submitted();
    }
    hive::EncodedFrame old;old.session=7;old.sequence=0;old.capture_time_us=now;old.keyframe=true;old.data={1};
    require(!viewer.push(std::move(old),now+1000),"Fix weakened duplicate/old packet rejection");
    require(!viewer.recovering(),"Healthy desktop restart left receiver recovering");
    sequence.begin(8);require(sequence.next()==0,"New stream did not start its own sequence");
    sequence.submitted();sequence.begin(0);require(sequence.next()==0,"Legacy stream binding failed");sequence.submitted();sequence.begin(0);
    require(sequence.next()==1,"Legacy capture restart reset sequence");
}
void attach_on_windowless_thread(){
    // No SwitchDesktop, SendInput, visible windows or interaction. The private
    // desktop is only a thread attachment used to exercise actual Win32 APIs.
    auto check=std::async(std::launch::async,[]{
        const auto original=GetThreadDesktop(GetCurrentThreadId());
        const auto name=L"HiveDesk.Test."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
        HDESK private_desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
        require(private_desktop!=nullptr,"Create private test desktop failed");
        struct Cleanup {HDESK original,desktop;~Cleanup(){SetThreadDesktop(original);CloseDesktop(desktop);}} cleanup{original,private_desktop};
        require(SetThreadDesktop(private_desktop)!=FALSE,"Attach private test desktop failed");
        {
            hive::InputDesktop input(hive::DesktopUse::Input);
            require(input.attach(),"Input owner failed to follow active desktop");
            require(!input.attach(),"Stable desktop unnecessarily rebound input");
            require(hive::desktop_name(GetThreadDesktop(GetCurrentThreadId()))==hive::input_desktop_name(),"Input owner stayed on old desktop");
        }
        require(hive::desktop_name(GetThreadDesktop(GetCurrentThreadId()))==name,"Owned desktop handle did not restore original binding");
        // Windows rejects a thread switch while it owns a window. Exercise the
        // error path on the invisible private desktop and preserve that binding.
        HWND window=CreateWindowExW(0,L"STATIC",L"Hidden attachment test",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window!=nullptr,"Create hidden test window failed");bool rejected=false;
        try{hive::InputDesktop input(hive::DesktopUse::Input);input.attach();}catch(const std::system_error& error){rejected=error.code().value()==ERROR_BUSY;}
        DestroyWindow(window);require(rejected,"Busy desktop thread error lost its Windows code");
        require(hive::desktop_name(GetThreadDesktop(GetCurrentThreadId()))==name,"Failed attachment changed desktop binding");
    });
    check.get();
    require((hive::desktop_access(hive::DesktopUse::Input)&DESKTOP_JOURNALPLAYBACK)!=0,"Input attachment missing playback right");
    require((hive::desktop_access(hive::DesktopUse::Capture)&DESKTOP_JOURNALPLAYBACK)==0,"Capture unnecessarily requests input access");
}
int main(){try{stream_restarts();attach_on_windowless_thread();
    std::puts("PASS desktop transitions: 4 same-stream capture epochs accepted, stale packets rejected, independent sessions, native hidden desktop attachment/restore/error code; no locking or injected input.");return 0;
}catch(const std::exception& error){std::printf("FAIL desktop transitions: %s\n",error.what());return 1;}}
