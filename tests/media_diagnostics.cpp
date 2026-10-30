#include "../src/media_diagnostics.h"
#include "hivedesk/transport.h"
#include <cstdio>
#include <stdexcept>
#include <condition_variable>
#include <chrono>
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(int argc,char** argv){
    if(argc==2){const std::string mode=argv[1];setvbuf(stdout,nullptr,_IONBF,0);
        if(mode=="--child-hang"){std::puts("child started");Sleep(30000);return 0;}
        if(mode=="--child-flood"){for(unsigned i=0;i<100000;++i)std::putchar('x');return 7;}
        return 2;
    }
    try{
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        auto r=hive::bounded_probe_process(executable,L"--child-flood",3000,[]{return false;});
        require(r.result==7&&r.text.size()==hive::MaxMediaProbeReport&&r.text.find("truncated")!=std::string::npos,"bounded output or exit status");
        const auto began=GetTickCount64();r=hive::bounded_probe_process(executable,L"--child-hang",200,[]{return false;});
        require(r.result==WAIT_TIMEOUT&&GetTickCount64()-began<2000,"timeout did not reap child promptly");
        const auto started=GetTickCount64();r=hive::bounded_probe_process(executable,L"--child-hang",5000,[&]{return GetTickCount64()-started>100;});
        require(r.result==ERROR_CANCELLED&&GetTickCount64()-started<2000,"active-session cancellation");
        r=hive::run_media_probe(3,"127.0.0.1 & command",[]{return false;});require(r.result==ERROR_INVALID_PARAMETER,"non-address accepted");
        r=hive::run_media_probe(5,"127.0.0.1",[]{return false;});require(r.result==ERROR_INVALID_PARAMETER,"arbitrary diagnostic accepted");
        hive::MediaDiagnostics runner;bool deferred=false;
        runner.request(7,1,"127.0.0.1",[]{return false;},[&](hive::MediaProbeReport report){deferred=report.request==7&&report.result==ERROR_BUSY;});require(deferred,"active desktop admitted probe");
        std::mutex admission_mutex;std::condition_variable admission_changed;bool finished=false;
        runner.request(8,5,"127.0.0.1",[]{return true;},[&](hive::MediaProbeReport report){std::lock_guard lock(admission_mutex);finished=report.result==ERROR_INVALID_PARAMETER;admission_changed.notify_all();});
        {std::unique_lock lock(admission_mutex);require(admission_changed.wait_for(lock,std::chrono::seconds(2),[&]{return finished;}),"bounded worker completion");}
        deferred=false;runner.request(9,1,"127.0.0.1",[]{return true;},[&](hive::MediaProbeReport report){deferred=report.result==ERROR_BUSY;});require(deferred,"cooldown bypass");runner.stop();
        deferred=false;runner.request(10,1,"127.0.0.1",[]{return true;},[&](hive::MediaProbeReport report){deferred=report.result==ERROR_BUSY;});require(deferred,"shutdown admitted probe");
        std::mutex mutex;std::condition_variable changed;bool received=false;hive::Transport server,client;std::string error,peer;
        hive::TransportCallbacks cb;cb.media_probe_request=[&](const hive::Peer& p,uint64_t id,uint32_t mode){server.send_media_probe_report(p.id,{id,mode,"generated remote result"});};
        // Ephemeral listening port, read back from the advertised local peer is
        // not available; bind the test server to the suite's isolated test port.
        require(server.start({"diagnostic-server","Diagnostic server","Test1234",49235,false,true},cb,error),"server start");
        hive::TransportCallbacks reply;reply.media_probe_report=[&](const std::string&,hive::MediaProbeReport report){std::lock_guard lock(mutex);received=report.request==71&&report.result==2&&report.text=="generated remote result";changed.notify_all();};
        require(client.start({"diagnostic-client","Diagnostic client","Test1234",0,false,true},reply,error)&&client.connect("127.0.0.1",49235,peer,error),"encrypted connection");
        require(!client.request_media_probe(peer,0,1)&&!client.request_media_probe(peer,1,5),"request bounds");
        require(!server.send_media_probe_report("diagnostic-client",{71,0,std::string(hive::MaxMediaProbeReport+1,'x')}),"report bounds");
        require(client.request_media_probe(peer,71,2),"request send");
        {std::unique_lock lock(mutex);require(changed.wait_for(lock,std::chrono::seconds(3),[&]{return received;}),"encrypted report roundtrip");}
        client.stop();server.stop();std::puts("PASS bounded output, child timeout/cancellation, active desktop guard, fixed modes, encrypted report roundtrip");return 0;
    }catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}
}
