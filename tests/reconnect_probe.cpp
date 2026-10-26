// Interactive installed-agent probe. Captures one local IDR, replays it over
// encrypted loopback, restarts that peer, then tests cancellation. No input is
// injected into any other computer and clipboard content is never retained.
#include "hivedesk/config.h"
#include "hivedesk/media.h"
#include "hivedesk/transport.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
template<class F> void wait_for(F check,const char* error){
    const auto until=std::chrono::steady_clock::now()+12s;
    while(!check()){if(std::chrono::steady_clock::now()>until)throw std::runtime_error(error);std::this_thread::sleep_for(25ms);}
}
int main(){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND owner=FindWindowW(L"HiveDesk.Owner",nullptr),viewer=FindWindowW(L"HiveDesk.Viewer",nullptr),panel=FindWindowW(L"HiveDesk.Switcher",nullptr);
    if(!owner||!viewer||!panel){std::puts("FAIL: start HiveDesk first");return 1;}
    struct ReturnLocal{HWND viewer;~ReturnLocal(){SendMessageW(viewer,WM_CLOSE,0,0);}}cleanup{viewer};
    try {
        hive::EncodedFrame fixture;std::mutex mutex;hive::MediaHost encoder;std::string error;
        if(!encoder.start({30,30},[&](hive::EncodedFrame frame){std::lock_guard lock(mutex);if(fixture.data.empty()&&frame.keyframe)fixture=std::move(frame);},error))throw std::runtime_error(error);
        wait_for([&]{std::lock_guard lock(mutex);return !fixture.data.empty();},"No local encoded IDR");encoder.stop();
        hive::Transport source;const auto config=hive::load_config();std::string agent;
        std::atomic<unsigned> starts=0,handoffs=0;std::atomic<uint64_t> session=0;std::atomic<bool> invalid=false;
        hive::TransportCallbacks callbacks;
        callbacks.clipboard=[&](const std::string&,const std::wstring&){++handoffs;};
        callbacks.start_stream=[&](const std::string& peer,const hive::StreamSettings& settings){
            const auto previous=session.exchange(settings.session);const auto number=++starts;
            if(!settings.session||settings.session==previous||handoffs<number)invalid=true;
            auto frame=fixture;frame.session=settings.session;frame.sequence=1;source.send_frame(peer,frame);
        };
        auto connect=[&]{
            if(!source.start({"!!!!reconnect-probe-"+std::to_string(GetCurrentProcessId()),"Reconnect probe","",0,false,true},callbacks,error)||
               !source.connect("127.0.0.1",config.port,agent,error))throw std::runtime_error(error);
            wait_for([&]{return source.connection_token(agent)!=0;},"Pair not ready");
        };
        auto title=[&]{wchar_t text[256]{};GetWindowTextW(viewer,text,256);return std::wstring(text);};
        auto watching=[&]{return IsWindowVisible(viewer)&&title()==L"HiveDesk - Reconnect probe";};
        connect();std::this_thread::sleep_for(300ms);
        SendMessageW(owner,WM_TIMER,3,0);std::this_thread::sleep_for(300ms);
        const int dpi=GetDpiForWindow(panel);auto px=[&](int n){return MulDiv(n,dpi,96);};const int tile=(px(200)-2*px(12))*9/16;
        SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(px(100),px(12)+tile+px(16)+tile/2));
        wait_for([&]{return starts==1&&watching();},"Initial decoded desktop did not open");
        source.stop();wait_for([&]{return title()==L"HiveDesk - Reconnecting";},"Lost session did not enter reconnect state");
        connect();wait_for([&]{return starts==2&&watching();},"Desktop did not automatically resume after restart");
        if(invalid)throw std::runtime_error("Reconnected session reused ID or missed clipboard-before-start");
        source.stop();wait_for([&]{return title()==L"HiveDesk - Reconnecting";},"Second loss did not enter reconnect state");
        SendMessageW(viewer,WM_CLOSE,0,0);connect();std::this_thread::sleep_for(2500ms);
        if(starts!=2||IsWindowVisible(viewer))throw std::runtime_error("Cancelled reconnect resumed or stole the desktop");
        source.stop();
        std::puts("PASS real App + NVENC/NVDEC + encrypted TCP: decoded first frame, remote restart, automatic video resume with fresh session/clipboard, cancellation prevents restart and focus steal.");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL reconnect integration: %s\n",e.what());return 1;}
}
