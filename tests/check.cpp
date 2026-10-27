#include "hivedesk/transport.h"
#include "hivedesk/media.h"
#include "hivedesk/host_bridge.h"
#include <mutex>
#include <condition_variable>
#include "media_test_api.h"
#include "hivedesk/desktop_aux.h"
#include "hivedesk/adaptive.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
int remote_start_check(const char* address,const std::string& part);
int warm_remote_check(const char* address);
int main(int argc,char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        const std::string mode=argc>1?argv[1]:"--all";
        if(mode=="--warm-remote"){
            if(argc!=3){std::cerr<<"Use --warm-remote <IPv4 address>\n";return 2;}
            return warm_remote_check(argv[2]);
        }
        if(mode=="--warm"){
            // Explicit installed-broker media probe. No hooks or input packets.
            std::mutex mutex;std::condition_variable changed;unsigned frames=0;uint64_t sequence=0;bool fresh=false;
            hive::HostBridge host;hive::StreamSettings settings;settings.session=GetTickCount64();settings.receiver_millihz=60000;settings.warm=true;
            std::string why;
            if(!host.start(settings,[&](hive::EncodedFrame frame){std::lock_guard lock(mutex);if(frames){fresh=frame.keyframe&&frame.sequence>sequence;}sequence=frame.sequence;++frames;changed.notify_all();},why))throw std::runtime_error(why);
            {std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return frames>0;}))throw std::runtime_error("No warm broker frame");}
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            {std::lock_guard lock(mutex);if(frames!=1)throw std::runtime_error("Unselected broker kept encoding");}
            if(!host.activate())throw std::runtime_error("Broker did not acknowledge activation");
            {std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return frames>1;})||!fresh)throw std::runtime_error("Promotion did not deliver a fresh sequenced IDR");}
            host.stop();std::cout<<"PASS installed broker warm/pause/activate/fresh IDR; no input injected.\n";return 0;
        }
        if(mode=="--remote"){
            const std::string part=argc==4?argv[3]:"all";
            if((argc!=3&&argc!=4)||(part!="all"&&part!="--snapshot-only"&&part!="--video-only")){
                std::cerr<<"Use --remote <IPv4 address> [--snapshot-only|--video-only]\n";return 2;
            }
            return remote_start_check(argv[2],part);
        }
        bool ok=true; std::string report;
        if(mode=="--lan"){ok=hive::Transport::discovery_self_test(report);std::cout<<report<<'\n';return ok?0:1;}
        if(mode=="--transport"||mode=="--all") { const bool passed=hive::Transport::self_test(report); std::cout<<report<<'\n'; ok=ok&&passed; }
        if(mode=="--media"||mode=="--all") { const bool passed=hive::media_self_test(report); std::cout<<report<<'\n'; ok=ok&&passed; }
        if(mode=="--media-auto"||mode=="--all") { const bool passed=hive::media_auto_self_test(report); std::cout<<report<<'\n'; ok=ok&&passed; }
        if(mode=="--capture") {const bool passed=hive::media_capture_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--render") {const bool passed=hive::media_render_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--throughput") {const bool passed=hive::media_throughput_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--pipeline") {const bool passed=hive::media_pipeline_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--auto-quality") {const bool passed=hive::media_auto_quality_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--motion") {const bool passed=hive::media_motion_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--colors"||mode=="--all") {const bool passed=hive::media_color_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--desktop-aux"||mode=="--all") {const bool passed=hive::desktop_aux_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--snapshot") {const bool passed=hive::snapshot_capture_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--adaptive"||mode=="--all") {const bool passed=hive::adaptive_self_test(report);std::cout<<report<<'\n';ok=ok&&passed;}
        if(mode=="--display")std::cout<<"Primary refresh: "<<hive::primary_refresh_millihz()/1000.0<<" Hz (read-only)\n";
        if(mode!="--all"&&mode!="--transport"&&mode!="--media"&&mode!="--media-auto"&&mode!="--capture"&&mode!="--render"&&mode!="--throughput"&&mode!="--pipeline"&&mode!="--auto-quality"&&mode!="--motion"&&mode!="--colors"&&mode!="--desktop-aux"&&mode!="--snapshot"&&mode!="--adaptive"&&mode!="--display") {std::cerr<<"Use --all, --transport, --media, --media-auto, --capture, --render, --colors, --desktop-aux, --snapshot, --adaptive, --display, --throughput, --pipeline, --auto-quality or --motion\n";return 2;}
        return ok?0:1;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
