#include "../src/frame_timer.h"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
double milliseconds(Clock::duration elapsed){return std::chrono::duration<double,std::milli>(elapsed).count();}
template<class Wait>std::vector<double> measure(Wait wait){
    std::vector<double> samples;
    for(int sample=0;sample<48;++sample){
        const auto begin=Clock::now(),deadline=begin+10ms;wait(deadline);
        samples.push_back(milliseconds(Clock::now()-begin));
    }
    std::sort(samples.begin(),samples.end());return samples;
}
}
int main(){try {
    // Deterministic pacing trace: 0.5 ms dispatch jitter must not accumulate
    // into a 95 Hz schedule when the selected target is 100 Hz.
    const Clock::time_point origin{};
    auto scheduled=origin;
    for(int frame=0;frame<1000;++frame){
        const auto started=scheduled+500us;
        scheduled=hive::advance_frame_deadline(scheduled,started,10ms);
        require(scheduled==origin+10ms*(frame+1),"Dispatch jitter accumulated into slower frame pacing");
        require(scheduled>started,"Next ordinary frame deadline was already expired");
    }
    // One admitted frame after a stall/idle gap skips every missed tick. The
    // next frame still waits for the next phase; there is no catch-up train.
    scheduled=origin+10ms;
    scheduled=hive::advance_frame_deadline(scheduled,origin+105ms,10ms);
    require(scheduled==origin+110ms,"Stall left an expired deadline or reset cadence phase");
    scheduled=hive::advance_frame_deadline(scheduled,origin+110ms,10ms);
    require(scheduled==origin+120ms,"Stall recovery admitted a catch-up frame");
    scheduled=hive::advance_frame_deadline(scheduled,origin+4000500us,10ms);
    require(scheduled==origin+4010ms,"Idle resume replayed elapsed frame ticks");
    // Being exactly on a later phase still advances beyond the frame just
    // admitted, while a changed rate uses that new interval immediately.
    require(hive::advance_frame_deadline(origin,origin+100ms,10ms)==origin+110ms,"Exact-phase stall admitted the same tick twice");
    require(hive::advance_frame_deadline(origin+10ms,origin+10500us,20ms)==origin+30ms,"New slower rate was ignored");
    require(hive::advance_frame_deadline(origin+10ms,origin+10500us,5ms)==origin+15ms,"New faster rate was ignored");
    require(hive::advance_frame_deadline(origin,origin+100ms,0ms)==origin+100ms,"Unpaced diagnostic gained an artificial wait");

    hive::FrameTimer timer;
    // Covers cancellation published before the worker enters its native wait.
    timer.notify();
    auto begin=Clock::now();
    require(timer.wait_until(begin+5s)==hive::FrameTimer::Result::Wake,"Pre-wait notification was lost");
    require(Clock::now()-begin<1s,"Pre-wait notification waited for deadline");

    std::promise<void> entering;auto entered=entering.get_future();
    auto waiter=std::async(std::launch::async,[&]{entering.set_value();return timer.wait_until(Clock::now()+5s);});
    entered.wait();Sleep(20);timer.notify();
    require(waiter.wait_for(1s)==std::future_status::ready&&waiter.get()==hive::FrameTimer::Result::Wake,"In-wait cancellation did not wake");

    // Stale timer signals must not leak into the next arm. Multiple control
    // notifications coalesce; a second wait should reach its real deadline.
    for(int round=0;round<16;++round){
        timer.notify();timer.notify();
        require(timer.wait_until(Clock::now()+5s)==hive::FrameTimer::Result::Wake,"Rearmed cancellation was lost");
        const auto deadline=Clock::now()+2ms;
        require(timer.wait_until(deadline)==hive::FrameTimer::Result::Deadline,"Unexpected stale notification after rearm");
        require(Clock::now()>=deadline,"Timer expired before requested deadline");
    }
    require(timer.wait_until(Clock::now()-1ms)==hive::FrameTimer::Result::Deadline,"Past deadline blocked");

    // Informational measurements only. Host scheduling/GPU load can affect both
    // paths, so correctness does not depend on a brittle latency threshold.
    std::mutex mutex;std::condition_variable changed;
    const auto condition=measure([&](auto deadline){std::unique_lock lock(mutex);changed.wait_until(lock,deadline,[]{return false;});});
    const auto precise=measure([&](auto deadline){while(Clock::now()<deadline)timer.wait_until(deadline);});
    std::printf("PASS frame timer: phase-preserving jitter/stall/idle pacing, notification before/during wait, cancel/rearm, coalesced notifications, no early deadline. high_resolution=%s\n",timer.high_resolution()?"true":"false");
    std::printf("10 ms wait, 48 samples: condition variable p50=%.3f ms p95=%.3f ms; frame timer p50=%.3f ms p95=%.3f ms.\n",
        condition[24],condition[45],precise[24],precise[45]);
    return 0;
}catch(const std::exception& error){std::printf("FAIL frame timer: %s\n",error.what());return 1;}}
