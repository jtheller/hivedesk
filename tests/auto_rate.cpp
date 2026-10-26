#include "hivedesk/auto_rate.h"
#include "hivedesk/adaptive.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool pass,const char* message){if(!pass)throw std::runtime_error(message);}
struct Simulation {
    hive::AutoRateController controller;
    hive::AutoRateSample sample{};
    uint64_t now=1000000;
    Simulation(uint32_t width=1920,uint32_t height=1080,uint32_t rate=60000):controller(width,height,rate){
        sample.pacing_millihz=rate;controller.observe(now,sample);
    }
    hive::AutoRateDecision step(uint32_t frames,uint32_t bytes_per_frame,uint32_t qp,uint32_t received,uint64_t wait=0,bool recover=false){
        now+=1000000;
        sample.encoded_frames+=frames;sample.encoded_bytes+=uint64_t(frames)*bytes_per_frame;
        sample.qp_samples+=frames;sample.qp_sum+=uint64_t(frames)*qp;
        sample.sent_frames+=frames;sample.received_frames+=received;sample.received_bytes+=uint64_t(received)*bytes_per_frame;
        sample.send_wait_us+=wait;++sample.receiver_report_id;sample.receive_sample_age_us=0;sample.recovering=recover;
        const auto result=controller.observe(now,sample);sample.pacing_millihz=result.rate_limit_millihz;return result;
    }
};
}
int main(){
    try {
        using namespace hive;
        require(initial_auto_bitrate(2560,1440,100000)==92160000,"Initial bitrate must scale with pixels and negotiated refresh");
        require(initial_auto_bitrate(7680,4320,240000)==500000000,"Initial bitrate estimate must have a finite bound");
        Simulation quality;const auto original=quality.controller.bitrate();
        auto q=quality.step(60,90000,22,60);
        require(q.bitrate_bps>original&&q.bitrate_bps<=uint64_t(original)*1125/1000,"High QP on a healthy network must raise bitrate gradually");
        require(q.rate_limit_millihz==60000,"Quality pressure with available network headroom must retain display cadence");
        const auto once=q;
        for(int i=0;i<20;++i)q=quality.controller.observe(quality.now+1000*i,quality.sample);
        require(q.bitrate_bps==once.bitrate_bps,"Producer-loop repeats must not accelerate the bitrate controller");

        Simulation network;
        auto n=network.step(30,75000,22,20,300000);
        require(n.bitrate_bps==network.controller.decision().bitrate_bps&&n.bitrate_bps==original,"One send-pressure window must not shrink the budget");
        n=network.step(30,75000,22,20,300000);
        require(n.bitrate_bps<original&&n.rate_limit_millihz<60000,"Sustained send pressure must trade rate for a bounded quality budget");
        require(n.reason==AutoRateReason::QualityFloor,"Quality-preserving FPS limit must be separately diagnosable");
        Simulation high_rate(2560,1440,100000);const auto high_initial=high_rate.controller.bitrate();
        high_rate.step(60,75000,22,20,300000);
        const auto high_cut=high_rate.step(60,75000,22,20,300000);
        require(high_cut.bitrate_bps==uint64_t(high_initial)*75/100,"Above-57-Mbps targets must keep the bounded 25-percent decrease without integer overflow");
        for(int i=0;i<8;++i)n=network.step(30,75000,22,20,300000);
        require(n.bitrate_bps<=12000000&&n.rate_limit_millihz<=20000,"Sustained 12-Mbps delivery must converge with headroom");
        const auto constrained=n;
        const uint32_t flowing=std::max(1u,n.rate_limit_millihz/1000);
        n=network.step(flowing,75000,18,flowing);
        require(n.bitrate_bps==constrained.bitrate_bps&&n.rate_limit_millihz==constrained.rate_limit_millihz,"Network recovery must wait for a healthy interval");
        for(int i=0;i<100;++i){const uint32_t frames=std::max(1u,n.rate_limit_millihz/1000);n=network.step(frames,75000,18,frames);}
        require(n.rate_limit_millihz==60000&&n.bitrate_bps>constrained.bitrate_bps,"Sustained healthy delivery must recover both quality budget and refresh target");

        Simulation idle;
        for(int i=0;i<60;++i)idle.step(i%5==0?1:0,3000,18,i%5==0?1:0);
        require(idle.controller.bitrate()==original&&idle.controller.decision().rate_limit_millihz==60000,"Static desktop goodput must not be mistaken for link capacity");
        Simulation recovery;
        for(int i=0;i<20;++i)recovery.step(30,75000,22,20,500000,true);
        require(recovery.controller.bitrate()==original&&recovery.controller.decision().rate_limit_millihz==60000,"IDR recovery alone must not start a bitrate/FPS collapse");
        Simulation decoder;
        decoder.sample.receiver_queue_us=30000;
        for(int i=0;i<20;++i)decoder.step(30,75000,22,20,500000);
        require(decoder.controller.bitrate()==original,"Decode backlog must not be mislabelled network bandwidth pressure");

        Simulation stale;
        for(int i=0;i<10;++i){
            stale.now+=1000000;stale.sample.encoded_frames+=30;stale.sample.encoded_bytes+=2250000;
            stale.sample.qp_samples+=30;stale.sample.qp_sum+=660;stale.sample.sent_frames+=30;
            stale.sample.send_wait_us+=500000;
            stale.controller.observe(stale.now,stale.sample);
        }
        require(stale.controller.bitrate()==original,"Absent receiver progress must not fabricate network goodput");

        Simulation slow;
        // Large difficult frames on a very narrow link force the quality
        // ceiling below three FPS. One fresh delivered frame still provides
        // enough evidence for a later cautious headroom probe.
        for(int i=0;i<30;++i)slow.step(3,2500000,22,3,500000);
        auto low=slow.controller.decision();
        require(low.rate_limit_millihz<3000,"Low-cadence regression fixture did not constrain the rate");
        const auto old_low=low.rate_limit_millihz;
        for(int i=0;i<20;++i)low=slow.step(1,2500000,18,1);
        require(low.rate_limit_millihz>old_low,"Below-three-FPS quality limit must not become a permanent recovery trap");

        AdaptivePacer repeated(100000,144000);
        repeated.observe(1000000,1000,1000,21000,false,false,1000000,1000000);
        require(repeated.rate()==80000,"First fresh receiver queue must apply one bounded decrease");
        for(uint64_t now=1010000;now<=2000000;now+=10000)
            repeated.observe(now,1000,1000,21000,false,false,now,1000000);
        require(repeated.rate()==80000,"Replaying one receiver report must not repeatedly cut FPS");
        repeated.observe(2100000,1000,1000,21000,false,false,2100000,2100000);
        require(repeated.rate()==64000,"A genuinely new overloaded queue report must still react");
        for(uint64_t now=2150000;now<8000000;now+=50000)
            repeated.observe(now,1000,52863,21000,false,false,now,2100000);
        require(repeated.rate()==100000,"Expired receiver service/queue must not pin the recovery cap");
        AdaptivePacer recover(100000,144000);
        for(uint64_t now=1000000;now<5000000;now+=50000)
            recover.observe(now,1000,1000,0,false,true,now,now);
        require(recover.rate()==100000&&!recover.pressure(),"Repeated fresh recovery flags must not lower FPS or latch slowdown");
        recover.observe(5000000,1000,20000,0,false,true,5000000,5000000);
        require(recover.rate()==45000&&(recover.pressure()&PressureDecode),"Recovery must not hide independently measured decode overload");
        recover.set_external_limit(12000);
        require(recover.target()==100000&&recover.rate()==12000,"Quality ceiling must preserve the negotiated physical target");
        recover.set_external_limit(100000);
        require(recover.rate()==45000,"Removing quality ceiling must retain measured decoder capacity");
        std::cout<<"PASS: automatic bitrate quality/network separation, bounded cadence, static and stale-feedback safety, low-FPS recovery, receiver report deduplication and independent physical target.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
}
