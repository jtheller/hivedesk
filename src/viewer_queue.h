#pragma once
#include "hivedesk/types.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>

namespace hive {

// Decoder capacity belongs to current work, not an old expensive picture left
// in a count-only ring while a desktop is idle.
class ViewerServiceWindow {
    struct Sample {uint64_t at;uint32_t cost;};
    std::deque<Sample> samples_;
    void expire(uint64_t now) {
        while(!samples_.empty()&&now>=samples_.front().at&&now-samples_.front().at>1000000)samples_.pop_front();
    }
public:
    void clear(){samples_.clear();}
    void add(uint64_t now,uint32_t cost){expire(now);samples_.push_back({now,cost});if(samples_.size()>64)samples_.pop_front();}
    uint32_t p90(uint64_t now){expire(now);if(samples_.size()<8)return 0;std::vector<uint32_t> costs;for(const auto& s:samples_)costs.push_back(s.cost);std::sort(costs.begin(),costs.end());return costs[(costs.size()-1)*9/10];}
    uint32_t sample_age(uint64_t now){expire(now);return samples_.empty()?UINT32_MAX:uint32_t(std::min<uint64_t>(UINT32_MAX,now>=samples_.back().at?now-samples_.back().at:0));}
};

// Intervals between successful Present submissions, not physical scanout.
// Keep raw intervals (including idle) so callers can interpret them alongside
// receive FPS, instead of silently treating a static desktop as dropped frames.
class ViewerPresentWindow {
    struct Sample {uint64_t at;uint32_t interval;};
    std::deque<Sample> samples_;
    uint64_t previous_=0;
    void expire(uint64_t now){while(!samples_.empty()&&now>=samples_.front().at&&now-samples_.front().at>1000000)samples_.pop_front();}
public:
    void clear(){samples_.clear();previous_=0;}
    void presented(uint64_t now){
        expire(now);
        if(previous_&&now>=previous_){samples_.push_back({now,uint32_t(std::min<uint64_t>(UINT32_MAX,now-previous_))});if(samples_.size()>256)samples_.pop_front();}
        previous_=now;
    }
    uint32_t p95(uint64_t now){
        expire(now);if(samples_.empty())return 0;
        std::vector<uint32_t> intervals;for(const auto& sample:samples_)intervals.push_back(sample.interval);
        std::sort(intervals.begin(),intervals.end());return intervals[((intervals.size()*95+99)/100)-1];
    }
    uint32_t maximum(uint64_t now){expire(now);uint32_t result=0;for(const auto& sample:samples_)result=std::max(result,sample.interval);return result;}
};

// The sender and viewer steady clocks have unrelated origins. Compare elapsed
// intervals within each clock, then subtract the best observed transit. This is
// extra transit delay, not a claim to know absolute one-way network latency.
class ViewerTransitClock {
    bool anchored_ = false;
    uint64_t source_anchor_ = 0, local_anchor_ = 0, last_local_ = 0;
    int64_t floor_ = 0;
    static int64_t delta(uint64_t value, uint64_t origin) {
        constexpr auto limit = uint64_t(std::numeric_limits<int64_t>::max() / 2);
        return value >= origin ? int64_t(std::min(value-origin,limit)) : -int64_t(std::min(origin-value,limit));
    }
    int64_t relative(uint64_t source, uint64_t local) const {
        return delta(local,local_anchor_) - delta(source,source_anchor_);
    }
public:
    void observe(uint64_t source, uint64_t local) {
        if (!source) return; // Legacy packets without capture timestamps.
        if (!anchored_) {
            anchored_ = true; source_anchor_ = source; local_anchor_ = last_local_ = local; floor_ = 0;
            return;
        }
        const auto transit = relative(source,local);
        // Track ordinary oscillator drift (at most 500 ppm) only close to the
        // healthy baseline. A stalled TCP burst must never reset that baseline.
        const auto elapsed = local >= last_local_ ? local-last_local_ : 0;
        if (transit > floor_ && transit-floor_ < 10000)
            floor_ += int64_t(std::min<uint64_t>(elapsed,1000000) / 2000);
        floor_ = std::min(floor_,transit); last_local_ = local;
    }
    uint64_t extra_age(uint64_t source, uint64_t local) const {
        if (!anchored_ || !source) return 0;
        const auto transit = relative(source,local);
        return transit > floor_ ? uint64_t(transit-floor_) : 0;
    }
};

// No playback/jitter buffer: consume immediately, with a hard bound on both
// pending compressed pictures and their age. Removing a reference picture
// invalidates the whole dependent chain until a fresh IDR arrives.
// All methods are called while the MediaViewer mutex is held.
class ViewerQueue {
public:
    static constexpr size_t MaxPending = 4;
    static constexpr uint64_t MaxAgeUs = 50000;
    static constexpr uint64_t RecoveryTimeoutUs = 2000000;
    struct Pending {
        EncodedFrame frame;
        uint64_t arrival = 0, epoch = 0;
    };
private:
    std::deque<Pending> frames_;
    ViewerTransitClock transit_;
    bool bound_ = false, waiting_keyframe_ = true, recover_ = false, sequenced_ = false;
    uint64_t session_ = 0, epoch_ = 1, last_sequence_ = 0, last_capture_ = 0, discarded_ = 0;
    bool recovery_pending_ = false;
    uint64_t recovery_since_ = 0;
    uint64_t sequence_gaps_=0,age_expired_=0,queue_overflows_=0,transit_extra_=0;
    enum class RecoveryReason {Gap,Age,Overflow};
    uint64_t age(const Pending& pending, uint64_t now) const {
        const auto local = now >= pending.arrival ? now-pending.arrival : 0;
        return std::max(local,transit_.extra_age(pending.frame.capture_time_us,now));
    }
    void begin_recovery(uint64_t now) {
        if(!recovery_pending_){recovery_pending_=true;recovery_since_=now;}
    }
    void invalidate(uint64_t now,RecoveryReason reason) {
        if(reason==RecoveryReason::Gap)++sequence_gaps_;
        else if(reason==RecoveryReason::Age)++age_expired_;
        else ++queue_overflows_;
        discarded_ += frames_.size(); frames_.clear(); ++epoch_;
        waiting_keyframe_ = true; recover_ = true;begin_recovery(now);
    }
public:
    void reset() {
        frames_.clear(); transit_ = {}; bound_ = sequenced_ = false;
        waiting_keyframe_ = true; recover_ = false;
        recovery_pending_ = false; recovery_since_ = 0;
        session_ = last_sequence_ = last_capture_ = discarded_ = 0; ++epoch_;
        sequence_gaps_=age_expired_=queue_overflows_=transit_extra_=0;
    }
    bool accepts_session(uint64_t session) const { return !bound_ || session == session_; }
    bool push(EncodedFrame frame, uint64_t now) {
        if (!accepts_session(frame.session)) { ++discarded_; return false; }
        if (!bound_) { bound_ = true; session_ = frame.session; }
        // TCP preserves order, but reject duplicates/old packets defensively.
        // A missing encoded reference requires IDR; merely skipping presentation
        // does not, because every reference was still decoded in order.
        if (sequenced_ && frame.sequence <= last_sequence_) { ++discarded_; return false; }
        if (sequenced_ && frame.sequence-last_sequence_ != 1 && !frame.keyframe) invalidate(now,RecoveryReason::Gap);
        if (last_capture_ && frame.capture_time_us && frame.capture_time_us < last_capture_) {
            invalidate(now,RecoveryReason::Gap); ++discarded_; return false;
        }
        sequenced_ = true; last_sequence_ = frame.sequence;
        if (frame.capture_time_us) last_capture_ = frame.capture_time_us;
        transit_.observe(frame.capture_time_us,now);
        transit_extra_=transit_.extra_age(frame.capture_time_us,now);
        Pending pending{std::move(frame),now,epoch_};
        if (age(pending,now) > MaxAgeUs) {
            invalidate(now,RecoveryReason::Age); ++discarded_; return false;
        }
        if (frames_.size() >= MaxPending) invalidate(now,RecoveryReason::Overflow);
        else if (!frames_.empty() && age(frames_.front(),now) > MaxAgeUs) invalidate(now,RecoveryReason::Age);
        if (pending.frame.keyframe) {
            // An IDR supersedes any queued reference chain. Invalidate a decode
            // already in flight too, so its late output cannot flash afterward.
            discarded_ += frames_.size(); frames_.clear(); ++epoch_;
            waiting_keyframe_ = recover_ = false;
        }
        if (waiting_keyframe_) { recover_ = true; begin_recovery(now); ++discarded_; return false; }
        pending.epoch = epoch_; frames_.push_back(std::move(pending)); return true;
    }
    std::optional<Pending> pop(uint64_t now) {
        if (frames_.empty()) return {};
        if (age(frames_.front(),now) > MaxAgeUs) { invalidate(now,RecoveryReason::Age); return {}; }
        auto pending = std::move(frames_.front()); frames_.pop_front(); return pending;
    }
    bool fresh(const Pending& pending, uint64_t now) {
        if (pending.epoch != epoch_ || waiting_keyframe_) return false;
        if (age(pending,now) > MaxAgeUs) { invalidate(now,RecoveryReason::Age); return false; }
        return true;
    }
    bool presentable(const Pending& pending, uint64_t now) {
        // Skip conversion while newer compressed pictures still need decoding.
        // Once conversion has started, an arriving packet is not a replacement
        // picture yet: presentation only needs the freshness/epoch guard above.
        return fresh(pending,now) && frames_.empty();
    }
    bool mark_ready(const Pending& pending,uint64_t now) {
        if(!fresh(pending,now))return false;
        recovery_pending_=false;recovery_since_=0;return true;
    }
    bool recovery_pending() const{return recovery_pending_;}
    bool recovery_expired(uint64_t now) const {
        return recovery_pending_&&now>=recovery_since_&&now-recovery_since_>=RecoveryTimeoutUs;
    }
    bool empty() const { return frames_.empty(); }
    size_t size() const { return frames_.size(); }
    bool recovering() const { return recover_; }
    uint64_t discarded() const { return discarded_; }
    uint64_t sequence_gaps() const{return sequence_gaps_;}
    uint64_t age_expired() const{return age_expired_;}
    uint64_t queue_overflows() const{return queue_overflows_;}
    uint32_t transit_extra_us() const{return uint32_t(std::min<uint64_t>(UINT32_MAX,transit_extra_));}
    uint64_t queue_age(uint64_t now) const {
        return frames_.empty() || now < frames_.front().arrival ? 0 : now-frames_.front().arrival;
    }
};
}
