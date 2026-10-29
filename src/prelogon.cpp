#include "hivedesk/bootstrap.h"
#include "hivedesk/config.h"
#include "hivedesk/transport.h"
#include "hivedesk/host_bridge.h"
#include "hivedesk/desktop_aux.h"
#include "hivedesk/desktop_session.h"
#include "hivedesk/session_control.h"
#include "hivedesk/update.h"
#include <atomic>
#include <mutex>

namespace hive {
// Deliberately no Shell, clipboard access or viewer. The existing SYSTEM broker
// still owns capture, desktop transitions and independent input injection.
template<class Host=HostBridge> class Prelogon {
    Transport transport_;Host host_;
    std::mutex lifecycle_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::shared_ptr<HostControl>> current_;
    void stop_host(){
        auto old=current_.exchange(nullptr);if(old){old->revoke();transport_.end_stream(old->peer,old->session);}
        host_.stop();
    }
    bool current(const std::shared_ptr<HostControl>& lease)const{return !stopping_&&lease&&!lease->revoked&&current_.load()==lease;}
public:
    bool start(const bootstrap::Identity& identity,std::string& error,uint16_t port=47980,bool discovery=true){
        TransportCallbacks cb;
        cb.status=[](const std::string& text){update::log("Prelogon: "+text);};
        cb.peer=[this](const Peer& peer){if(!peer.authenticated){std::lock_guard lock(lifecycle_);auto c=current_.load();if(c&&c->peer==peer.id)stop_host();}};
        cb.start_stream=[this](const std::string& peer,const StreamSettings& settings){
            std::lock_guard lock(lifecycle_);if(stopping_)return;
            auto c=current_.load();
            if(update::pending()&&(!c||c->peer!=peer||c->session!=settings.session)){transport_.end_stream(peer,settings.session);return;}
            if(c&&c->peer==peer&&c->session==settings.session&&current(c)){
                if(c->warming){
                    c->warm_until=GetTickCount64()+10000;
                    if(!settings.warm){c->warming=false;c->previewing=false;if(!host_.activate())stop_host();}
                }
                return;
            }
            if(settings.warm&&c){transport_.end_stream(peer,settings.session);return;}
            stop_host();
            c=std::make_shared<HostControl>(peer,settings.session,transport_.connection_token(peer));
            c->warming=settings.warm;c->warm_until=GetTickCount64()+10000;current_=c;
            std::string failure;
            if(!host_.start(settings,[this,c](EncodedFrame frame){if(current(c)&&!transport_.send_frame(c->peer,frame))host_.request_keyframe();},failure,
                [this,c]{return current(c)&&transport_.frame_ready(c->peer);},
                [this,c](const StreamHealth& h){if(current(c))transport_.send_health(c->peer,h);},
                [this,c](const SourceDiagnostics& d){if(current(c))transport_.send_diagnostics(c->peer,d);},
                [this,c](const InputAck& ack){if(current(c)&&ack.session==c->session)transport_.send_input_ack(c->peer,ack);})){
                update::log("Prelogon capture: "+failure);stop_host();
            }
        };
        cb.stop_stream=[this](const std::string& peer){std::lock_guard lock(lifecycle_);auto c=current_.load();if(c&&c->peer==peer)stop_host();};
        cb.cancel_warm=[this](const std::string& peer,uint64_t session){std::lock_guard lock(lifecycle_);auto c=current_.load();if(c&&c->warming&&c->peer==peer&&c->session==session)stop_host();};
        cb.preview=[this](const std::string& peer,uint64_t session,bool enabled){std::lock_guard lock(lifecycle_);auto c=current_.load();if(current(c)&&c->warming&&c->peer==peer&&c->session==session){c->previewing=enabled;c->warm_until=GetTickCount64()+10000;if(!host_.preview(enabled))stop_host();}};
        cb.input_begin=[this](const std::string& peer,uint64_t session){std::lock_guard lock(lifecycle_);auto c=current_.load();if(!current(c)||c->warming||c->peer!=peer||c->session!=session)return false;c->input_enabled=true;return true;};
        cb.return_request=[this](const std::string& peer,const ReturnRequest& request){
            const auto c=current_.load();
            const bool authorized=current(c)&&!c->warming&&c->input_enabled&&c->peer==peer&&c->session==request.session&&c->connection==transport_.connection_token(peer);
            // There is no outbound viewer before logon, but advertise/answer
            // the same protocol so escape never waits for a nonexistent Shell.
            transport_.send_return_reply(peer,{request.session,request.id,authorized?ReturnResult::Leaf:ReturnResult::Stale});
        };
        cb.input_packet=[this](const std::string& peer,const InputPacket& packet){
            auto c=current_.load();if(!current(c)||c->warming||!c->input_enabled||c->peer!=peer||c->session!=packet.session){transport_.end_stream(peer,packet.session);return;}
            if(!host_.input(packet)){InputAck ack;ack.session=packet.session;ack.sequence=packet.sequence;ack.capture_time_us=packet.capture_time_us;transport_.send_input_ack(peer,ack);}
        };
        cb.input=[this](const std::string& peer,const InputEvent& event){auto c=current_.load();if(current(c)&&!c->warming&&!c->input_enabled&&c->peer==peer)host_.input(event);};
        cb.feedback=[this](const std::string& peer,const StreamFeedback& feedback){auto c=current_.load();if(current(c)&&c->peer==peer&&c->session==feedback.session)host_.feedback(feedback);};
        cb.frame_ready=[this](const std::string& peer){auto c=current_.load();if(current(c)&&c->peer==peer)host_.notify_ready();};
        cb.request_keyframe=[this](const std::string& peer){auto c=current_.load();if(current(c)&&c->peer==peer)host_.request_keyframe();};
        cb.snapshot_request=[this](const std::string& peer,uint64_t request,bool native){
            if(stopping_)return;
            // The callback owns a transport reader thread, never a UI thread.
            try{InputDesktop desktop;desktop.attach();transport_.send_snapshot(peer,capture_snapshot(request,native));}catch(const std::exception& e){update::log(std::string("Prelogon snapshot: ")+e.what());}
        };
        return transport_.start({narrow(identity.id),narrow(identity.name),"",port,discovery,true},std::move(cb),error);
    }
    bool update_idle(){
        std::lock_guard lock(lifecycle_);auto c=current_.load();
        if(c&&c->warming&&GetTickCount64()>c->warm_until){stop_host();c=nullptr;}
        if(!update::pending()||!bootstrap::prelogon_idle(bool(c),c&&c->warming,c&&c->previewing))return false;
        stopping_=true;return true;
    }
    ~Prelogon(){
        stopping_=true;if(auto c=current_.load())c->revoke();
        // Release the listening port before joining GPU/IPC cleanup so the
        // logged-in tray can take over immediately.
        transport_.stop();std::lock_guard lock(lifecycle_);stop_host();
    }
};
#ifndef HIVEDESK_PRELOGON_TEST
int prelogon_main(DWORD parent){
    if(!update::is_system()||!parent)return ERROR_ACCESS_DENIED;
    HANDLE service=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent);
    if(!service)return ERROR_ACCESS_DENIED;
    wchar_t path[32768]{};DWORD length=32768;
    const auto expected=update::module_directory()/L"HiveDeskService.exe";
    if(!QueryFullProcessImageNameW(service,0,path,&length)||_wcsicmp(path,expected.c_str())){CloseHandle(service);return ERROR_ACCESS_DENIED;}
    DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);
    HANDLE stop=OpenEventW(SYNCHRONIZE|READ_CONTROL,FALSE,bootstrap::stop_event(session).c_str());
    if(!session||!stop||!update::system_owned(stop)){if(stop)CloseHandle(stop);CloseHandle(service);return ERROR_ACCESS_DENIED;}
    struct Close{HANDLE a,b;~Close(){CloseHandle(a);CloseHandle(b);}}handles{service,stop};
    update::StartupGuard guard;if(!guard.allowed())return 0;
    const auto identity=bootstrap::load();if(identity.id.empty()||!identity.enabled)return 0;
    Prelogon<> endpoint;std::string error;
    if(!endpoint.start(identity,error)){update::log("Prelogon startup: "+error);return 1;}
    guard.release();update::log("Prelogon network endpoint ready (no signed-in user).");
    HANDLE waits[]{service,stop};
    while(WaitForMultipleObjects(2,waits,FALSE,100)==WAIT_TIMEOUT)if(endpoint.update_idle())break;
    update::log("Prelogon endpoint handing off or stopping.");return 0;
}
#endif
}
