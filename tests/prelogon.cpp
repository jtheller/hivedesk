#include <winsock2.h>
#include <ws2tcpip.h>
#define HIVEDESK_PRELOGON_TEST
#include "../src/prelogon.cpp"
#include <iostream>
#include <chrono>
#include <stdexcept>

namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void await(F predicate,const char* text){const auto until=GetTickCount64()+3000;while(!predicate()&&GetTickCount64()<until)Sleep(10);require(predicate(),text);}
struct FakeHost {
    static inline std::atomic<int> starts=0,stops=0,promotions=0,inputs=0,previews=0;
    std::function<void(const hive::InputAck&)> acknowledge;
    bool start(const hive::StreamSettings&,std::function<void(hive::EncodedFrame)>,std::string&,
        std::function<bool()>,std::function<void(const hive::StreamHealth&)>,std::function<void(const hive::SourceDiagnostics&)>,
        std::function<void(const hive::InputAck&)> ack){acknowledge=std::move(ack);++starts;return true;}
    void stop(){++stops;}
    void request_keyframe(){}void feedback(const hive::StreamFeedback&){}void notify_ready(){}
    bool activate(){++promotions;return true;}bool preview(bool){++previews;return true;}
    bool input(const hive::InputEvent&){++inputs;return true;}
    bool input(const hive::InputPacket& p){++inputs;hive::InputAck a;a.session=p.session;a.sequence=p.sequence;a.capture_time_us=p.capture_time_us;a.injected=true;acknowledge(a);return true;}
};
}
int main(){try{
    using namespace hive;
    require(bootstrap::replace_host(1,L"S-1-5-18",1,L"user"),"First logon must replace SYSTEM-only broker ACL");
    require(bootstrap::replace_host(1,L"user",1,L"S-1-5-18"),"Logoff must release user broker");
    require(!bootstrap::replace_host(1,L"user",1,L"user"),"Lock/unlock must preserve active broker");
    require(bootstrap::replace_host(1,L"user",2,L"user"),"Console migration must replace broker");
    require(bootstrap::prelogon_idle(false,false,false)&&bootstrap::prelogon_idle(true,true,false),"Idle/warm update admission");
    require(!bootstrap::prelogon_idle(true,false,false)&&!bootstrap::prelogon_idle(true,true,true),"Active control/Peek must defer OTA");
    require(bootstrap::valid_id(L"0123456789abcdef0123456789abcdef")&&!bootstrap::valid_id(L"arbitrary-path"),"Cache identity validation");
    WSADATA ws{};require(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock");
    SOCKET reserved=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(reserved,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Reserve isolated port");int size=sizeof(address);getsockname(reserved,reinterpret_cast<sockaddr*>(&address),&size);const auto port=ntohs(address.sin_port);closesocket(reserved);
    {
        Prelogon<FakeHost> endpoint;std::string error;
        const bootstrap::Identity identity{L"0123456789abcdef0123456789abcdef",L"Isolated prelogon test",true};
        require(endpoint.start(identity,error,port,false),error.c_str());
        Transport viewer;std::atomic<int> acks=0,ended=0;TransportCallbacks cb;
        cb.input_ack=[&](const std::string&,const InputAck& a){if(a.injected)++acks;};
        cb.host_ended=[&](const std::string&,uint64_t){++ended;};
        require(viewer.start({"prelogon-viewer","Generated viewer","",0,false,true},std::move(cb),error),error.c_str());
        std::string peer;require(viewer.connect("127.0.0.1",port,peer,error),error.c_str());
        StreamSettings request;request.session=51;request.receiver_millihz=60000;request.warm=true;
        require(viewer.request_stream(peer,request),"Warm request");await([]{return FakeHost::starts==1;},"Warm did not start broker");
        require(viewer.begin_input(peer,51),"Warm input request");Sleep(80);require(!viewer.input_ready(peer,51)&&FakeHost::inputs==0,"Warm granted input authority");
        require(viewer.preview(peer,51,true),"Peek request");await([]{return FakeHost::previews==1;},"Peek not forwarded");
        require(viewer.request_stream(peer,request),"Warm renewal");Sleep(50);require(FakeHost::starts==1,"Renewal restarted capture");
        request.warm=false;require(viewer.request_stream(peer,request),"Promotion request");await([]{return FakeHost::promotions==1;},"Warm promotion failed");
        require(viewer.begin_input(peer,51),"Input begin");await([&]{return viewer.input_ready(peer,51);},"Promoted input not ready");
        InputPacket packet;packet.session=51;packet.sequence=1;packet.capture_time_us=input_now_us();packet.event={InputKind::Key,'A',1,0};
        require(viewer.send_input(peer,packet),"Synthetic input enqueue");await([&]{return acks==1;},"Independent ACK not returned");
        // FakeHost consumes this event; no SendInput or desktop changes occur.
        require(FakeHost::inputs==1,"Unexpected injected-event count");
        const auto before=FakeHost::stops.load();viewer.cancel_warm(peer,51);Sleep(50);require(FakeHost::stops==before,"Late warm cancellation stopped promoted control");
        viewer.stop_stream(peer);await([&]{return FakeHost::stops>before;},"Stop did not release broker");
        require(viewer.connection_token(peer)!=0,"Stopping host unnecessarily broke network pair");
    }
    WSACleanup();std::cout<<"PASS: prelogon authority transitions, update admission, encrypted warm/Peek/promotion, independent synthetic input ACK, stale cancellation and stop. No physical input.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
