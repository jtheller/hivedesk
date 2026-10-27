#pragma once
#include "hivedesk/types.h"
#include <algorithm>
#include <deque>
#include <optional>
#include <stdexcept>

namespace hive {
// Complete encoded frames only. A dropped P picture invalidates every later
// dependent picture, so resume only at a fresh IDR. Never replay a stalled tail.
// Caller serializes access; capture times use this source's steady clock.
class SourceFrameQueue {
public:
    static constexpr size_t capacity=2;
    static constexpr uint64_t max_age_us=100000;
    // Reserve output space before submitting GPU work. A reservation includes
    // the completion in transit after Encoder::receive releases its GPU slot,
    // until push_reserved takes the caller's queue lock. Counting the encoder's
    // in-flight jobs separately would miss that interval and allow overflow.
    bool can_reserve() const{return frames_.size()+reserved_<capacity;}
    size_t reserved() const{return reserved_;}
    bool reserve_completion(){if(!can_reserve())return false;++reserved_;return true;}
    void cancel_completion(){if(!reserved_)throw std::logic_error("Missing source completion reservation");--reserved_;}
    bool push_reserved(EncodedFrame frame,uint64_t now,uint64_t processing_us=0){
        cancel_completion();return push(std::move(frame),now,processing_us);
    }
    bool push(EncodedFrame frame,uint64_t now,uint64_t processing_us=0) {
        if(!valid_frame_timing(frame)||(frame.source_ready_us&&frame.source_ready_us>now)){reset_chain();++discarded_;return false;}
        // Do not turn intrinsically slow encoding into an endless IDR/drop
        // loop. A just-completed job gets at most 20 ms beyond its actual work;
        // the normal 100 ms capture-age budget still applies to fast encoding.
        const auto work=frame.source_ready_us?frame.source_ready_us-frame.capture_time_us:processing_us;
        if(work>MaxSourceWorkUs){reset_chain();++discarded_;return false;}
        const auto budget=std::max(max_age_us,work+20000);
        const auto origin=frame.capture_time_us?frame.capture_time_us:now;
        if(now>=origin&&now-origin>budget){reset_chain();++discarded_;return false;}
        if(frames_.size()>=capacity)reset_chain();
        if(waiting_keyframe_&&!frame.keyframe){++discarded_;return false;}
        if(frame.keyframe){discarded_+=frames_.size();frames_.clear();waiting_keyframe_=false;}
        frames_.push_back({std::move(frame),origin+budget});return true;
    }
    std::optional<EncodedFrame> take(uint64_t now) {
        if(frames_.empty())return {};
        if(now>frames_.front().deadline){reset_chain();return {};}
        auto frame=std::move(frames_.front().frame);frames_.pop_front();return frame;
    }
    bool empty() const{return frames_.empty();}
    size_t size() const{return frames_.size();}
    bool needs_keyframe() const{return waiting_keyframe_;}
    uint64_t discarded() const{return discarded_;}
private:
    void reset_chain(){discarded_+=frames_.size();frames_.clear();waiting_keyframe_=true;}
    struct Pending{EncodedFrame frame;uint64_t deadline;};
    std::deque<Pending> frames_;
    bool waiting_keyframe_=true;
    uint64_t discarded_=0;
    size_t reserved_=0;
};
}
