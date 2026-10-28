#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hive {
// Provisional visual-quality guardrails, not a claim of mathematical lossless
// coding. The encoder retains 4:4:4 and the source resolution at every rate.
inline constexpr uint32_t AutoRateMinQp=16,AutoRateTargetQp=18,AutoRateMaxQp=22;
inline constexpr uint32_t AutoRateMinBitrate=1000000,AutoRateMaxBitrate=1000000000;
inline uint32_t initial_auto_bitrate(uint32_t width,uint32_t height,uint32_t target_millihz){
    // 0.25 bits / pixel / frame is only the starting estimate. Real encoded
    // frame sizes and QPs replace it; static-desktop goodput never does.
    const auto estimate=uint64_t(width)*height*target_millihz/4000;
    return uint32_t(std::clamp<uint64_t>(estimate,8000000,500000000));
}
enum class AutoRateReason : uint32_t { Stable=0,Quality=1,Network=2,QualityFloor=3,Probe=4 };
inline const char* auto_rate_reason_name(AutoRateReason reason){
    switch(reason){
    case AutoRateReason::Quality:return "quality";
    case AutoRateReason::Network:return "network";
    case AutoRateReason::QualityFloor:return "quality_floor";
    case AutoRateReason::Probe:return "probe";
    default:return "stable";
    }
}
struct AutoRateSample {
    // Session-cumulative P-frame counters. Exclude IDRs from the QP/frame-size
    // model because a recovery keyframe is not steady-state quality demand.
    uint64_t encoded_frames=0,encoded_bytes=0,qp_sum=0,qp_samples=0;
    // All-frame delivery/receive counters and total producer send-credit wait.
    uint64_t sent_frames=0,send_wait_us=0,received_bytes=0,received_frames=0;
    uint64_t receiver_report_id=0;
    uint32_t receive_sample_age_us=UINT32_MAX,receiver_queue_us=0;
    // Actual current pacing, including a hardware limit. This avoids raising
    // the bitrate for hypothetical frames the encoder cannot currently make.
    uint32_t pacing_millihz=0;
    bool recovering=false,transport_details=false;
    uint64_t transport_bytes=0,transport_sample_us=0;
};
struct AutoRateDecision {
    uint32_t bitrate_bps=0,rate_limit_millihz=0;
    AutoRateReason reason=AutoRateReason::Stable;
};
class AutoRateController {
    static constexpr uint64_t WindowUs=1000000;
    uint32_t target_;
    AutoRateDecision decision_;
    AutoRateSample previous_{};
    uint64_t last_window_=0,last_pressure_=0,last_probe_=0;
    bool initialized_=false,have_quality_=false;
    unsigned pressure_windows_=0;
    double quality_bits_per_frame_=0;
    uint32_t network_budget_=AutoRateMaxBitrate;

    static uint64_t delta(uint64_t value,uint64_t previous){return value>=previous?value-previous:0;}
    uint32_t quality_ceiling()const{
        // At six QP steps, the customary rate/quantization model is a factor
        // of two. It is a smoothed estimate only; observed max-QP pressure
        // continually updates it rather than trusting a pixel heuristic.
        const double minimum_frame_bits=quality_bits_per_frame_*std::exp2((double(AutoRateTargetQp)-AutoRateMaxQp)/6.0);
        if(minimum_frame_bits<=0)return target_;
        const double limit=double(std::min(network_budget_,decision_.bitrate_bps))*1000.0/minimum_frame_bits;
        return uint32_t(std::clamp(limit,1000.0,double(target_)));
    }
public:
    AutoRateController(uint32_t width,uint32_t height,uint32_t target_millihz):
        target_(std::max(1000u,target_millihz)),
        decision_{initial_auto_bitrate(width,height,target_),target_,AutoRateReason::Stable},
        quality_bits_per_frame_(double(decision_.bitrate_bps)*1000.0/target_){}
    const AutoRateDecision& decision()const{return decision_;}
    uint32_t bitrate()const{return decision_.bitrate_bps;}
    AutoRateDecision observe(uint64_t now,const AutoRateSample& sample){
        if(!initialized_||now<last_window_){initialized_=true;last_window_=now;previous_=sample;return decision_;}
        if(now-last_window_<WindowUs)return decision_;
        const uint64_t elapsed=now-last_window_;
        const uint64_t frames=delta(sample.encoded_frames,previous_.encoded_frames);
        const uint64_t bytes=delta(sample.encoded_bytes,previous_.encoded_bytes);
        const uint64_t qps=delta(sample.qp_samples,previous_.qp_samples);
        const uint64_t qp_sum=delta(sample.qp_sum,previous_.qp_sum);
        const uint64_t sent=delta(sample.sent_frames,previous_.sent_frames);
        const uint64_t received=delta(sample.received_frames,previous_.received_frames);
        const uint64_t received_bytes=delta(sample.received_bytes,previous_.received_bytes);
        const uint64_t wait=std::min(elapsed,delta(sample.send_wait_us,previous_.send_wait_us));
        const auto wire_bytes=delta(sample.transport_bytes,previous_.transport_bytes);
        const auto wire_elapsed=delta(sample.transport_sample_us,previous_.transport_sample_us);
        const bool wire_fresh=sample.transport_details&&previous_.transport_details&&
            sample.receiver_report_id&&sample.receiver_report_id!=previous_.receiver_report_id&&
            sample.transport_bytes>=previous_.transport_bytes&&wire_elapsed>=500000&&wire_elapsed<=3000000;
        const bool fresh=sample.receiver_report_id&&sample.receiver_report_id!=previous_.receiver_report_id&&sample.receive_sample_age_us<=WindowUs;
        previous_=sample;last_window_=now;
        const uint32_t pacing=std::clamp(sample.pacing_millihz?sample.pacing_millihz:target_,1000u,target_);
        // A quality-constrained one-FPS stream must still be able to recover.
        // At normal rates require three frames; at a genuinely low configured
        // cadence require proportional progress rather than waiting forever.
        const auto minimum_progress=std::clamp<uint32_t>(pacing/10000,1,3);
        const bool active=frames>=minimum_progress&&sent>=minimum_progress;
        const bool quality_sample=active&&qps>=minimum_progress&&qp_sum<=qps*51&&bytes>0&&!sample.recovering;
        const double average_qp=qps?double(qp_sum)/double(qps):0.0;
        if(quality_sample){
            const double observed=double(bytes)*8.0/double(frames)*std::exp2((average_qp-AutoRateTargetQp)/6.0);
            // Content can get harder suddenly. Relax an established quality
            // estimate slowly so one easy frame does not open an FPS cliff.
            const double confidence=std::min(1.0,double(qps)/3.0);
            const double weight=!have_quality_?1.0:(observed>quality_bits_per_frame_?0.5:0.1)*confidence;
            quality_bits_per_frame_+=weight*(observed-quality_bits_per_frame_);have_quality_=true;
        }
        // Receiver decode queues and IDR recovery are not bandwidth evidence.
        // Require two active windows of actual local send-credit blocking and
        // advancing receiver delivery before learning a network budget.
        // Transport progress is independent of complete pictures and decoder
        // recovery. Require sustained producer pressure plus actual bytes;
        // silence, stale reports and decode overload are never capacity evidence.
        const bool wire_pressure=wire_fresh&&wire_bytes>=32768&&wait>elapsed*15/100&&sample.receiver_queue_us<20000;
        const bool network_pressure=wire_pressure||(!sample.transport_details&&fresh&&active&&received>=minimum_progress&&received_bytes&&wait>elapsed*15/100&&
            sample.receiver_queue_us<20000&&!sample.recovering);
        if(network_pressure)++pressure_windows_;else pressure_windows_=0;
        if(network_pressure)last_pressure_=now;
        if(pressure_windows_>=2){
            const double goodput=wire_pressure?double(wire_bytes)*8000000.0/double(wire_elapsed):double(received_bytes)*8000000.0/double(elapsed);
            const auto budget=uint32_t(std::clamp(goodput*0.90,double(AutoRateMinBitrate),double(AutoRateMaxBitrate)));
            // Bound each decrease. Receiver bytes are windowed feedback and
            // may be bursty even when the sender itself is well paced.
            const auto bounded_drop=uint32_t(uint64_t(decision_.bitrate_bps)*75/100);
            network_budget_=std::min(network_budget_,std::max(budget,bounded_drop));
            decision_.bitrate_bps=std::min(decision_.bitrate_bps,network_budget_);
            if(have_quality_)decision_.rate_limit_millihz=std::min(decision_.rate_limit_millihz,quality_ceiling());
            decision_.reason=decision_.rate_limit_millihz<target_?AutoRateReason::QualityFloor:AutoRateReason::Network;
            last_probe_=now;
            return decision_;
        }
        // A quiet/static desktop is application-limited. Preserve every
        // learned budget and cap instead of interpreting its tiny goodput as
        // the link's capacity; no idle-driven bitrate or frame-rate decrease.
        if(!active||sample.recovering)return decision_;
        const bool healthy=fresh&&received>=minimum_progress&&received+2>=sent&&wait<elapsed*5/100&&sample.receiver_queue_us<10000;
        if(healthy&&(!last_pressure_||now-last_pressure_>=3000000)&&
            (!last_probe_||now-last_probe_>=2000000)&&network_budget_<AutoRateMaxBitrate){
            network_budget_=uint32_t(std::min<uint64_t>(AutoRateMaxBitrate,uint64_t(network_budget_)*110/100));
            const auto desired=quality_ceiling();
            if(desired>decision_.rate_limit_millihz)
                decision_.rate_limit_millihz=std::min(desired,decision_.rate_limit_millihz+std::max(5000u,decision_.rate_limit_millihz/10));
            // Probe the rate at which traffic is actually supplied, otherwise
            // an idle budget increase cannot discover additional headroom.
            if(quality_sample&&average_qp>=AutoRateTargetQp){
                const auto demand=uint32_t(std::clamp(quality_bits_per_frame_*double(target_)/1000.0,
                    double(AutoRateMinBitrate),double(AutoRateMaxBitrate)));
                const auto probe=std::min(demand,uint32_t(std::min<uint64_t>(AutoRateMaxBitrate,uint64_t(decision_.bitrate_bps)*110/100)));
                // Learning that the link is faster is not a reason to spend
                // unlimited bits once this content already meets the target.
                if(probe>decision_.bitrate_bps)decision_.bitrate_bps=std::min(network_budget_,probe);
            }
            decision_.reason=AutoRateReason::Probe;last_probe_=now;
        }
        if(quality_sample&&average_qp>=AutoRateTargetQp+1.0&&!network_pressure&&healthy){
            const auto needed=uint32_t(std::clamp(quality_bits_per_frame_*double(pacing)/1000.0,
                double(AutoRateMinBitrate),double(AutoRateMaxBitrate)));
            const auto step=uint32_t(std::min<uint64_t>(AutoRateMaxBitrate,uint64_t(decision_.bitrate_bps)*1125/1000));
            const auto next=std::min(network_budget_,std::min(needed,step));
            if(next>decision_.bitrate_bps){decision_.bitrate_bps=next;decision_.reason=AutoRateReason::Quality;}
        }
        if(quality_sample&&(network_budget_<AutoRateMaxBitrate||decision_.bitrate_bps==AutoRateMaxBitrate)){
            const auto ceiling=quality_ceiling();
            if(ceiling<decision_.rate_limit_millihz){
                decision_.rate_limit_millihz=ceiling;decision_.reason=AutoRateReason::QualityFloor;
            }
        }
        return decision_;
    }
};
}
