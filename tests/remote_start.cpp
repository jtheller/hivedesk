#include "hivedesk/transport.h"
#include "hivedesk/config.h"
#include "hivedesk/desktop_aux.h"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <condition_variable>

// Read-only warm lease probe: never sends a normal Start, clipboard or input.
// A busy host rejects the lease. Cleanup only cancels this warm session.
int warm_remote_check(const char* address) {
    const auto config=hive::load_config();std::mutex mutex;std::condition_variable changed;
    hive::Transport client;const uint64_t session=GetTickCount64();unsigned frames=0;bool declined=false;
    hive::TransportCallbacks callbacks;
    callbacks.frame=[&](const std::string&,hive::EncodedFrame frame){if(frame.session==session){std::lock_guard lock(mutex);++frames;changed.notify_all();}};
    callbacks.host_ended=[&](const std::string&,uint64_t id){if(id==session){std::lock_guard lock(mutex);declined=true;changed.notify_all();}};
    std::string error,peer;
    if(!client.start({"warm-check-"+std::to_string(GetCurrentProcessId()),"Warm media check",config.group_key,0,false,true},callbacks,error)||
        !client.connect(address,config.port,peer,error)){std::printf("FAIL warm connect: %s\n",error.c_str());return 1;}
    struct Stop{hive::Transport& client;std::string peer;uint64_t session;~Stop(){client.cancel_warm(peer,session);client.stop();}} stop{client,peer,session};
    hive::StreamSettings settings;settings.session=session;settings.receiver_millihz=100000;settings.warm=true;
    if(!client.request_stream(peer,settings)){std::puts("FAIL warm request (both endpoints need 0.4.0)");return 1;}
    {std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(6),[&]{return frames||declined;})||declined){std::puts("FAIL warm first frame (busy/declined/unavailable)");return 1;}}
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if(!client.request_stream(peer,settings)){std::puts("FAIL warm renewal");return 1;}
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {std::lock_guard lock(mutex);if(frames!=1||declined){std::puts("FAIL warm lease did not retain exactly one picture");return 1;}}
    std::puts("PASS remote warm request, single encoded picture, paused renewal, scoped cancellation; no input/clipboard/normal Start.");return 0;
}

// Bounded, explicit remote video-start diagnostic. No input, clipboard writes,
// viewer window or saved images. Reports transport and host pipeline evidence.
int remote_start_check(const char* address,const std::string& part) {
    const bool snapshot_only=part=="--snapshot-only",video_only=part=="--video-only";
    const auto config=hive::load_config();hive::Transport client;std::mutex mutex;
    unsigned frames=0,health_count=0,diagnostic_count=0,snapshots=0;
    hive::SourceDiagnostics source{};uint32_t width=0,height=0;std::string version;
    hive::TransportCallbacks callbacks;
    callbacks.peer=[&](const hive::Peer& peer){std::lock_guard lock(mutex);if(!peer.version.empty())version=peer.version;};
    callbacks.status=[](const std::string& message){std::printf("transport: %s\n",message.c_str());};
    callbacks.frame=[&](const std::string&,hive::EncodedFrame frame){std::lock_guard lock(mutex);++frames;width=frame.width;height=frame.height;};
    callbacks.health=[&](const std::string&,const hive::StreamHealth&){std::lock_guard lock(mutex);++health_count;};
    callbacks.diagnostics=[&](const std::string&,const hive::SourceDiagnostics& value){std::lock_guard lock(mutex);++diagnostic_count;source=value;};
    callbacks.snapshot=[&](const std::string&,hive::Snapshot value){
        try{const auto image=hive::decode_snapshot(value);std::lock_guard lock(mutex);++snapshots;std::printf("snapshot: %ux%u, %zu bytes\n",image.width,image.height,value.jpeg.size());}
        catch(const std::exception& e){std::printf("snapshot error: %s\n",e.what());}
    };
    std::string error,peer;
    if(!client.start({"remote-start-check-"+std::to_string(GetCurrentProcessId()),"Video startup check",config.group_key,0,false,true},callbacks,error)||
        !client.connect(address,config.port,peer,error)){std::printf("FAIL connect: %s\n",error.c_str());return 1;}
    struct Stop{hive::Transport& client;std::string peer;~Stop(){client.stop_stream(peer);client.stop();}} stop{client,peer};
    const uint64_t session=GetTickCount64();
    if(!video_only)client.request_snapshot(peer,1);
    if(!snapshot_only&&!client.request_stream(peer,{0,0,60000,session})){std::puts("FAIL stream request");return 1;}
    for(unsigned second=0;second<(snapshot_only?12u:30u);++second){
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::lock_guard lock(mutex);
        std::printf("t=%u version=%s connected=%d frames=%u size=%ux%u health=%u diagnostics=%u encoded=%.3f fps encode=%u us discarded=%llu snapshots=%u\n",
            second+1,version.c_str(),client.connection_token(peer)!=0,frames,width,height,health_count,diagnostic_count,
            source.encoded_millihz/1000.0,source.encode_us,static_cast<unsigned long long>(source.source_discarded),snapshots);
        std::fflush(stdout);
        std::printf("tcp:%s\n",client.tcp_diagnostics(peer).c_str());
    }
    std::lock_guard lock(mutex);return (snapshot_only?snapshots:frames)?0:1;
}
