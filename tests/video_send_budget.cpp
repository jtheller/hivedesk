#include "video_send_budget.h"
#include <deque>
#include <cstdio>
#include <stdexcept>
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
// Deterministic ownership model: each issued packet remains owned for one RTT.
// This isolates the application window ceiling; it does not model TCP congestion
// control, prove physical throughput, or substitute for the remote-device test.
unsigned simulate(bool adaptive){
    hive::VideoSendBudget budget;
    if(adaptive)budget.observe({true,100000,100000,1250000,1250000,1250000,0});
    std::deque<unsigned> flight;unsigned sent=0;
    for(unsigned t=0;t<2000000;t+=1000){
        while(!flight.empty()&&flight.front()<=t)flight.pop_front();
        if(t%10000==0&&budget.admits(flight.size(),flight.size()*100000,100000)){flight.push_back(t+100000);++sent;}
    }
    return sent;
}
}
int main(){try{
    hive::VideoSendBudget budget;
    require(budget.admits(1,100000,100000)&&!budget.admits(2,200000,100000),"Unobserved connection changed");
    budget.observe({true,1000,700,8000000,8000000,8000000,0});
    require(!budget.admits(2,200000,100000),"Fast LAN expanded its write window");
    budget.observe({true,100000,100000,1250000,1000000,2000000,200000});
    require(budget.bytes()==1000000&&budget.admits(9,900000,100000)&&!budget.admits(10,1000000,1),"Congestion/byte bound ignored");
    budget.observe({true,100000,100000,1250000,1000000,400000,0});
    require(budget.bytes()==400000&&!budget.admits(4,400000,1),"Receiver window ignored");
    budget.observe({true,200000,100000,1250000,1000000,2000000,0});
    require(!budget.admits(2,200000,1),"Queueing delay inflated the window");
    budget.observe({true,100000,100000,UINT32_MAX,UINT32_MAX,UINT32_MAX,0});
    require(budget.bytes()==hive::VideoSendBudget::MaximumBytes&&!budget.admits(32,32,1),"Hard ownership bound ignored");
    require(!budget.admits(3,UINT64_MAX,UINT64_MAX),"Byte admission overflow");
    budget.observe({true,140000,138000,65536,13090,262912,13090});
    require(budget.admits(0,0,1200000)&&!budget.admits(1,1200000,1),"Slow path queued another frame behind a large IDR");
    require(budget.admits(1,1000,1000)&&!budget.admits(2,13090,1),"Small high RTT packets lost their byte budget");
    budget.observe({true,UINT32_MAX,UINT32_MAX,1000000,1000000,1000000,0});
    require(!budget.adaptive(),"Invalid RTT sentinel expanded the window");
    budget.observe({});require(!budget.admits(3,300000,1),"Lost TCP observation retained expanded credit");
    const auto baseline=simulate(false),expanded=simulate(true);
    require(baseline==40&&expanded==200,"High RTT application ceiling did not improve");
    std::printf("PASS video byte budget: LAN unchanged, BDP/cwnd/rwnd bounds, pressure shrink, overflow; 100 ms model %u -> %u frames in 2 s (not a network benchmark)\n",baseline,expanded);return 0;
}catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}}
