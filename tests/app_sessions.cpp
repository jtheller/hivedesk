#define HIVEDESK_APP_SESSION_TEST
#include "../src/main.cpp"
#include <cstdio>
#include <future>
#include <utility>
namespace hive {EncodedFrame media_generated_test_frame();}
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int probe(){
    USEROBJECTFLAGS flags{};DWORD bytes=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),"Refuse synthetic events on interactive station");
    const auto profile=std::filesystem::temp_directory_path()/(L"HiveDesk-AppSessions-"+std::to_wstring(GetCurrentProcessId()));std::filesystem::create_directories(profile);SetEnvironmentVariableW(L"LOCALAPPDATA",profile.c_str());
    auto encoded=hive::media_generated_test_frame();
    struct Peer {hive::Transport net;std::atomic<uint64_t> session=0,sequence=0;std::atomic<bool> warm=false;std::atomic<unsigned> remaining=0;std::string client;std::mutex mutex;bool handoff=false;};
    std::array<Peer,3> peers;std::atomic<unsigned> starts=0,promotions=0,warms=0;std::atomic<bool> bad_handoff=false;
    for(unsigned i=0;i<3;++i){auto& p=peers[i];hive::TransportCallbacks cb;
        cb.clipboard=[&p](const std::string&,const std::wstring&){std::lock_guard lock(p.mutex);p.handoff=true;};
        cb.start_stream=[&p,&starts,&promotions,&warms,&bad_handoff](const std::string& id,const hive::StreamSettings& s){std::lock_guard lock(p.mutex);if(s.warm)++warms;else{++starts;if(!std::exchange(p.handoff,false))bad_handoff=true;if(p.warm&&p.session==s.session)++promotions;}p.client=id;if(p.session!=s.session)p.sequence=0;p.session=s.session;p.warm=s.warm;p.remaining=s.warm?1:10000;};
        cb.stop_stream=[&p](const std::string&){p.remaining=0;};
        cb.cancel_warm=[&p](const std::string&,uint64_t s){if(p.session==s&&p.warm)p.remaining=0;};
        cb.input_begin=[](const std::string&,uint64_t){return true;};
        cb.input_packet=[&p](const std::string& id,const hive::InputPacket& in){p.net.send_input_ack(id,{in.session,in.sequence,in.capture_time_us,0,0,true});};
        std::string error;require(p.net.start({"peer-"+std::to_string(i),"Generated peer "+std::to_string(i),"stress-key",uint16_t(48190+i),false,true},cb,error),error.c_str());
    }
    hive::AppConfig config;config.device_id="isolated-viewer";config.name=L"Isolated viewer";config.group_key="stress-key";config.port=48194;config.first_run=false;config.desktop.tray_icon=false;
    App app(config);std::atomic<bool> stop=false;
    std::thread producer([&]{while(!stop){for(auto& p:peers){std::lock_guard lock(p.mutex);if(p.remaining&&p.net.frame_ready(p.client)){auto f=encoded;f.session=p.session;f.sequence=p.sequence++;f.capture_time_us=f.source_ready_us=0;if(p.net.send_frame(p.client,f))--p.remaining;}}Sleep(5);}});
    std::thread driver([&]{
        HWND owner=nullptr,panel=nullptr;while(!stop&&!(owner=FindWindowW(L"HiveDesk.Owner",nullptr)))Sleep(5);if(stop)return;
        panel=FindWindowW(L"HiveDesk.Switcher",nullptr);Sleep(500);
        SendMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(new std::function<void()>([&]{KillTimer(owner,1);SendMessageW(owner,WM_APP+48,1,0);})));
        for(auto& p:peers){std::string id,error;for(unsigned attempt=0;attempt<30&&!stop;++attempt){if(p.net.connect("127.0.0.1",48194,id,error))break;Sleep(100);}}

        Sleep(150);
        for(unsigned n=0;n<24&&!stop;++n){
            PostMessageW(owner,WM_HOTKEY,81,0);Sleep(n%6==0?1500:350);
            auto task=new std::function<void()>([&,n]{
                const auto px=[&](int x){return MulDiv(x,int(GetDpiForWindow(panel)),96);};const int tile=(px(200)-2*px(12))*9/16;
                const int y=px(12)+(1+n%3)*(tile+px(16))+tile/2;
                SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(100),y));
                if(n%2)SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(600),px(240)));
                SendMessageW(panel,WM_LBUTTONUP,0,n%2?MAKELPARAM(px(600),px(240)):MAKELPARAM(px(100),y));
            });PostMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(task));
            Sleep(n%5==0?600:40);
        }
        Sleep(2000);PostMessageW(owner,WM_CLOSE,0,0);
    });
    const int result=app.run(GetModuleHandleW(nullptr),false);stop=true;driver.join();producer.join();for(auto& p:peers)p.net.stop();
    std::printf("session counts: starts=%u warm=%u promotions=%u\n",starts.load(),warms.load(),promotions.load());
    require(starts>=8&&warms>=3&&promotions>=1,"Stress did not exercise actual warm promotion and repeated Start");
    require(!bad_handoff,"A selected stream started before its clipboard handoff");
    std::puts("PASS isolated actual App: three encrypted generated peers, prewarm, rapid selection, windowed drops, cancellation and shutdown");return result;
}
int wmain(int argc,wchar_t**){
    if(argc>1){try{return probe();}catch(const std::exception& e){std::fprintf(stderr,"FAIL shell video: %s\n",e.what());return 1;}}
    const std::wstring name=L"HiveDeskAppSessionsTest-"+std::to_wstring(GetCurrentProcessId());
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
        if(WaitForSingleObject(child.hProcess,60000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&code);
        else{TerminateProcess(child.hProcess,124);WaitForSingleObject(child.hProcess,5000);code=124;}
        CloseHandle(child.hThread);CloseHandle(child.hProcess);
    }
    CloseDesktop(desktop);CloseWindowStation(station);
    std::printf("%s full App sessions: isolated station, async DXGI startup, NVENC/NVDEC, resize and teardown (exit %lu).\n",code?"FAIL":"PASS",code);
    return code?1:0;
}

