#include "hivedesk/transport.h"
#include "hivedesk/media.h"
#include "media_test_api.h"
#include "hivedesk/desktop_aux.h"
#include "hivedesk/adaptive.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
int remote_start_check(const char* address,const std::string& part);
int warm_remote_check(const char* address,bool peek=false);
int main(int argc,char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        const std::string mode=argc>1?argv[1]:"--all";
        if(mode=="--peek"||mode=="--peek-remote"){if(mode=="--peek-remote"&&argc!=3)return 2;return warm_remote_check(mode=="--peek"?"127.0.0.1":argv[2],true);}
        if(mode=="--warm-remote"){
            if(argc!=3){std::cerr<<"Use --warm-remote <IPv4 address>\n";return 2;}
            return warm_remote_check(argv[2]);
        }
        // The broker accepts only the installed tray app. Exercise that app's
        // authenticated warm path; do not weaken its local IPC trust boundary.
        if(mode=="--warm")return warm_remote_check("127.0.0.1");
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
