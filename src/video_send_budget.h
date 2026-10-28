#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace hive {
struct VideoTcpSample {
    bool available=false;
    uint32_t rtt_us=0,min_rtt_us=0,ideal_bytes=0,cwnd_bytes=0,send_window_bytes=0,inflight_bytes=0;
};
// Keep the original two writes on low-RTT or unobservable connections. For
// sustained propagation delay, Windows' BDP estimate is a byte budget, NOT a
// request to queue an arbitrary number of video frames. Congestion/receive
// windows can only reduce that estimate. Admission never changes frame order.
class VideoSendBudget {
    VideoTcpSample sample_{};
public:
    static constexpr size_t BaselineWrites=2,MaximumWrites=32;
    static constexpr uint64_t MaximumBytes=16ull*1024*1024;
    void observe(VideoTcpSample sample){sample_=sample;}
    bool observed_path()const{
        return sample_.available&&sample_.min_rtt_us>=12000&&sample_.rtt_us<=10000000&&sample_.rtt_us>=sample_.min_rtt_us&&
            sample_.ideal_bytes&&sample_.cwnd_bytes&&sample_.send_window_bytes;
    }
    bool adaptive()const{return observed_path()&&sample_.rtt_us-sample_.min_rtt_us<=50000;}
    uint64_t bytes()const{
        return observed_path()?std::min({MaximumBytes,uint64_t(sample_.ideal_bytes),uint64_t(sample_.cwnd_bytes),uint64_t(sample_.send_window_bytes)}):0;
    }
    size_t limit()const{return adaptive()?MaximumWrites:BaselineWrites;}
    bool admits(size_t owned,uint64_t owned_bytes,uint64_t next_bytes)const{
        // One oversized picture can always make progress. On an observed slow
        // path, however, do not put a second picture behind more than one TCP
        // window of data. Two unconditional writes turn serialization into
        // stale reference pictures and repeated, still larger recovery IDRs.
        if(!owned)return true;
        if(observed_path()&&owned_bytes>=bytes())return false;
        // Preserve existing large-IDR behavior, including when a receive window
        // closes. TCP handles it and the existing no-progress watchdog remains.
        if(owned<BaselineWrites)return true;
        const auto budget=bytes();
        return owned<limit()&&next_bytes<=budget&&owned_bytes<=budget-next_bytes;
    }
};
}
