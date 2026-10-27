#include "hivedesk/auto_rate.h"
#include "hivedesk/adaptive.h"
#include <deque>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool pass,const char* message){if(!pass)throw std::runtime_error(message);}
// A deterministic two-slot asynchronous engine. Latency and busy throughput
// are independent; completion = max(submit + latency, previous + service).
// The simulated producer follows the real host's scheduled deadlines and only
// marks slot waits, never content-idle or downstream-credit waits, as pressure.
struct EncoderPipeline {
    struct Job {uint64_t submitted,completed;bool keyframe;};
    hive::AdaptivePacer pacer{240000,100000};
    hive::EncoderCapacity capacity;
    std::deque<Job> jobs;
    uint64_t now=1000000,deadline=now,last_completion=0,next_content=now;
    uint32_t latency,service,color=500;
    size_t submitted=0,completed=0,peak_in_flight=0;
    explicit EncoderPipeline(uint32_t latency_us,uint32_t service_us):latency(latency_us),service(service_us){}
    void observe(){pacer.observe(now,capacity.cost(now),1000,0,false,false,capacity.sample_time(now),now);}
    void run(uint64_t duration,uint32_t content_period){
        const auto until=now+duration;
        while(now<until){
            while(!jobs.empty()&&jobs.front().completed<=now){
                const auto job=jobs.front();jobs.pop_front();++completed;
                capacity.observe(job.completed,job.submitted,color,job.keyframe);
                capacity.end_slot_wait();
            }
            observe();
            if(jobs.size()==2){
                capacity.begin_slot_wait(deadline);
                now=std::min(until,jobs.front().completed);continue;
            }
            const auto capture=std::max({now,deadline,next_content});
            if(!jobs.empty()&&jobs.front().completed<=capture){now=std::min(until,jobs.front().completed);continue;}
            if(capture>=until){now=until;break;}
            now=capture;observe();
            const auto submit=now+color;
            const auto finish=std::max(submit+latency,last_completion+service);
            jobs.push_back({submit,finish,submitted==0});++submitted;last_completion=finish;
            peak_in_flight=std::max(peak_in_flight,jobs.size());
            const auto interval=1000000000ull/pacer.rate();
            deadline+=interval;if(deadline<=now)deadline+=interval*((now-deadline)/interval+1);
            next_content=now+content_period;now=submit;
        }
    }
};
void encoder_capacity_regressions(){
    EncoderPipeline fast(15000,4000);
    fast.run(3000000,50000);
    require(fast.pacer.rate()==100000,"Sparse 15ms residence became a false 60FPS encoder ceiling");
    const auto before=fast.completed;
    fast.run(3000000,0);
    require(fast.pacer.rate()==100000&&fast.completed-before>=295,"Healthy overlapping 100FPS pipeline was limited by latency or paced completion spacing");
    require(fast.peak_in_flight==2,"Latency regression did not exercise the actual two-slot overlap bound");

    EncoderPipeline slow(25000,25000);
    slow.run(6000000,0);
    require(slow.pacer.rate()<60000,"Actual GPU-slot starvation failed to limit encoder demand");
    require(slow.peak_in_flight==2&&slow.jobs.size()<=2,"Capacity adaptation exceeded the two-job ownership bound");
    slow.latency=15000;slow.service=4000;
    slow.run(6000000,0);
    require(slow.pacer.rate()==100000,"A recovered engine stayed capped by its old saturated service estimate");

    hive::EncoderCapacity evidence;
    evidence.observe(1000000,990000,500,false);
    evidence.begin_slot_wait(1005000);
    evidence.observe(1020000,995000,500,false);evidence.end_slot_wait();
    require(evidence.cost(1020000)==20000,"Deadline-crossing saturated completion spacing was not learned");
    // A continuing sparse stream refreshes color work, not an old saturated
    // NVENC bound. Otherwise an under-supplied pipeline cannot probe upward.
    for(uint64_t at=1070000;at<=2070000;at+=50000)evidence.observe(at,at-15000,500,false);
    require(evidence.cost(2070000)==500,"Isolated completions kept an expired NVENC ceiling alive");
    require(evidence.cost(3070001)==0,"Idle producer cost failed to expire");

    hive::EncoderCapacity downstream;
    downstream.observe(1000000,990000,500,false);downstream.begin_slot_wait(1010000);
    downstream.observe(1005000,995000,500,false);
    // A GPU slot is now free, but the host may still await network credit.
    // The second completion must not turn that compound wait into GPU pressure.
    downstream.observe(1015000,1000000,500,false);
    require(downstream.cost(1015000)==500,"Downstream wait after a GPU slot returned polluted encoder capacity");

    hive::EncoderCapacity spike;hive::AdaptivePacer spike_pacer(240000,100000);
    spike.observe(1000000,990000,500,false);spike.begin_slot_wait(1005000);
    spike.observe(1050000,995000,500,false);
    spike_pacer.observe(1050000,spike.cost(1050000),1000,0,false,false,spike.sample_time(1050000),1050000);
    for(uint64_t at=1060000;at<=2500000;at+=10000){
        spike.observe(at,at-4000,500,false);
        spike_pacer.observe(at,spike.cost(at),1000,0,false,false,spike.sample_time(at),at);
        require(spike_pacer.rate()==100000,"Cheap new color samples relabeled one old NVENC spike as sustained pressure");
    }

    hive::EncoderCapacity recovery;
    recovery.observe(1000000,990000,500,false);recovery.begin_slot_wait(1001000);
    recovery.observe(1100000,995000,500,true);recovery.end_slot_wait();
    require(recovery.cost(1100000)==500,"Recovery IDR latency became steady-state capacity");
    hive::AdaptivePacer color_limited(240000,100000);hive::EncoderCapacity color;
    for(uint64_t at=1000000;at<=4000000;at+=50000){
        color.observe(at,at-16000,15000,false);
        color_limited.observe(at,color.cost(at),1000,0,false,false,color.sample_time(at),at);
    }
    require(color_limited.rate()==60000,"Synchronous producer color work lost its capacity protection");
}
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
        encoder_capacity_regressions();
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
        std::cout<<"PASS: two-slot asynchronous latency/throughput separation, sparse-to-100FPS recovery, real encoder starvation, saturated-evidence expiry, color capacity, automatic bitrate quality/network separation, bounded cadence, static/stale-feedback safety, low-FPS recovery and independent physical target.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
}
