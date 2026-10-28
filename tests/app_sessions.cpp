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
    struct Peer {hive::Transport net;std::atomic<uint64_t> session=0,sequence=0;std::atomic<bool> warm=false,peek=false;std::atomic<unsigned> remaining=0;std::string client;std::mutex mutex;bool handoff=false;};
    std::array<Peer,3> peers;std::atomic<unsigned> starts=0,promotions=0,warms=0,previews=0,preview_stops=0,visible_previews=0;std::atomic<bool> bad_handoff=false;
    for(unsigned i=0;i<3;++i){auto& p=peers[i];hive::TransportCallbacks cb;
        cb.clipboard=[&p](const std::string&,const std::wstring&){std::lock_guard lock(p.mutex);p.handoff=true;};
        cb.start_stream=[&p,&starts,&promotions,&warms,&bad_handoff](const std::string& id,const hive::StreamSettings& s){std::lock_guard lock(p.mutex);if(s.warm){++warms;if(p.session==s.session&&p.warm)return;p.handoff=false;p.peek=false;}else{p.peek=false;++starts;if(!std::exchange(p.handoff,false))bad_handoff=true;if(p.warm&&p.session==s.session)++promotions;}p.client=id;if(p.session!=s.session)p.sequence=0;p.session=s.session;p.warm=s.warm;p.remaining=s.warm?1:10000;};
        cb.preview=[&p,&previews,&preview_stops,&bad_handoff](const std::string&,uint64_t session,bool enabled){std::lock_guard lock(p.mutex);if(p.warm&&p.session==session){if(enabled){++previews;if(p.handoff)bad_handoff=true;}else ++preview_stops;p.peek=enabled;p.remaining=enabled?10000:0;}};
        cb.stop_stream=[&p](const std::string&){p.remaining=0;};
        cb.cancel_warm=[&p](const std::string&,uint64_t s){if(p.session==s&&p.warm)p.remaining=0;};
        cb.input_begin=[&p,&bad_handoff](const std::string&,uint64_t){if(p.warm){bad_handoff=true;return false;}return true;};
        cb.input_packet=[&p](const std::string& id,const hive::InputPacket& in){p.net.send_input_ack(id,{in.session,in.sequence,in.capture_time_us,0,0,true});};
        std::string error;require(p.net.start({"peer-"+std::to_string(i),"Generated peer "+std::to_string(i),"stress-key",uint16_t(48190+i),false,true},cb,error),error.c_str());
    }
    hive::AppConfig config;config.device_id="isolated-viewer";config.name=L"Isolated viewer";config.group_key="stress-key";config.port=48194;config.first_run=false;config.desktop.tray_icon=false;
    App app(config);std::atomic<bool> stop=false;
    std::thread producer([&]{while(!stop){for(auto& p:peers){std::lock_guard lock(p.mutex);if(p.remaining&&p.net.frame_ready(p.client)){auto f=encoded;f.session=p.session;f.sequence=p.sequence++;f.capture_time_us=f.source_ready_us=0;if(p.net.send_frame(p.client,f))--p.remaining;}}Sleep(5);}});
    std::exception_ptr driver_failure;
    std::thread driver([&]{try{
        HWND owner=nullptr,panel=nullptr;while(!stop&&!(owner=FindWindowW(L"HiveDesk.Owner",nullptr)))Sleep(5);if(stop)return;
        // Owner is created before the switcher. Do not keep a null HWND for
        // the entire run when GPU/window setup yields between those creates.
        while(!stop&&!(panel=FindWindowW(L"HiveDesk.Switcher",nullptr)))Sleep(5);if(stop)return;Sleep(500);
        SendMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(new std::function<void()>([&]{KillTimer(owner,1);SendMessageW(owner,WM_APP+48,1,0);})));
        for(auto& p:peers){std::string id,error;for(unsigned attempt=0;attempt<30&&!stop;++attempt){if(p.net.connect("127.0.0.1",48194,id,error))break;Sleep(100);}}

        Sleep(150);
        PostMessageW(owner,WM_HOTKEY,81,0);Sleep(1500);
        for(int index=1;index<=2;++index){
            SendMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(new std::function<void()>([&,index]{const auto px=[&](int x){return MulDiv(x,int(GetDpiForWindow(panel)),96);};const int tile=(px(200)-2*px(12))*9/16;SendMessageW(panel,WM_MOUSEMOVE,0,MAKELPARAM(px(100),px(12)+index*(tile+px(16))+tile/2));})));
            Sleep(700);
        }
        // Promote while Hive still owns the hovered warm lease. Closing Hive,
        // waiting and reopening before this assertion can legitimately expire
        // every warm lease and exercise only cold starts in the rapid loop.
        SendMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(new std::function<void()>([&]{
            const auto px=[&](int x){return MulDiv(x,int(GetDpiForWindow(panel)),96);};const int tile=(px(200)-2*px(12))*9/16;
            const auto point=MAKELPARAM(px(100),px(12)+2*(tile+px(16))+tile/2);
            SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(panel,WM_LBUTTONUP,0,point);
        })));
        for(unsigned attempt=0;attempt<200&&!promotions&&!stop;++attempt)Sleep(10);
        for(unsigned n=0;n<24&&!stop;++n){
            PostMessageW(owner,WM_HOTKEY,81,0);Sleep(n%6==0?1500:350);
            auto hover=new std::function<void()>([&,n]{
                const auto px=[&](int x){return MulDiv(x,int(GetDpiForWindow(panel)),96);};const int tile=(px(200)-2*px(12))*9/16;
                SendMessageW(panel,WM_MOUSEMOVE,0,MAKELPARAM(px(100),px(12)+(1+n%3)*(tile+px(16))+tile/2));
            });SendMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(hover));
            Sleep(n%4==0?700:30);if(GetPropW(panel,L"HiveDesk.PeekReady"))++visible_previews;
            if(n%4==0){
                SendMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(new std::function<void()>([&]{SendMessageW(panel,WM_MOUSEMOVE,0,MAKELPARAM(0,0));})));
                Sleep(60);
                require(!GetPropW(panel,L"HiveDesk.PeekReady"),"Pointer leave retained active preview");
            }
            auto task=new std::function<void()>([&,n]{
                const auto px=[&](int x){return MulDiv(x,int(GetDpiForWindow(panel)),96);};const int tile=(px(200)-2*px(12))*9/16;
                const int y=px(12)+(1+n%3)*(tile+px(16))+tile/2;
                SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(100),y));
                if(n%2)SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(600),px(240)));
                SendMessageW(panel,WM_LBUTTONUP,0,n%2?MAKELPARAM(px(600),px(240)):MAKELPARAM(px(100),y));
            });PostMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(task));
            Sleep(n%5==0?600:40);
        }
        // Rapid hover swaps without closing Hive, then demand an actually
        // presented final selection. Counting Start requests alone misses hangs.
        SendMessageW(owner,WM_APP+48,1,0);PostMessageW(owner,WM_HOTKEY,81,0);Sleep(1200);
        for(unsigned n=0;n<72;++n){
            SendMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(new std::function<void()>([&,n]{
                const auto px=[&](int x){return MulDiv(x,int(GetDpiForWindow(panel)),96);};const int tile=(px(200)-2*px(12))*9/16;
                SendMessageW(panel,WM_MOUSEMOVE,0,MAKELPARAM(px(100),px(12)+(1+n%3)*(tile+px(16))+tile/2));
            })));Sleep(20);
        }
        SendMessageW(owner,WM_APP+41,0,reinterpret_cast<LPARAM>(new std::function<void()>([&]{
            const auto px=[&](int x){return MulDiv(x,int(GetDpiForWindow(panel)),96);};const int tile=(px(200)-2*px(12))*9/16;
            const auto point=MAKELPARAM(px(100),px(12)+tile+px(16)+tile/2);
            SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(panel,WM_LBUTTONUP,0,point);
        })));
        const auto await_ready=[&]{const auto until=GetTickCount64()+7000;while(!app.test_ready("peer-0")&&GetTickCount64()<until)Sleep(10);return app.test_ready("peer-0");};
        const bool final_ready=await_ready();if(!final_ready)app.test_report();require(final_ready,"Final selection after rapid Peek never presented a desktop");
        for(unsigned n=0;n<3;++n){
            const auto old=app.test_surface();
            app.test_delay_retirement(2000);app.test_stop_viewer();
            require(await_ready(),"Failed viewer could not recover while old graphics retirement was delayed");
            require(app.test_surface()!=old,"Recovery reused the failed renderer's HWND");
        }
        std::puts("PASS 72 rapid Peek transitions and three same-peer recoveries with blocked retirement; final frames verified");
        auto recovery=app.test_recovery_guards();
        require(recovery.wait_for(std::chrono::seconds(3))==std::future_status::ready&&recovery.get(),"Graphics watchdog interrupted a healthy session or failed to request idle recovery");
        std::puts("PASS aged-retirement watchdog: healthy viewer/controller/preview protected; idle recovery requested (process helper tested separately)");
        Sleep(300);PostMessageW(owner,WM_CLOSE,0,0);
    }catch(...){driver_failure=std::current_exception();if(const auto owner=FindWindowW(L"HiveDesk.Owner",nullptr))PostMessageW(owner,WM_CLOSE,0,0);}
    });
    const int result=app.run(GetModuleHandleW(nullptr),false);stop=true;driver.join();producer.join();for(auto& p:peers)p.net.stop();
    if(driver_failure)std::rethrow_exception(driver_failure);
    std::printf("session counts: starts=%u warm=%u promotions=%u\n",starts.load(),warms.load(),promotions.load());
    require(starts>=8&&warms>=3&&promotions>=1,"Stress did not exercise actual warm promotion and repeated Start");
    std::printf("peek counts: starts=%u stops=%u shown=%u\n",previews.load(),preview_stops.load(),visible_previews.load());
    require(previews>=2&&preview_stops>=1,"Stress did not exercise live preview, reveal and leave");
    require(!bad_handoff,"Preview granted input/clipboard or selection missed handoff");
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

