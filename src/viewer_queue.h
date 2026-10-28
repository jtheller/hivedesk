#pragma once
#include "hivedesk/types.h"
#include "hivedesk/session_watchdog.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>

namespace hive {

// Recent mapping/conversion latency for diagnostics. Blocking map residence is
// not a throughput ceiling; expire old samples while the desktop is idle.
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

// Decode intact references immediately, including bounded late references.
// Presentation has its own tighter deadline and latest-ready replacement.
// Only missing references, ownership overflow or severe backlog reset the chain.
// All methods are called while the MediaViewer mutex is held.
class ViewerQueue {
public:
    static constexpr size_t MaxPending = 4;
    // 50 ms is a local presentation deadline, not a reason to destroy a
    // valid predictive chain. Decode catch-up remains bounded by both count
    // and a one-second excess-transit/local-work ceiling.
    static constexpr uint64_t MaxAgeUs = 50000;
    static constexpr uint64_t MaxReferenceAgeUs = 1000000;
    static constexpr uint64_t RecoveryTimeoutUs = SessionStallUs;
    static constexpr uint64_t BacklogEvidenceUs = 250000;
    struct Pending {
        EncodedFrame frame;
        uint64_t arrival = 0, epoch = 0;
    };
private:
    std::deque<Pending> frames_;
    ViewerTransitClock transit_;
    bool bound_ = false, waiting_keyframe_ = true, recover_ = false, sequenced_ = false;
    bool ready_timing_ = false, presentation_refresh_ = false;
    uint64_t session_ = 0, epoch_ = 1, last_sequence_ = 0, last_capture_ = 0, last_ready_ = 0, discarded_ = 0;
    uint64_t output_ready_epoch_ = 0, output_ready_sequence_ = 0;
    bool recovery_pending_ = false;
    uint64_t recovery_since_ = 0;
    uint64_t network_bytes_=0,network_progress_at_=0;
    bool network_observed_=false;
    uint64_t sequence_gaps_=0,age_expired_=0,queue_overflows_=0,transit_extra_=0;
    uint64_t backlog_at_=0,backlog_us_=0;
    enum class RecoveryReason {Gap,Age,Overflow};
    uint64_t transport_origin(const EncodedFrame& frame) const {
        return ready_timing_ ? frame.source_ready_us : frame.capture_time_us;
    }
    uint64_t age(const Pending& pending, uint64_t now) const {
        const auto local = now >= pending.arrival ? now-pending.arrival : 0;
        if(pending.frame.receive_started_us){
            // Keep pre-packet transit/backlog evidence; exclude only the
            // measured assembly of this packet. Local decoder/Present time
            // still consumes exactly the same 50 ms freshness budget.
            return transit_.extra_age(transport_origin(pending.frame),pending.frame.receive_started_us)+local;
        }
        return std::max(local,transit_.extra_age(transport_origin(pending.frame),now));
    }
    void begin_recovery(uint64_t now) {
        if(!recovery_pending_){recovery_pending_=true;recovery_since_=now;}
    }
    void remember_ready(const Pending& pending) {
        if(output_ready_epoch_!=pending.epoch||pending.frame.sequence>output_ready_sequence_){
            output_ready_epoch_=pending.epoch;output_ready_sequence_=pending.frame.sequence;
        }
    }
    void remember_backlog(uint64_t now,uint64_t waiting) {
        // Only compressed pictures actually waiting here are capacity evidence.
        // Source work, network transit and already-decoded presentation age are
        // different constraints and must not create a decoder throughput cap.
        if(waiting>20000){
            if(!backlog_at_||now<backlog_at_||now-backlog_at_>=BacklogEvidenceUs)backlog_us_=waiting;
            else backlog_us_=std::max(backlog_us_,waiting);
            backlog_at_=now;
        }
    }
    void remember_backlog(uint64_t now){remember_backlog(now,queue_age(now));}
    void invalidate(uint64_t now,RecoveryReason reason) {
        remember_backlog(now);
        if(reason==RecoveryReason::Gap)++sequence_gaps_;
        else if(reason==RecoveryReason::Age)++age_expired_;
        else ++queue_overflows_;
        discarded_ += frames_.size(); frames_.clear(); ++epoch_;
        output_ready_epoch_=output_ready_sequence_=0;
        waiting_keyframe_ = true; recover_ = true; presentation_refresh_ = false; begin_recovery(now);
    }
public:
    void reset() {
        frames_.clear(); transit_ = {}; bound_ = sequenced_ = ready_timing_ = false;
        waiting_keyframe_ = true; recover_ = presentation_refresh_ = false;
        recovery_pending_ = false; recovery_since_ = 0;
        network_bytes_=network_progress_at_=0;network_observed_=false;
        session_ = last_sequence_ = last_capture_ = last_ready_ = discarded_ = 0; ++epoch_;
        output_ready_epoch_=output_ready_sequence_=0;
        sequence_gaps_=age_expired_=queue_overflows_=transit_extra_=0;
        backlog_at_=backlog_us_=0;
    }
    bool accepts_session(uint64_t session) const { return !bound_ || session == session_; }
    bool push(EncodedFrame frame, uint64_t now) {
        if (!accepts_session(frame.session)) { ++discarded_; return false; }
        if (!bound_) { bound_ = true; session_ = frame.session; ready_timing_ = frame.source_ready_us != 0; }
        // TCP preserves order, but reject duplicates/old packets defensively.
        // A missing encoded reference requires IDR; merely skipping presentation
        // does not, because every reference was still decoded in order.
        if (sequenced_ && frame.sequence <= last_sequence_) { ++discarded_; return false; }
        // A stream chooses its clock basis once. Do not let missing/changing
        // provenance (or a malformed future completion) re-anchor a stale tail.
        // Legacy sessions keep the original capture-based policy throughout.
        if ((bool(frame.receive_started_us)!=bool(frame.receive_completed_us))||
            (frame.receive_started_us&&(frame.receive_completed_us<frame.receive_started_us||frame.receive_completed_us>now))||
            !valid_frame_timing(frame) || (ready_timing_ && !frame.source_ready_us) ||
            (last_ready_ && frame.source_ready_us && frame.source_ready_us < last_ready_)) {
            invalidate(now,RecoveryReason::Age); ++discarded_; return false;
        }
        if (sequenced_ && frame.sequence-last_sequence_ != 1 && !frame.keyframe) invalidate(now,RecoveryReason::Gap);
        if (last_capture_ && frame.capture_time_us && frame.capture_time_us < last_capture_) {
            invalidate(now,RecoveryReason::Gap); ++discarded_; return false;
        }
        sequenced_ = true; last_sequence_ = frame.sequence;
        if (frame.capture_time_us) last_capture_ = frame.capture_time_us;
        if (frame.source_ready_us) last_ready_ = frame.source_ready_us;
        // Encoder residence is measured on the source and bounded separately.
        // Only completed-output transport + local residence consume the 50 ms
        // queue budget. A slow valid encode therefore cannot masquerade as an
        // old TCP burst; no timestamps or transit floor are reset on recovery.
        const auto origin=transport_origin(frame);
        const auto arrived=frame.receive_started_us?frame.receive_started_us:now;
        const auto completed=frame.receive_completed_us?frame.receive_completed_us:now;
        transit_.observe(origin,arrived);
        transit_extra_=transit_.extra_age(origin,arrived);
        Pending pending{std::move(frame),completed,epoch_};
        if (age(pending,now) > MaxReferenceAgeUs) {
            invalidate(now,RecoveryReason::Age); ++discarded_; return false;
        }
        if (frames_.size() >= MaxPending) invalidate(now,RecoveryReason::Overflow);
        else if (!frames_.empty() && age(frames_.front(),now) > MaxReferenceAgeUs) invalidate(now,RecoveryReason::Age);
        if (pending.frame.keyframe) {
            // An IDR supersedes any queued reference chain. Invalidate a decode
            // already in flight too, so its late output cannot flash afterward.
            remember_backlog(now);discarded_ += frames_.size(); frames_.clear(); ++epoch_;
            output_ready_epoch_=output_ready_sequence_=0;
            waiting_keyframe_ = recover_ = presentation_refresh_ = false;
        }
        if (waiting_keyframe_) { recover_ = true; begin_recovery(now); ++discarded_; return false; }
        pending.epoch = epoch_; frames_.push_back(std::move(pending)); return true;
    }
    std::optional<Pending> pop(uint64_t now) {
        if (frames_.empty()) return {};
        remember_backlog(now);
        if (age(frames_.front(),now) > MaxReferenceAgeUs) { invalidate(now,RecoveryReason::Age); return {}; }
        auto pending = std::move(frames_.front()); frames_.pop_front(); return pending;
    }
    bool fresh(const Pending& pending, uint64_t now) {
        if (pending.epoch != epoch_ || waiting_keyframe_) return false;
        if (age(pending,now) > MaxReferenceAgeUs) { invalidate(now,RecoveryReason::Age); return false; }
        return true;
    }
    bool admit(const Pending& pending,uint64_t now) {
        if(pending.epoch!=epoch_||waiting_keyframe_)return false;
        // A popped compressed picture can still wait for the decoder's two
        // credits. Count that real local backlog before strict admission, even
        // if the container is now empty; decoded output never calls this path.
        remember_backlog(now,now>=pending.arrival?now-pending.arrival:0);
        return fresh(pending,now);
    }
    bool output_fresh(const Pending& pending, uint64_t now) {
        if (pending.epoch != epoch_ || waiting_keyframe_) return false;
        if (age(pending,now) > MaxReferenceAgeUs || (now>=pending.arrival && now-pending.arrival>MaxAgeUs)) {
            // Mapping and presentation run independently. A late old Present
            // retry cannot re-arm recovery after the mapper has already made a
            // newer picture ready. That newer output still enforces its own age.
            if(output_ready_epoch_==pending.epoch&&output_ready_sequence_>pending.frame.sequence)return false;
            // This picture already reached the decoder, so its references are
            // intact even if conversion/presentation finished too late. Keep
            // subsequent compressed pictures and their epoch. A static desktop
            // still needs a fresh IDR request, with the same recovery deadline.
            if(!presentation_refresh_)++age_expired_;
            presentation_refresh_=recover_=true;begin_recovery(now);return false;
        }
        return true;
    }
    bool presentable(const Pending& pending, uint64_t now) {
        // Skip conversion while newer compressed pictures still need decoding.
        // Once conversion has started, an arriving packet is not a replacement
        // picture yet: presentation only needs the freshness/epoch guard above.
        return output_fresh(pending,now) && frames_.empty();
    }
    bool mark_ready(const Pending& pending,uint64_t now) {
        if(!output_fresh(pending,now))return false;
        remember_ready(pending);
        presentation_refresh_=recover_=recovery_pending_=false;recovery_since_=0;return true;
    }
    bool mark_output_ready(const Pending& pending,uint64_t now) {
        if(!output_fresh(pending,now))return false;
        remember_ready(pending);
        // The next fresh P picture is sufficient: no compressed reference was
        // lost merely because a previous decoded picture missed presentation.
        presentation_refresh_=recover_=recovery_pending_=false;recovery_since_=0;return true;
    }
    bool recovery_pending() const{return recovery_pending_;}
    void network_progress(uint64_t session,uint64_t bytes,uint64_t now){
        if(!bound_||session!=session_)return;
        if(network_observed_&&bytes>network_bytes_)network_progress_at_=now;
        network_bytes_=bytes;network_observed_=true;
    }
    bool recovery_expired(uint64_t now) const {
        if(!recovery_pending_||now<recovery_since_)return false;
        // Slow authenticated-frame assembly can outlast two seconds. Renew
        // only on new bytes for this viewing session, never on control/HUD
        // traffic or repeated polls. A finite ceiling bounds a trickling peer.
        const auto progress=std::max(recovery_since_,network_progress_at_);
        return now-recovery_since_>=30000000||(now>=progress&&now-progress>=RecoveryTimeoutUs);
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
    uint64_t feedback_queue_age(uint64_t now) const {
        // Feedback runs less often than dequeue/recovery. Preserve a real
        // backlog briefly across those operations, without refreshing it when
        // polled or keeping an idle stream constrained by a stale observation.
        const auto recent=backlog_at_&&now>=backlog_at_&&now-backlog_at_<BacklogEvidenceUs?backlog_us_:0;
        return std::max(queue_age(now),recent);
    }
};
}
