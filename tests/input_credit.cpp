#include "input_credit.h"
#include "input_send_window.h"
#include <cstdio>
#include <stdexcept>
int main(){try{
    const auto check=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    hive::InputCredit credit;
    for(int i=0;i<64;++i)credit.observe(1500,200,300,true);
    check(credit.limit()==8,"Fast LAN behavior changed");
    credit.observe(200000,200,300,true);check(credit.limit()==8,"One RTT spike inflated credits");
    for(int i=0;i<32;++i)credit.observe(100500,200,300,true);
    check(credit.limit()==27,"100 ms propagation did not earn a bounded window");
    credit.observe(150000,20000,300,true);check(credit.limit()==8,"Host backlog inflated credits");
    for(int i=0;i<8;++i)credit.observe(300500,200,300,true);
    check(credit.limit()==64,"High RTT window exceeded or missed hard bound");
    credit.observe(100500,200,20000,true);check(credit.limit()==8,"Slow injection did not collapse window");
    hive::InputSendWindow window;window.reset(1);
    uint64_t seq=0,now=1000000;
    for(int i=0;i<8;++i){
        hive::InputPacket p{1,++seq,now,{hive::InputKind::Key,30,i%2}};
        check(window.enqueue(p,now)&&window.writable(),"Training input admission");window.take(now);
        check(window.acknowledge({1,p.sequence,p.capture_time_us,200,300,true},now+100500),"Training ACK");now+=101000;
    }
    check(window.snapshot(now).credit_limit==27,"Measured credit not connected to sender");
    for(int i=0;i<28;++i)check(window.enqueue({1,++seq,now,{hive::InputKind::Key,30,i%2}},now),"Ordered edge enqueue");
    hive::InputPacket first{};
    for(int i=0;i<27;++i){check(window.writable(),"Early high RTT credit stall");auto p=window.take(now);if(i==0)first=p;check(p.event.b==i%2,"Input edge order changed");}
    check(!window.writable(),"High RTT credit overflow");
    check(window.acknowledge({1,first.sequence,first.capture_time_us,20000,300,true},now+100500),"Pressure ACK");
    check(!window.writable()&&window.snapshot(now+100500).credit_limit==8,"Shrink lost inflight bound");
    window.reset(2);check(window.snapshot(now).credit_limit==8&&window.empty(),"Session retained old credits or input");
    std::puts("PASS input RTT window: unchanged LAN, no spike growth, bounded high delay, host pressure shrink, ordered edges and session reset");return 0;
}catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}}
