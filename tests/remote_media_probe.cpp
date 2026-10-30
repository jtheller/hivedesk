#include "hivedesk/transport.h"
#include "hivedesk/config.h"
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <chrono>
int main(int argc,char** argv){
    if(argc!=3){std::puts("Usage: HiveDeskRemoteMediaProbe <IPv4> capabilities|intel-roundtrip|receive\nReceive previews THIS computer on the target for 15 seconds; no input or clipboard.");return 2;}
    const std::string mode_name=argv[2];const uint32_t mode=mode_name=="capabilities"?1:mode_name=="intel-roundtrip"?2:mode_name=="receive"?3:0;if(!mode)return 2;
    setvbuf(stdout,nullptr,_IONBF,0);const auto config=hive::load_config();hive::Transport client;
    const auto request=GetTickCount64();std::mutex mutex;std::condition_variable changed;bool done=false;hive::MediaProbeReport report;
    hive::TransportCallbacks callbacks;callbacks.peer=[](const hive::Peer& p){if(!p.version.empty())std::printf("peer=%s version=%s\n",p.name.c_str(),p.version.c_str());};
    callbacks.media_probe_report=[&](const std::string&,hive::MediaProbeReport value){if(value.request!=request)return;{std::lock_guard lock(mutex);report=std::move(value);done=true;}changed.notify_all();};
    std::string peer,error;
    if(!client.start({"media-check-"+std::to_string(GetCurrentProcessId()),"Media diagnostic",config.group_key,0,false,true},callbacks,error)||!client.connect(argv[1],config.port,peer,error)){std::printf("FAIL connect: %s\n",error.c_str());return 1;}
    if(!client.request_media_probe(peer,request,mode)){std::puts("UNAVAILABLE: target requires remote media diagnostics support (0.13.5+)");client.stop();return 2;}
    {std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(55),[&]{return done;})){std::puts("FAIL diagnostic reply timeout");lock.unlock();client.stop();return 1;}}
    client.stop();std::printf("%s\nRESULT code=%u\n",report.text.c_str(),report.result);return report.result?1:0;
}
