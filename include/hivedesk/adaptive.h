#pragma once
#include "types.h"
#include <algorithm>
#include <cstdint>
namespace hive {
// Read-only display query. Never changes a physical or virtual display mode.
uint32_t primary_refresh_millihz();
// Only sustained inability to enqueue a frame is send-path pressure. Always
// sample readiness before observing the pacer so cleared waits cannot reduce it.
class SendBackpressure {
    uint64_t blocked_since_=0;
public:
    bool observe(uint64_t now,bool ready) {
        if(ready){blocked_since_=0;return false;}
        if(!blocked_since_)blocked_since_=now;
        return now-blocked_since_>20000;
    }
};
// Residence latency is not throughput: an asynchronous job may take 15 ms
// while the same engine accepts another every 4 ms. Even overlapping jobs'
// completion spacing can be imposed by our own pacing. Learn NVENC capacity
// only when both GPU slots kept the producer waiting past its next deadline.
// Color conversion is synchronous producer work and has no such ambiguity.
// Caller serializes observations and wait boundaries with the host mutex.
class EncoderCapacity {
    static constexpr uint64_t FreshUs=1000000;
    uint32_t color_us_=0,service_us_=0;
    uint64_t color_at_=0,service_at_=0,previous_completion_=0,wait_deadline_=0;
    bool previous_keyframe_=true;
    static bool fresh(uint64_t at,uint64_t now){return at&&now>=at&&now-at<=FreshUs;}
    static uint32_t smooth(uint32_t previous,uint32_t sample){return previous?(previous*7+sample)/8:sample;}
public:
    void begin_slot_wait(uint64_t deadline){wait_deadline_=deadline;}
    void end_slot_wait(){wait_deadline_=0;}
    void observe(uint64_t completed,uint64_t submitted,uint32_t color_us,bool keyframe){
        // This completion returned a GPU slot even if the host's compound
        // wait remains blocked on downstream credit. It ends the evidence
        // interval; later paced completions need a new actual GPU-slot wait.
        const auto wait_deadline=wait_deadline_;wait_deadline_=0;
        color_us_=smooth(fresh(color_at_,completed)?color_us_:0,std::min(color_us,1000000u));
        color_at_=completed;
        if(wait_deadline&&completed>wait_deadline&&previous_completion_&&
            completed>=previous_completion_&&!keyframe&&!previous_keyframe_&&submitted<=previous_completion_){
            const auto service=uint32_t(std::min<uint64_t>(1000000,completed-previous_completion_));
            service_us_=smooth(fresh(service_at_,completed)?service_us_:0,service);
            service_at_=completed;
        }
        previous_completion_=completed;previous_keyframe_=keyframe;
    }
    uint32_t cost(uint64_t now)const{
        // Without new saturated evidence this is an unknown bound, not a
        // permanent ceiling. The pacer's existing bounded probes can retry.
        return std::max(fresh(color_at_,now)?color_us_:0u,fresh(service_at_,now)?service_us_:0u);
    }
    uint64_t sample_time(uint64_t now)const{
        const auto color=fresh(color_at_,now)?color_us_:0u;
        // Cheap new color work cannot relabel one old NVENC spike as repeated
        // evidence of sustained overload. Date the component that sets cost().
        if(fresh(service_at_,now)&&service_us_>color)return service_at_;
        return fresh(color_at_,now)?color_at_:0;
    }
};
class AdaptivePacer {
    uint32_t target_,rate_,pressure_=0,external_limit_;
    uint64_t last_change_=0,last_pressure_=0,last_receiver_pressure_=0;
    uint64_t encoder_pressure_since_=0,last_encoder_sample_=0,last_receiver_sample_=0;
    uint32_t encoder_pressure_samples_=0;
    bool encoder_pressure_active_=false;
public:
    AdaptivePacer(uint32_t source,uint32_t receiver):target_(std::min(source,receiver)),rate_(target_),external_limit_(target_){}
    void observe(uint64_t now,uint32_t encode_us,uint32_t decode_us,uint32_t queue_us,bool network,bool recovery,
        uint64_t encode_sample_time_us=0,uint64_t receiver_report_time_us=0) {
        // The host also observes while waiting for capture or send credit. A
        // cached sample must not become evidence of a sustained encoder stall.
        // Zero preserves the implicit-fresh behavior of callers without a clock.
        const bool encode_fresh=!encode_sample_time_us||
            (now>=encode_sample_time_us&&now-encode_sample_time_us<=1000000);
        const bool new_encode=encode_fresh&&(!encode_sample_time_us||encode_sample_time_us!=last_encoder_sample_);
        if(new_encode&&encode_sample_time_us)last_encoder_sample_=encode_sample_time_us;
        if(!encode_fresh)encode_us=0;
        // A receiver report is evidence once, not every producer-loop turn.
        // Expire its service bound too: otherwise one bad old sample prevents
        // every upward probe even after it can no longer trigger decreases.
        const bool receiver_fresh=!receiver_report_time_us||
            (now>=receiver_report_time_us&&now-receiver_report_time_us<=1000000);
        const bool new_receiver=receiver_fresh&&(!receiver_report_time_us||receiver_report_time_us!=last_receiver_sample_);
        if(new_receiver&&receiver_report_time_us)last_receiver_sample_=receiver_report_time_us;
        if(!receiver_fresh)decode_us=queue_us=0;
        // Recovery is a reference-chain request, not measured throughput.
        // Independent fresh service/queue overload remains actionable.
        (void)recovery;
        const auto service_capacity=[this](uint32_t cost){return cost?
            uint32_t(std::min<uint64_t>(target_,900000000ull/cost)):target_;};
        const uint32_t encode_capacity=service_capacity(encode_us),decode_capacity=service_capacity(decode_us);
        const uint32_t capacity=std::min(encode_capacity,decode_capacity);
        const bool encoder_limited=encode_capacity<rate_;
        if(encoder_limited) {
            last_pressure_=now;
            if(new_encode) {
                if(!encoder_pressure_active_){encoder_pressure_active_=true;encoder_pressure_since_=now;encoder_pressure_samples_=0;}
                if(encoder_pressure_samples_<3)++encoder_pressure_samples_;
            }
        }else {encoder_pressure_active_=false;encoder_pressure_samples_=0;}
        const bool sustained_encode=encoder_limited&&new_encode&&encoder_pressure_active_&&
            encoder_pressure_samples_>=3&&now-encoder_pressure_since_>=500000;
        uint32_t pressure=0;
        if(new_receiver&&decode_capacity<rate_)pressure|=PressureDecode;
        if(new_receiver&&queue_us>20000)pressure|=PressureDecode;
        if(network)pressure|=PressureNetwork;
        if(pressure) {
            // Real receiver backlog/send blocking still reacts immediately.
            last_pressure_=last_receiver_pressure_=now;pressure_=pressure;
            if(now-last_change_<250000)return;
            uint32_t next=std::min(rate_,decode_capacity);
            if(network||(new_receiver&&queue_us>20000))next=std::min(next,rate_*80/100);
            if(sustained_encode){next=std::min(next,std::max(encode_capacity,rate_*80/100));pressure_|=PressureEncode;}
            rate_=std::clamp(next,std::min(1000u,target_),target_);last_change_=now;
        }else if(encoder_limited) {
            // Finite GPU slots already bound asynchronous production. A short
            // scheduling spike does not justify a lasting frame-rate cliff.
            if(sustained_encode&&now-last_change_>=500000) {
                rate_=std::clamp(std::max(encode_capacity,rate_*80/100),std::min(1000u,target_),target_);
                pressure_=PressureEncode;last_change_=now;
            }
        }else if(now-last_pressure_>=1000000&&
            (!last_receiver_pressure_||now-last_receiver_pressure_>=2000000)&&now-last_change_>=500000) {
            // Probe every half second after pressure clears, bounded by both
            // stages' measured capacity and the physical min-refresh target.
            const auto next=std::min(capacity,std::min(target_,rate_+std::max(5000u,rate_/5)));
            if(next>rate_){rate_=next;last_change_=now;}
            if(rate_==target_)pressure_=0;
        }
    }
    // The quality/network controller owns this ceiling independently. Raising
    // it cannot erase a real encoder/decoder limit, nor change the physical
    // min-refresh target reported to the UI.
    void set_external_limit(uint32_t millihz){external_limit_=std::clamp(millihz,std::min(1000u,target_),target_);}
    uint32_t target()const{return target_;}
    uint32_t intrinsic_rate()const{return rate_;}
    uint32_t rate()const{return std::min(rate_,external_limit_);}
    uint32_t pressure()const{return pressure_|(external_limit_<target_?PressureNetwork:0);}
};
class SlowIndicator {
    bool visible_=false;
    uint64_t bad_since_=0,good_since_=0;
public:
    void reset(){*this={};}
    bool update(uint64_t now,const StreamHealth& health) {
        const bool bad=health.target_millihz&&uint64_t(health.rate_millihz)*100<uint64_t(health.target_millihz)*80;
        if(bad){good_since_=0;if(!bad_since_)bad_since_=now;if(now-bad_since_>=1000)visible_=true;}
        else {bad_since_=0;if(!good_since_)good_since_=now;if(now-good_since_>=3000)visible_=false;}
        return visible_;
    }
};
bool adaptive_self_test(std::string&);
}
