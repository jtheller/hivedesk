#include "hivedesk/transport.h"
#include "hivedesk/config.h"
#include "hivedesk/desktop_aux.h"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

// Bounded, explicit remote video-start diagnostic. No input, clipboard writes,
// viewer window or saved images. Reports transport and host pipeline evidence.
int remote_start_check(const char* address) {
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
    client.request_snapshot(peer,1);
    if(!client.request_stream(peer,{0,0,60000,session})){std::puts("FAIL stream request");return 1;}
    for(unsigned second=0;second<12;++second){
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::lock_guard lock(mutex);
        std::printf("t=%u version=%s connected=%d frames=%u size=%ux%u health=%u diagnostics=%u encoded=%.3f fps encode=%u us discarded=%llu snapshots=%u\n",
            second+1,version.c_str(),client.connection_token(peer)!=0,frames,width,height,health_count,diagnostic_count,
            source.encoded_millihz/1000.0,source.encode_us,static_cast<unsigned long long>(source.source_discarded),snapshots);
        std::fflush(stdout);
    }
    std::lock_guard lock(mutex);return frames?0:1;
}
