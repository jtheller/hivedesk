#include "hivedesk/config.h"
#include "hivedesk/media.h"
#include "hivedesk/shell.h"
#include "hivedesk/transport.h"
#include "hivedesk/desktop_aux.h"
#include "hivedesk/adaptive.h"
#include "hivedesk/host_bridge.h"
#include "hivedesk/desktop_session.h"
#include "hivedesk/telemetry.h"
#include "hivedesk/reconnect.h"
#include "hivedesk/session_control.h"
#include "hivedesk/update.h"
#include "hivedesk/gpu_policy_restart.h"
#include "hivedesk/recovery.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace hive {int host_check();int warm_broker_check();}
namespace {
using MonitorSide=hive::Telemetry::Side;
class App {
public:
    explicit App(hive::AppConfig config): config_(std::move(config)) {
        telemetry_.enabled(config_.desktop.debug_overlay);
        video_restart_pending_=hive::gpu_policy_restart::pending_request(video_restart_marker_);
    }
    ~App() { shutdown(); }
    int run(HINSTANCE instance,bool open_settings=false) {
        hive::ShellCallbacks callbacks;
        callbacks.select_peer=[this](const std::string& peer) {
            const std::lock_guard lock(view_intent_mutex_);
            if(updating_)return;
            const auto epoch=++generation_;
            hive::log("Viewer selection queued: "+peer+" generation="+std::to_string(epoch));
            request_snapshot(peer);
            enqueue([this,peer,epoch]{
                if(epoch!=generation_)return;
                go_local(active_.exchange(nullptr),false);
                reconnect_.select(peer,epoch);retry_tick();
            });
        };
        callbacks.return_local=[this] {
            ++generation_;
            view_ready_=false;
            const auto previous=active_.exchange(nullptr);
            enqueue([this,previous]{go_local(previous,false);});
        };
        callbacks.kick_controller=[this] {
            const auto target=hosting_.load();
            if(!target||closing_||!target->revoke())return;
            // Input/video callbacks reject this lease immediately. Cleanup has
            // its own worker so an outbound first-frame wait cannot delay kick.
            {const std::lock_guard lock(kick_mutex_);kick_pending_=target;}
            kick_wake_.notify_one();
        };
        callbacks.visible_peers=[this](const std::vector<std::shared_ptr<const hive::ViewerSurface>>& surfaces){
            {const std::lock_guard lock(warm_mutex_);warm_visible_=surfaces;}
            wake_.notify_one();
        };
        callbacks.panel_opened=[this] {
            for(const auto& peer:transport_.peers())if(peer.features&hive::FeatureDesktopAux) {
                request_snapshot(peer.id);
            }
        };
        callbacks.cursor_changed=[this](const std::string& peer,const hive::CursorState& cursor) {
            const auto target=hosting_.load();
            return !closing_&&target&&!target->revoked&&target->peer==peer&&transport_.send_cursor(peer,cursor);
        };
        callbacks.stream_tick=[this] {
            if(!closing_&&clipboard_pending_&&GetTickCount64()-last_clipboard_retry_>=1000) {
                last_clipboard_retry_=GetTickCount64();enqueue([this]{
                    const std::lock_guard lock(host_mutex_);const auto target=hosting_.load();
                    if(target&&!target->revoked&&clipboard_pending_)apply_handoff(target->peer);
                });
            }
            const auto peer=active_.load();if(!peer||closing_)return;
            const auto report=viewer_.load()->feedback();if(report.session&&report.session==active_session_)transport_.send_feedback(*peer,report);
            if(shell_.debug_enabled()&&GetTickCount64()-last_debug_tick_>=250){
                last_debug_tick_=GetTickCount64();const auto size=remote_size_.load();
                const auto stats=viewer_.load()->diagnostics();const auto input=transport_.input_diagnostics(*peer);
                telemetry_.viewer(*peer,stats,uint32_t(size>>32),uint32_t(size));
                telemetry_.input(*peer,active_session_,input);
            }
        };
        callbacks.debug_tick=[this] {
            const auto peer=active_.load();if(!peer||closing_)return;
            const auto stats=viewer_.load()->diagnostics();shell_.update_viewer_diagnostics(*peer,stats);
            const auto input=transport_.input_diagnostics(*peer);shell_.update_input_diagnostics(*peer,input);
        };
        callbacks.captured_input=[this](const std::string& peer,uint64_t session,const hive::InputEvent& input,uint64_t captured_us) {
            // Bind captured events to their original selection. A queued event
            // must never pick up the next computer (or its new same-peer lease).
            const std::lock_guard lock(view_intent_mutex_);
            const auto target=view_control_;
            if(!target||target->peer!=peer||target->session!=session||
                target->generation!=generation_||!view_ready_||closing_)return;
            if(active_dedicated_input_){
                hive::InputPacket packet;packet.session=session;packet.sequence=++input_sequence_;
                packet.capture_time_us=captured_us;packet.event=input;
                transport_.send_input(peer,packet);
            }else transport_.send_input(peer,input);
        };
        callbacks.clipboard_changed=[this](const std::wstring& text) {
            const auto viewing=active_.load();
            const auto hosting=hosting_.load();
            if(viewing) transport_.send_clipboard(*viewing,text);
            else if(hosting&&!hosting->revoked&&!hosting->warming) transport_.send_clipboard(hosting->peer,text);
        };
        callbacks.save_preferences=[this](const std::wstring& name,const hive::DesktopPreferences& desktop) {
            const std::lock_guard lock(config_mutex_);
            auto next=config_;next.name=name;next.desktop=desktop;
            try {hive::set_startup(desktop.startup);hive::save_config(next);}
            catch(const std::exception& e){try{hive::set_startup(config_.desktop.startup);}catch(...){}status(e.what());return false;}
            config_=std::move(next);telemetry_.enabled(desktop.debug_overlay);transport_.rename(hive::narrow(name));return true;
        };
        callbacks.restart_video=[this] {
            {
                const std::lock_guard lock(queue_mutex_);
                if(closing_||updating_||!hive::gpu_policy_restart::save_request(video_restart_marker_))return false;
                queue_.push_back([this]{
                    video_restart_pending_=true;
                    status("GPU policy saved. Video processes will restart when all remote sessions and Settings are closed.");
                });
            }
            wake_.notify_one();return true;
        };
        callbacks.restart=[this] {
            // This callback runs directly on the tray thread. Do not acquire
            // session/queue locks or release input before arming the helper:
            // any of those paths may be precisely what needs recovery.
            try {
                hive::gpu_policy_restart::Relaunch relaunch(false,true);
                if(!relaunch.commit())throw std::runtime_error("Could not start restart helper");
                return true; // Helper posts close and enforces its own deadline.
            } catch(...) {return false;}
        };
        callbacks.save_order=[this](const std::vector<std::string>& order){
            const std::lock_guard lock(config_mutex_);auto next=config_;next.device_order=order;
            try{hive::save_config(next);}catch(const std::exception& e){status(e.what());return false;}
            config_=std::move(next);return true;
        };
        std::string error;
        if(!shell_.create(instance,{config_.name,config_.group_key,config_.stream,config_.desktop,config_.device_order},std::move(callbacks),error))throw std::runtime_error(error);
        const bool initially_show_settings=(config_.first_run||open_settings)&&!video_restart_pending_;
        kick_worker_=std::thread([this]{kick_work();});
        worker_=std::thread([this]{work();});
        enqueue([this]{start_network();});
        // A persisted request must finish its idle restart before reopening
        // Settings; otherwise the Settings admission barrier would block it.
        if(initially_show_settings)shell_.show_settings();
        const int result=shell_.run();
        shutdown();
        return result;
    }
private:
    void apply_handoff(const std::string& peer) {
        const auto cached=staged_clipboards_.find(peer);if(cached==staged_clipboards_.end())return;
        // The secure desktop has no user clipboard. Keep its handoff in memory
        // and apply it on return to Default, without blocking login input.
        input_ready_=true;clipboard_pending_=true;
        if(hive::input_desktop_name()!=L"Default")return;
        try {shell_.apply_clipboard(cached->second);staged_clipboards_.erase(cached);clipboard_pending_=false;}
        catch(const std::exception&) { /* Retry on the next host tick. */ }
    }
    void status(const std::string& text) { hive::log(text); if(!closing_)shell_.set_status(hive::widen(text)); }
    void connection_error(const std::string& text) { status(text); if(!closing_)shell_.show_error(hive::widen(text)); }
    bool enqueue(std::function<void()> job) {
        {const std::lock_guard lock(queue_mutex_);if(closing_||updating_)return false;queue_.push_back(std::move(job));}
        wake_.notify_one();return true;
    }
    void work() {
        SetThreadDescription(GetCurrentThread(),L"HiveDesk commands / viewer lifecycle");
        for(;;) {
            std::function<void()> job;
            {std::unique_lock lock(queue_mutex_);wake_.wait_for(lock,std::chrono::milliseconds(250),[this]{return closing_||!queue_.empty();});if(closing_)break;if(!queue_.empty()){job=std::move(queue_.front());queue_.pop_front();}}
            try {if(job)job();retry_tick();warm_tick();update_tick();} catch(const std::exception& error) {status(error.what());}
        }
        // Keep DXGI destruction and socket/thread joins off the HWND-owning thread.
        finish_shutdown();
    }
    void kick_work() {
        for(;;) {
            std::shared_ptr<hive::HostControl> target;
            {std::unique_lock lock(kick_mutex_);kick_wake_.wait_for(lock,std::chrono::milliseconds(250),[this]{return closing_||kick_pending_;});
                if(closing_)return;if(!kick_pending_)continue;target=std::exchange(kick_pending_,nullptr);}
            // Notify before joining: slow driver teardown must not let the
            // viewer's automatic recovery create a new intent in the meantime.
            const bool notified=transport_.end_stream(target->peer,target->session);
            {
                const std::lock_guard lock(host_mutex_);
                if(hosting_.load()!=target)continue;
                if(!notified)legacy_kicked_[target->peer]=target->connection;
                input_ready_=false;clipboard_pending_=false;hosting_.store(nullptr);
                shell_.observe_cursor({});staged_clipboards_.erase(target->peer);
                host_.stop(); // Closing its input pipe releases held input.
                telemetry_.clear(MonitorSide::Host);hive::release_injected_input();
            }
            // Remove any last frame whose callback raced with revocation.
            transport_.end_stream(target->peer,target->session);
            status(notified?"Remote controller disconnected by local shortcut.":
                "Remote controller disconnected. Update both computers for kick support; this legacy connection is blocked.");
        }
    }
    void warm_tick() {
        if(closing_||updating_)return;
        const auto now=GetTickCount64();
        // Leases also expire when the requesting process disappears without Stop.
        if(const auto host=hosting_.load();host&&host->warming&&now>host->warm_until){
            const std::lock_guard lock(host_mutex_);
            if(hosting_.load()==host&&host->warming&&now>host->warm_until){
                host->revoke();hosting_=nullptr;host_.stop();transport_.end_stream(host->peer,host->session);
            }
        }
        std::vector<std::shared_ptr<const hive::ViewerSurface>> visible;
        std::vector<std::shared_ptr<WarmView>> expired;
        {
            const std::lock_guard lock(warm_mutex_);visible=warm_visible_;
            for(auto it=warm_views_.begin();it!=warm_views_.end();){
                const bool wanted=std::any_of(visible.begin(),visible.end(),[&](const auto& surface){return surface==it->second->surface;});
                if(wanted)it->second->last_visible=now;
                if(now-it->second->last_visible>2000){expired.push_back(it->second);it=warm_views_.erase(it);}else ++it;
            }
        }
        for(const auto& warm:expired){
            transport_.cancel_warm(warm->surface->peer,warm->settings.session);
            hive::log("Warm graphics retiring: "+warm->surface->peer+" session="+std::to_string(warm->settings.session));
            warm->viewer->close();hive::log("Warm graphics retired: "+warm->surface->peer);
        }
        for(const auto& surface:visible){
            {const std::lock_guard lock(queue_mutex_);if(closing_||!queue_.empty())break;}
            if(const auto active=active_.load();active&&*active==surface->peer)continue;
            const auto token=transport_.connection_token(surface->peer);if(!token)continue;
            std::shared_ptr<WarmView> warm;
            {const std::lock_guard lock(warm_mutex_);auto found=warm_views_.find(surface->peer);if(found!=warm_views_.end())warm=found->second;}
            if(warm){
                if(warm->connection!=token||warm->surface!=surface)warm->failed=true;
                if(!warm->failed&&now-warm->renewed>=2000){
                    transport_.request_stream(surface->peer,warm->settings);warm->renewed=now;
                    const auto feedback=warm->viewer->feedback();if(feedback.session)transport_.send_feedback(surface->peer,feedback);
                }
                continue;
            }
            warm=std::make_shared<WarmView>();warm->surface=surface;warm->connection=token;
            warm->last_visible=warm->renewed=now;warm->settings.session=++session_sequence_;
            warm->settings.receiver_millihz=hive::primary_refresh_millihz();warm->settings.warm=true;
            std::string error;
            if(!warm->viewer->open(surface->window,error,[this,peer=surface->peer,session=warm->settings.session](const hive::CursorPresentation& cursor){
                const auto active=active_.load();if(!closing_&&active&&*active==peer&&active_session_==session)
                    shell_.update_cursor_composition(peer,cursor.session,cursor.sequence,cursor.composition);
            },true)){
                warm->failed=true;hive::log("Warm viewer unavailable: "+surface->peer+" "+error);
            }
            {const std::lock_guard lock(warm_mutex_);warm_views_[surface->peer]=warm;}
            if(!warm->failed){warm->failed=!transport_.request_stream(surface->peer,warm->settings);hive::log("Warm viewer requested: "+surface->peer+" session="+std::to_string(warm->settings.session));}
        }
    }
    void update_tick() {
        if(updating_||closing_||GetTickCount64()-last_update_tick_<1000)return;
        last_update_tick_=GetTickCount64();
        const bool installed=hive::update::is_installed_agent();
        const bool update_pending=installed&&hive::update::pending();
        if(!update_pending&&!video_restart_pending_)return;
        {
            // Serialize the final idle decision with inbound Start and local
            // selection admission. The command worker also owns reconnect intent.
            const std::scoped_lock lock(host_mutex_,view_intent_mutex_);
            const std::lock_guard queue_lock(queue_mutex_);
            if(!hive::update::idle_for_update(bool(active_.load()),bool(hosting_.load()),reconnect_.current(generation_),!queue_.empty()))return;
            if(!update_pending){
                if(!shell_.prepare_video_restart())return;
                try {
                    HANDLE broker=installed?hive::gpu_policy_restart::open_broker_event():nullptr;
                    struct Close {HANDLE value;~Close(){if(value)CloseHandle(value);}}close{broker};
                    if(installed&&!broker)throw std::runtime_error("Video restart waiting for the desktop broker.");
                    hive::gpu_policy_restart::Relaunch relaunch;
                    if(broker&&!hive::gpu_policy_restart::signal_broker(broker))throw std::runtime_error("Could not request idle broker restart.");
                    if(!hive::gpu_policy_restart::clear_request(video_restart_marker_))throw std::runtime_error("Could not complete pending video restart; HiveDesk is still running.");
                    if(!relaunch.commit()){
                        hive::gpu_policy_restart::save_request(video_restart_marker_);
                        throw std::runtime_error("Could not launch replacement HiveDesk; current process is still running.");
                    }
                }catch(...){shell_.cancel_video_restart();throw;}
            }else hive::gpu_policy_restart::clear_request(video_restart_marker_);
            updating_=true;
        }
        hive::log(update_pending?"Automatic update: idle; stopping discovery and exiting for installation.":
            "GPU policy: idle; restarting video processes and reopening Settings.");
        transport_.stop();shell_.quit();
    }
    void start_network() {
        const std::lock_guard config_lock(config_mutex_);
        hive::TransportCallbacks callbacks;
        callbacks.peer=[this](const hive::Peer& peer) {if(!closing_){
            if(!peer.authenticated){
                telemetry_.disconnected(peer.id);
                const auto viewing=active_.load();if(viewing&&*viewing==peer.id)view_ready_=false;
                // Do not leave injected keys or a capture worker alive after its
                // controller disappears. A newer session is protected by its token.
                const std::lock_guard lock(host_mutex_);const auto target=hosting_.load();
                const auto token=transport_.connection_token(peer.id);
                if(target&&target->peer==peer.id&&(!token||target->connection!=token)){
                    target->revoke();
                    input_ready_=false;clipboard_pending_=false;hosting_=nullptr;
                    shell_.observe_cursor({});staged_clipboards_.erase(peer.id);
                    host_.stop();telemetry_.clear(MonitorSide::Host);hive::release_injected_input();
                }
            }
            shell_.update_peers(transport_.peers());wake_.notify_one();
        }};
        callbacks.status=[this](const std::string& text){status(text);};
        callbacks.frame_ready=[this](const std::string& peer){
            const auto target=hosting_.load();
            if(!closing_&&target&&!target->revoked&&target->peer==peer)host_.notify_ready();
        };
        callbacks.request_keyframe=[this](const std::string& peer){
            const auto target=hosting_.load();
            if(!closing_&&target&&!target->revoked&&target->peer==peer)host_.request_keyframe();
        };
        callbacks.snapshot_request=[this](const std::string& peer,uint64_t request,bool native) {
            // One pending capture per peer; never block its keyboard/control reader.
            {const std::lock_guard lock(snapshot_mutex_);
                auto [pending,inserted]=snapshot_pending_.try_emplace(peer,request);
                if(!inserted){pending->second=request;return;}}
            enqueue([this,peer,request,native] {
                bool released=false;
                try {
                    const auto now=std::chrono::steady_clock::now();
                    auto& snapshot_cache_=snapshot_caches_[native?1:0];auto& snapshot_time_=snapshot_times_[native?1:0];
                    if(snapshot_cache_.jpeg.empty()||now-snapshot_time_>std::chrono::milliseconds(250)) {
                        snapshot_cache_=hive::capture_snapshot(request,native);snapshot_time_=now;
                    }
                    auto snapshot=snapshot_cache_;
                    {const std::lock_guard lock(snapshot_mutex_);snapshot.request=snapshot_pending_.at(peer);snapshot_pending_.erase(peer);released=true;}
                    transport_.send_snapshot(peer,snapshot);
                }catch(const std::exception& e){
                    hive::log(std::string("Snapshot failed: ")+e.what());
                    if(!released){const std::lock_guard lock(snapshot_mutex_);snapshot_pending_.erase(peer);}
                }
            });
        };
        callbacks.snapshot=[this](const std::string& peer,hive::Snapshot snapshot) {
            {
                const std::lock_guard lock(snapshot_reply_mutex_);
                const auto request=snapshot_requests_.find(peer);
                if(request==snapshot_requests_.end()||request->second!=snapshot.request)return;
                snapshot_requests_.erase(request);
            }
            // Startup may wait eight seconds on the command worker. Snapshot
            // decoding and UI publication must continue throughout that wait.
            shell_.update_snapshot(peer,std::move(snapshot));
        };
        callbacks.cursor=[this](const std::string& peer,hive::CursorState cursor) {
            const auto target=active_.load();if(!closing_&&target&&*target==peer)shell_.update_cursor(peer,std::move(cursor));
        };
        callbacks.feedback=[this](const std::string& peer,const hive::StreamFeedback& report) {
            const auto target=hosting_.load();if(!closing_&&target&&!target->revoked&&target->peer==peer&&target->session==report.session)host_.feedback(report);
        };
        callbacks.host_ended=[this](const std::string& peer,uint64_t session) {
            {const std::lock_guard lock(warm_mutex_);const auto found=warm_views_.find(peer);
                if(found!=warm_views_.end()&&found->second->settings.session==session){found->second->failed=true;return;}}
            uint64_t cancelled=0;
            {
                const std::lock_guard lock(view_intent_mutex_);
                const auto target=view_control_;
                if(closing_||!target||!target->cancel(peer,session,generation_))return;
                view_ready_=false;
                // Break select's first-frame wait immediately. Publishing a new
                // same-peer session is serialized with this identity check.
                const auto previous=active_.exchange(nullptr);
                if(previous){auto expected=previous;last_controlled_.compare_exchange_strong(expected,nullptr);}
                cancelled=target->generation+1;view_control_.reset();
            }
            enqueue([this,cancelled]{
                if(generation_!=cancelled)return;
                reconnect_={};go_local(active_.exchange(nullptr),true,false);
                status("The other computer ended this remote session. Select it again to reconnect.");
            });
        };
        callbacks.health=[this](const std::string& peer,const hive::StreamHealth& health) {
            const auto target=active_.load();if(!closing_&&target&&*target==peer&&health.session==active_session_){telemetry_.health(MonitorSide::View,peer,health);shell_.update_health(peer,health);}
        };
        callbacks.diagnostics=[this](const std::string& peer,const hive::SourceDiagnostics& value){
            const auto target=active_.load();if(!closing_&&target&&*target==peer&&value.session==active_session_){telemetry_.source(MonitorSide::View,peer,value);shell_.update_source_diagnostics(peer,value);}
        };
        callbacks.start_stream=[this](const std::string& peer,const hive::StreamSettings& settings) {
            if(closing_)return;
            const std::lock_guard lock(host_mutex_);
            if(closing_)return;
            if(updating_){transport_.end_stream(peer,settings.session);return;}
            const auto old=hosting_.load();
            if(settings.warm){
                if(old){
                    if(!old->revoked&&old->peer==peer&&old->session==settings.session&&old->connection==transport_.connection_token(peer)){
                        // A late renewal must never downgrade/end a promoted session.
                        if(old->warming)old->warm_until=GetTickCount64()+10000;
                    }
                    else transport_.end_stream(peer,settings.session);
                    return;
                }
            }else if(old&&old->warming&&!old->revoked&&old->peer==peer&&old->session==settings.session){
                old->warming=false;apply_handoff(peer);
                if(!host_.activate()){old->revoke();hosting_=nullptr;host_.stop();transport_.end_stream(peer,settings.session);return;}
                std::string name=peer;for(const auto& item:transport_.peers())if(item.id==peer)name=item.name;
                telemetry_.select(MonitorSide::Host,peer,settings.session,name);shell_.observe_cursor(peer);
                hive::log("Warm host promoted: "+peer+" session="+std::to_string(settings.session));return;
            }
            // A pending local kick also rejects an automatic retry that arrives
            // before its termination notice, rather than replacing the lease.
            if(old&&old->revoked){transport_.end_stream(peer,settings.session);return;}
            const auto connection=transport_.connection_token(peer);
            const auto blocked=legacy_kicked_.find(peer);
            if(blocked!=legacy_kicked_.end()){
                if(blocked->second==connection)return;
                legacy_kicked_.erase(blocked);
            }
            const auto control=std::make_shared<hive::HostControl>(peer,settings.session,connection);
            control->warming=settings.warm;control->warm_until=GetTickCount64()+10000;
            if(old){old->revoke();if(old->warming)transport_.end_stream(old->peer,old->session);}hosting_.store(control);
            host_.stop(); shell_.observe_cursor({});hive::release_injected_input();
            input_ready_=false;clipboard_pending_=false;input_error_reported_=false;
            if(control->revoked)return;
            std::string peer_name=peer;for(const auto& item:transport_.peers())if(item.id==peer)peer_name=item.name;
            if(!settings.warm)telemetry_.select(MonitorSide::Host,peer,settings.session,peer_name);
            const auto clipboard=staged_clipboards_.find(peer);
            if(!settings.warm&&clipboard!=staged_clipboards_.end()) {
                apply_handoff(peer);
            } else if(!settings.warm)status("No clipboard handoff for this controller; input paused.");
            std::string error;
            if(!host_.start(settings,[this,control](hive::EncodedFrame frame) {
                if(!closing_&&!control->revoked&&hosting_.load()==control){telemetry_.size(MonitorSide::Host,control->peer,frame.session,frame.width,frame.height);if(!transport_.send_frame(control->peer,frame))host_.request_keyframe();}
            },error,[this,control] {
                return !closing_&&!control->revoked&&hosting_.load()==control&&transport_.frame_ready(control->peer);
            },[this,control](const hive::StreamHealth& health) {
                if(!closing_&&!control->revoked&&hosting_.load()==control){telemetry_.health(MonitorSide::Host,control->peer,health);transport_.send_health(control->peer,health);}
            },[this,control](const hive::SourceDiagnostics& value){
                if(!closing_&&!control->revoked&&hosting_.load()==control){telemetry_.source(MonitorSide::Host,control->peer,value);telemetry_.video_send(control->peer,control->session,transport_.video_send_diagnostics(control->peer));transport_.send_diagnostics(control->peer,value);}
            },[this,control](const hive::InputAck& ack){
                if(!closing_&&!control->revoked&&hosting_.load()==control&&ack.session==control->session)
                    transport_.send_input_ack(control->peer,ack);
            })) {control->revoke();hosting_.store(nullptr);telemetry_.clear(MonitorSide::Host);status("Capture failed: "+error);}
            else if(!settings.warm) {
                const auto peers=transport_.peers();
                for(const auto& p:peers)if(p.id==peer&&(p.features&hive::FeatureDesktopAux))shell_.observe_cursor(peer);
                status("Streaming primary display to a computer on your network.");
            }
        };
        callbacks.cancel_warm=[this](const std::string& peer,uint64_t session){
            const std::lock_guard lock(host_mutex_);const auto target=hosting_.load();
            if(target&&target->warming&&target->peer==peer&&target->session==session){
                target->revoke();hosting_=nullptr;host_.stop();hive::log("Warm host released: "+peer);
            }
        };
        callbacks.stop_stream=[this](const std::string& peer) {
            if(closing_)return;
            const std::lock_guard lock(host_mutex_);
            const auto target=hosting_.load();
            if(target&&target->peer==peer) {target->revoke();input_ready_=false;clipboard_pending_=false;hosting_.store(nullptr);shell_.observe_cursor({});staged_clipboards_.erase(peer);host_.stop();telemetry_.clear(MonitorSide::Host);hive::release_injected_input();status("Remote control released.");}
        };
        callbacks.frame=[this](const std::string& peer,hive::EncodedFrame frame) {
            const auto target=active_.load();
            if(closing_)return;
            if(!target||*target!=peer){
                std::shared_ptr<WarmView> warm;
                {const std::lock_guard lock(warm_mutex_);auto found=warm_views_.find(peer);if(found!=warm_views_.end())warm=found->second;}
                if(warm&&!warm->failed&&warm->settings.session==frame.session){
                    warm->size=(uint64_t(frame.width)<<32)|frame.height;warm->viewer->submit(std::move(frame));
                }
                return;
            }
            const std::lock_guard lock(view_mutex_);
            const auto current=active_.load();
            if(current&&*current==peer) {
                if(frame.session!=active_session_)return;
                ++received_frames_;
                const uint64_t size=(uint64_t(frame.width)<<32)|frame.height;
                if(remote_size_.exchange(size)!=size)shell_.set_remote_size(frame.width,frame.height);
                viewer_.load()->submit(std::move(frame));
            }
        };
        callbacks.input_begin=[this](const std::string& peer,uint64_t session) {
            // InputEnable follows Clipboard/Start on the control stream. The
            // dedicated input reader itself never takes this potentially slow lock.
            const std::lock_guard lock(host_mutex_);
            const auto target=hosting_.load();
            if(closing_||!target||target->revoked||target->warming||target->peer!=peer||target->session!=session||!input_ready_)return false;
            target->input_enabled=true;return true;
        };
        callbacks.input_packet=[this](const std::string& peer,const hive::InputPacket& packet) {
            const auto target=hosting_.load();
            if(closing_||!target||target->revoked||!target->input_enabled||target->peer!=peer||target->session!=packet.session){
                // Stop/lease revocation can overtake the final packet on the
                // independent lane. End only that session, rather than report
                // an injection failure that would tear down a reciprocal stream.
                if(!closing_)transport_.end_stream(peer,packet.session);return;
            }
            if(!host_.input(packet)){
                hive::InputAck ack;ack.session=packet.session;ack.sequence=packet.sequence;ack.capture_time_us=packet.capture_time_us;
                transport_.send_input_ack(peer,ack);
                if(!target->input_error_reported.exchange(true))status("Dedicated desktop input connection failed.");
            }
        };
        callbacks.input=[this](const std::string& peer,const hive::InputEvent& event) {
            const std::lock_guard lock(host_mutex_);
            const auto target=hosting_.load();
            if(!closing_&&target&&!target->revoked&&!target->warming&&!target->input_enabled&&target->peer==peer&&(input_ready_||event.kind==hive::InputKind::ReleaseAll)) {
                if(!host_.input(event)&&!input_error_reported_){input_error_reported_=true;status("Desktop service input connection failed.");}
            }
        };
        callbacks.clipboard=[this](const std::string& peer,const std::wstring& text) {
            if(closing_)return;
            const std::lock_guard lock(host_mutex_);
            if(closing_)return;
            // Clipboard arrives before Start on the same control connection.
            // Another peer must not overwrite this controller's readiness.
            staged_clipboards_[peer]=text;
            const auto controller=hosting_.load();
            if(controller&&!controller->revoked&&!controller->warming&&controller->peer==peer) {
                apply_handoff(peer);
                return;
            }
            // A remote copy updates the operating computer's carried cache only
            // while that remote remains selected; unrelated/late peers stay staged.
            const auto viewing=active_.load();
            if(viewing&&*viewing==peer) {
                try {shell_.apply_clipboard(text);}
                catch(const std::exception& error) {status(std::string("Remote clipboard update failed: ")+error.what());}
            }
        };
        std::string error;
        // The isolated full-App regression uses generated loopback peers only.
#ifdef HIVEDESK_APP_SESSION_TEST
        constexpr bool discovery=false;
#else
        constexpr bool discovery=true;
#endif
        if(!transport_.start({config_.device_id,hive::narrow(config_.name),config_.group_key,config_.port,discovery,true},std::move(callbacks),error))throw std::runtime_error("Network startup failed: "+error);
        status("Ready. Move to the top-left corner to switch computers.");
    }
    void retry_tick() {
        if(updating_||closing_||!reconnect_.current(generation_))return;
        const auto current=active_.load();
        if(current&&view_ready_) {
            const auto state=viewer_.load()->stats();
            if(active_connection_!=transport_.connection_token(*current)||state.starts_with("Media error:")){
                hive::log("Viewer recovery: "+*current+" session="+std::to_string(active_session_.load())+" "+state);
                view_ready_=false;
            }
        }
        if(current&&!view_ready_&&!reconnect_.waiting) {
            status("Connection lost. Reconnecting to "+reconnect_.peer+"...");
            shell_.show_reconnecting(reconnect_.peer);
            go_local(active_.exchange(nullptr),false);reconnect_.lost(GetTickCount64());
        }
        if(!reconnect_.due(generation_,GetTickCount64()))return;
        shell_.show_reconnecting(reconnect_.peer);
        if(!transport_.connection_token(reconnect_.peer))return;
        const auto peer=reconnect_.peer;const auto epoch=reconnect_.epoch;
        reconnect_.attempted(GetTickCount64());
        select(peer,epoch);
    }
    void request_snapshot(const std::string& peer) {
        const std::lock_guard lock(snapshot_reply_mutex_);
        const auto request=++snapshot_sequence_;
        snapshot_requests_[peer]=request;
        if(!transport_.request_snapshot(peer,request))snapshot_requests_.erase(peer);
    }
    void select(std::string peer,uint64_t epoch) {
        if(epoch!=generation_||closing_)return;
        hive::log("Viewer startup: "+peer+" generation="+std::to_string(epoch));
        request_snapshot(peer);
        go_local(active_.exchange(nullptr),false);
        remote_size_=0;received_frames_=0;
        const auto peers=transport_.peers();
        auto found=std::find_if(peers.begin(),peers.end(),[&](const hive::Peer& item){return item.id==peer;});
        if(found==peers.end())return;
        const auto label=hive::widen(found->name);
        if(!found->authenticated) {
            std::string connected,error;
            if(!transport_.connect(found->address,found->port,connected,error)) {status("Connection failed: "+error);return;}
            peer=connected;
        }
        if(epoch!=generation_||closing_)return;
        uint32_t refresh=0;
        try {refresh=hive::primary_refresh_millihz();}
        catch(const std::exception& problem){status(problem.what());return;}
        const auto connected_peers=transport_.peers();
        const auto negotiated=std::find_if(connected_peers.begin(),connected_peers.end(),[&](const hive::Peer& item){return item.id==peer;});
        const bool automatic=negotiated!=connected_peers.end()&&(negotiated->features&hive::FeatureAutomaticStream);
        std::string error;
        active_connection_=transport_.connection_token(peer);if(!active_connection_)return;
        // Retain one immutable HWND lease throughout validation and startup.
        // A second read could bind a warmed decoder to a newer same-peer window.
        const auto selected_surface=shell_.viewer_surface();
        if(!selected_surface||selected_surface->peer!=peer||epoch!=generation_)return;
        std::shared_ptr<WarmView> warm;
        {const std::lock_guard lock(warm_mutex_);auto cached=warm_views_.find(peer);if(cached!=warm_views_.end()){warm=cached->second;warm_views_.erase(cached);}}
        if(warm&&(warm->failed||warm->viewer->stats().starts_with("Media error:")||warm->connection!=active_connection_||warm->surface!=selected_surface||warm->settings.receiver_millihz!=refresh)){
            transport_.cancel_warm(peer,warm->settings.session);warm->viewer->close();warm.reset();
        }
        if(!warm){
            // Cold fallback needs GPU capacity now, rather than waiting for the
            // grace timer while unused warm decoders retain their surfaces.
            std::map<std::string,std::shared_ptr<WarmView>> unused;
            {const std::lock_guard lock(warm_mutex_);unused.swap(warm_views_);}
            for(const auto& item:unused){transport_.cancel_warm(item.first,item.second->settings.session);item.second->viewer->close();}
        }
        {const std::lock_guard lock(view_mutex_);
            view_surface_=selected_surface;
            if(!view_surface_||view_surface_->peer!=peer){status("Viewer selection changed before startup; waiting for the current selection.");return;}
            hive::log(warm?"Viewer reusing warm graphics: "+peer:"Viewer creating graphics: "+peer);
            if(warm)viewer_.store(warm->viewer);
            else viewer_.store(std::make_shared<hive::MediaViewer>());
            if(!warm&&!viewer_.load()->open(view_surface_->window,error,
            [this,peer,epoch](const hive::CursorPresentation& cursor){
                if(epoch==generation_&&!closing_)
                    shell_.update_cursor_composition(peer,cursor.session,cursor.sequence,cursor.composition);
            })){status("Viewer failed: "+error);return;}}
        hive::log("Viewer graphics ready: "+peer);
        if(epoch!=generation_||closing_) {const std::lock_guard lock(view_mutex_);viewer_.load()->close();view_surface_.reset();return;}
        // The control stream delivers clipboard before Start/InputEnable. The
        // separate input lane is armed only after that handoff has completed.
        if(!transport_.send_clipboard(peer,shell_.cached_clipboard())) {const std::lock_guard lock(view_mutex_);viewer_.load()->close();view_surface_.reset();status("Could not send clipboard handoff.");return;}
        hive::StreamSettings stream;
        if(automatic){stream.session=warm?warm->settings.session:++session_sequence_;stream.receiver_millihz=refresh;}
        else {stream.fps=std::max(1u,refresh/1000);stream.bitrate_mbps=80;status("Peer uses legacy streaming. Update both computers for automatic quality.");}
        bool admitted=false;
        {
            const std::lock_guard lock(view_intent_mutex_);
            if(epoch==generation_&&!closing_){
                auto controlled=std::make_shared<const std::string>(peer);
                last_controlled_.store(controlled);active_.store(std::move(controlled));
                active_session_=stream.session;
                active_dedicated_input_=automatic&&(negotiated->features&hive::FeatureDedicatedInput)!=0;
                view_control_=std::make_shared<const hive::ViewControl>(peer,stream.session,epoch);
                admitted=true;
            }
        }
        // GPU/DXGI teardown can send UI messages; never join with the intent
        // lock held while UI selection or the input thread needs that lock.
        if(!admitted){const std::lock_guard lock(view_mutex_);viewer_.load()->close();view_surface_.reset();return;}
        if(warm)viewer_.load()->activate();
        if(warm){const auto size=warm->size.load();remote_size_=size;if(size)shell_.set_remote_size(uint32_t(size>>32),uint32_t(size));}
        telemetry_.select(MonitorSide::View,peer,stream.session,hive::narrow(label));
        if(!transport_.request_stream(peer,stream)) {go_local(active_.exchange(nullptr),false);status("Could not request desktop stream; retrying.");return;}
        if(active_dedicated_input_&&!transport_.begin_input(peer,stream.session)) {go_local(active_.exchange(nullptr),false);status("Could not initialize dedicated input; retrying.");return;}
        hive::log("Viewer stream requested: "+peer+" session="+std::to_string(stream.session));
        hive::StreamStartupDeadline deadline(GetTickCount64(),transport_.received_video_bytes(peer));
        bool ready=false;std::string failure;
        while(epoch==generation_&&!closing_&&!deadline.expired(GetTickCount64())) {
            if(transport_.connection_token(peer)!=active_connection_){failure="Connection changed while starting video; retrying.";break;}
            const auto progress=transport_.received_video_bytes(peer);
            deadline.progress(GetTickCount64(),progress);
            const auto state=viewer_.load()->stats();
            if(state.find("Media error:")==0) {failure=state;break;}
            // The waiting cover already makes the viewer visible. Keep it
            // opaque until this session has actually submitted its first image,
            // not merely finished decoding while the old HWND image remains.
            if(viewer_.load()->has_presented_frame()&&(!active_dedicated_input_||transport_.input_ready(peer,stream.session))) {ready=true;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if(epoch!=generation_||closing_) {go_local(active_.exchange(nullptr),false);return;}
        if(!ready) {
            if(failure.empty()&&viewer_.load()->has_frame()&&!viewer_.load()->has_presented_frame())
                failure="Desktop decoded, but the first image was not presented. Retrying with the connection snapshot visible.";
            if(failure.empty()&&active_dedicated_input_&&viewer_.load()->has_presented_frame())
                failure="The desktop is ready, but its input channel did not become ready. Install the same HiveDesk version on the controlled computer.";
            if(failure.empty()) failure=received_frames_.load()==0?
                "Connected to "+hive::narrow(label)+", but no video frames arrived. Check capture/encoding on that computer.":
                "Received "+std::to_string(received_frames_.load())+" video frames from "+hive::narrow(label)+", but none could be displayed. "+viewer_.load()->stats();
            go_local(active_.exchange(nullptr),false);status(failure);reconnect_.next_ms=GetTickCount64()+reconnect_.delay_ms();return;
        }
        {
            const std::lock_guard lock(view_intent_mutex_);
            if(epoch==generation_&&!closing_&&view_control_&&view_control_->session==stream.session){
                view_ready_=true;reconnect_.connected();shell_.show_remote(peer,label,stream.session,view_surface_->window);
                // A successful first present can precede the queued UI session
                // switch. Replay after show_remote; sequence guards reject any
                // replay overtaken by a newer presentation callback.
                const auto cursor=viewer_.load()->cursor_presentation();
                if(cursor.valid)shell_.update_cursor_composition(peer,cursor.session,cursor.sequence,cursor.composition);
                status("Viewing "+hive::narrow(label)+" | HEVC 4:4:4 | Ctrl+Alt+` returns here");
                return;
            }
        }
        go_local(active_.exchange(nullptr),false);
    }
    void go_local(std::shared_ptr<const std::string> previous,bool notify_shell=true,bool notify_source=true) {
        view_ready_=false;const auto old_session=active_session_.exchange(0);const bool dedicated=active_dedicated_input_.exchange(false);
        {const std::lock_guard lock(view_intent_mutex_);view_control_.reset();}
        telemetry_.clear(MonitorSide::View);
        if(previous) {
            if(notify_source){if(dedicated&&old_session)transport_.end_input(*previous,old_session);else transport_.send_input(*previous,{hive::InputKind::ReleaseAll,0,0,0});transport_.stop_stream(*previous);}
            auto expected=previous;last_controlled_.compare_exchange_strong(expected,nullptr);
        }
        {const std::lock_guard lock(view_mutex_);
            if(view_surface_)hive::log("Viewer graphics retiring: "+view_surface_->peer+" session="+std::to_string(old_session));
            viewer_.load()->close();
            if(view_surface_)hive::log("Viewer graphics retired: "+view_surface_->peer);
            view_surface_.reset();}
        if(!closing_&&notify_shell)shell_.show_local();
    }
    void finish_shutdown() {
        kick_wake_.notify_all();if(kick_worker_.joinable())kick_worker_.join();
        active_.store(nullptr);
        // A Quit callback can clear active_ and enqueue cleanup immediately before
        // closing_ causes the command queue to stop. Retain this peer independently.
        const auto previous=last_controlled_.exchange(nullptr);
        if(previous) {const auto session=active_session_.exchange(0);if(active_dedicated_input_.exchange(false)&&session)transport_.end_input(*previous,session);else transport_.send_input(*previous,{hive::InputKind::ReleaseAll,0,0,0});transport_.stop_stream(*previous);}
        {const std::lock_guard lock(host_mutex_);input_ready_=false;clipboard_pending_=false;staged_clipboards_.clear();hosting_.store(nullptr);host_.stop();}
        {const std::lock_guard lock(view_mutex_);viewer_.load()->close();view_surface_.reset();}
        std::map<std::string,std::shared_ptr<WarmView>> warm;
        {const std::lock_guard lock(warm_mutex_);warm.swap(warm_views_);warm_visible_.clear();}
        for(const auto& item:warm){transport_.cancel_warm(item.first,item.second->settings.session);item.second->viewer->close();}
        transport_.stop();hive::release_injected_input();
        telemetry_.clear(MonitorSide::View);telemetry_.clear(MonitorSide::Host);
    }
    void shutdown() {
        if(closing_.exchange(true))return;
        ++generation_;view_ready_=false;wake_.notify_all();kick_wake_.notify_all();
        if(worker_.joinable()) {
            const HANDLE worker_handle=worker_.native_handle();
            while(WaitForSingleObject(worker_handle,0)==WAIT_TIMEOUT) {
                const DWORD wait=MsgWaitForMultipleObjectsEx(1,&worker_handle,200,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if(wait==WAIT_OBJECT_0)break;
                // WM_QUIT was consumed by Shell::run. Pump sent/queued window
                // messages until media teardown completes, without restarting UI.
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT)continue;
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
                if(wait==WAIT_FAILED)Sleep(1);
            }
            worker_.join();
        } else finish_shutdown();
    }
    struct WarmView {
        std::shared_ptr<const hive::ViewerSurface> surface;
        std::shared_ptr<hive::MediaViewer> viewer=std::make_shared<hive::MediaViewer>();
        hive::StreamSettings settings;
        uint64_t connection=0,last_visible=0,renewed=0;
        std::atomic<bool> failed{false};std::atomic<uint64_t> size{0};
    };
    std::mutex warm_mutex_;
    std::map<std::string,std::shared_ptr<WarmView>> warm_views_;
    std::vector<std::shared_ptr<const hive::ViewerSurface>> warm_visible_;
    hive::AppConfig config_;
    hive::Shell shell_;
    hive::Transport transport_;
    hive::HostBridge host_;
    std::shared_ptr<const hive::ViewerSurface> view_surface_;
    std::atomic<std::shared_ptr<hive::MediaViewer>> viewer_{std::make_shared<hive::MediaViewer>()};
    hive::Telemetry telemetry_{hive::data_directory()};
    std::mutex snapshot_mutex_;std::map<std::string,uint64_t> snapshot_pending_;
    std::mutex snapshot_reply_mutex_;
    std::map<std::string,uint64_t> snapshot_requests_;uint64_t snapshot_sequence_=0;
    std::array<hive::Snapshot,2> snapshot_caches_;std::array<std::chrono::steady_clock::time_point,2> snapshot_times_{};
    std::atomic<std::shared_ptr<const std::string>> active_,last_controlled_;
    std::atomic<std::shared_ptr<hive::HostControl>> hosting_;
    std::shared_ptr<const hive::ViewControl> view_control_; // Protected by view_intent_mutex_.
    std::map<std::string,uint64_t> legacy_kicked_; // Protected by host_mutex_.
    std::shared_ptr<hive::HostControl> kick_pending_; // Protected by kick_mutex_.
    std::atomic<bool> closing_=false,view_ready_=false,updating_=false;
    ULONGLONG last_update_tick_=0;
    const std::filesystem::path video_restart_marker_=hive::data_directory()/L"video-restart.pending";
    bool video_restart_pending_=false; // Command worker only.
    bool input_ready_=false; // Protected by host_mutex_; belongs only to hosting_.
    bool input_error_reported_=false;
    std::atomic<bool> clipboard_pending_=false;ULONGLONG last_clipboard_retry_=0;
    ULONGLONG last_debug_tick_=0;
    std::map<std::string,std::wstring> staged_clipboards_; // Protected by host_mutex_.
    std::atomic<uint64_t> generation_=0;
    std::atomic<uint64_t> remote_size_=0;
    std::atomic<uint64_t> received_frames_=0;
    std::atomic<uint64_t> active_session_=0;
    std::atomic<bool> active_dedicated_input_=false;
    uint64_t input_sequence_=0; // Serialized by view_intent_mutex_.
    hive::ReconnectIntent reconnect_; // Command worker only.
    uint64_t active_connection_=0,session_sequence_=GetTickCount64();
    std::mutex queue_mutex_,host_mutex_,view_mutex_,config_mutex_,view_intent_mutex_,kick_mutex_;
    std::condition_variable wake_,kick_wake_;
    std::deque<std::function<void()>> queue_;
    std::thread worker_,kick_worker_;
};
}
#ifndef HIVEDESK_APP_SESSION_TEST
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count=0;LPWSTR* arguments=CommandLineToArgvW(GetCommandLineW(),&count);bool open_settings=true;bool background=false,quiet=false,host_check=false,warm_check=false;
    HANDLE previous=nullptr;bool valid_arguments=true,recover=false,manual_restart=false;
    if(arguments){for(int i=1;i<count;++i){
        if(wcscmp(arguments[i],L"--background")==0)background=true;
        else if(wcscmp(arguments[i],L"--quiet")==0)quiet=true;
        else if(wcscmp(arguments[i],L"--host-check")==0)host_check=true;
        else if(wcscmp(arguments[i],L"--warm-broker-check")==0)warm_check=true;
        else if(wcscmp(arguments[i],L"--restart")==0)manual_restart=true;
        else if(wcscmp(arguments[i],L"--restart-after")==0){if(previous||++i>=count||!hive::gpu_policy_restart::parse_wait_handle(arguments[i],previous)){valid_arguments=false;break;}}
        else if(wcscmp(arguments[i],L"--recover-after")==0){recover=true;if(previous||++i>=count||!hive::gpu_policy_restart::parse_wait_handle(arguments[i],previous)){valid_arguments=false;break;}}
    }LocalFree(arguments);}open_settings=!background&&!quiet;
    if(!valid_arguments||(manual_restart&&previous))return ERROR_INVALID_PARAMETER;
    // Also works when the tray's message loop cannot open its context menu.
    if(manual_restart){
        quiet=true;open_settings=false;
        const HWND owner=FindWindowW(L"HiveDesk.Owner",nullptr);DWORD pid=0;
        if(owner)GetWindowThreadProcessId(owner,&pid);
        if(pid){previous=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,pid);if(!previous)return ERROR_ACCESS_DENIED;recover=true;}
    }
    hive::recovery::Handle recovery_guard;
    if(recover){
        recovery_guard.value=CreateMutexW(nullptr,FALSE,L"Local\\HiveDesk.ManualRecovery.v1");
        const DWORD guard_error=GetLastError();
        if(!recovery_guard.value||guard_error==ERROR_ALREADY_EXISTS){CloseHandle(previous);return recovery_guard.value?0:ERROR_ACCESS_DENIED;}
        bool forced=false;
        if(!hive::recovery::previous(previous,forced)){
            hive::log("Recovery failed: predecessor identity or bounded termination failed.");
            MessageBoxW(nullptr,L"Windows could not finish stopping HiveDesk. A replacement was not started. Restart Windows if the graphics driver remains stuck.",L"HiveDesk restart failed",MB_OK|MB_ICONERROR);
            return ERROR_PROCESS_ABORTED;
        }
        hive::log(forced?"Manual recovery: forced old agent to exit.":"Manual recovery: old agent exited normally.");
        const auto broker=hive::recovery::restart_broker();
        hive::log(broker==hive::recovery::BrokerResult::Restarted?"Manual recovery: old Host stopped by service.":
            broker==hive::recovery::BrokerResult::Requested?"Manual recovery: Host reset requested.":
            broker==hive::recovery::BrokerResult::Unavailable?"Manual recovery: service reset unavailable; restarting tray only.":"Manual recovery: Host reset failed or timed out.");
    } else if(previous&&!hive::gpu_policy_restart::wait_for_previous(previous))return ERROR_INVALID_HANDLE;
    hive::update::StartupGuard update_startup;if(!update_startup.allowed())return 0;
    HANDLE singleton=CreateMutexW(nullptr,FALSE,L"Local\\HiveDesk.Desktop.Agent");
    if(!singleton)return 1;
    const DWORD singleton_status=GetLastError();
    if(host_check){CloseHandle(singleton);return hive::host_check();}
    if(warm_check){CloseHandle(singleton);return hive::warm_broker_check();}
    if(singleton_status==ERROR_ALREADY_EXISTS){if(open_settings){const auto owner=FindWindowW(L"HiveDesk.Owner",nullptr);DWORD process=0;GetWindowThreadProcessId(owner,&process);AllowSetForegroundWindow(process);PostMessageW(owner,WM_APP+45,0,0);}CloseHandle(singleton);return 0;}
    if(recovery_guard.value){CloseHandle(recovery_guard.value);recovery_guard.value=nullptr;}
    update_startup.release();
    int result=1;
    try {hive::log("HiveDesk " HIVEDESK_VERSION " starting");auto config=hive::load_config();if(background&&!config.desktop.startup){CloseHandle(singleton);return 0;}hive::set_startup(config.desktop.startup);App app(std::move(config));result=app.run(instance,open_settings);}
    catch(const std::exception& error) {hive::log(std::string("Fatal: ")+error.what());MessageBoxW(nullptr,hive::widen(error.what()).c_str(),L"HiveDesk",MB_OK|MB_ICONERROR);}
    CloseHandle(singleton);return result;
}
#endif
