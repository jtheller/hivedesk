// Actual capture/encode worker, no visible window or input injection. Measures
// readiness wake separately from capture and NVENC, and checks stop while gated.
#include "hivedesk/media.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
using Clock=std::chrono::steady_clock;
int main(){try{
    std::atomic<bool> credit=false,measure=false;
    std::atomic<unsigned> checks=0,frames=0,wakes=0;std::atomic<int64_t> wake_ns=0;
    std::mutex mutex;std::condition_variable changed;
    hive::MediaHost host; // Stops before callback-owned state is destroyed.
    auto wait=[&](auto predicate,const char* error){std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(3),predicate))throw std::runtime_error(error);};
    std::string error;
    const bool started=host.start({0,0,60000,8181},[&](hive::EncodedFrame){
        std::lock_guard lock(mutex);checks=0;credit=false;++frames;changed.notify_all();
    },error,[&]{
        std::lock_guard lock(mutex);
        if(!credit){++checks;changed.notify_all();return false;}
        if(measure.exchange(false)){wake_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();++wakes;changed.notify_all();}
        return true;
    });
    if(!started)throw std::runtime_error(error);
    double total_us=0,max_us=0;
    for(unsigned round=0;round<12;++round){
        wait([&]{return checks>=2;},"Worker never entered send-credit wait");
        const auto begin=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        measure=true;credit=true;host.notify_ready();host.request_keyframe();
        wait([&]{return wakes>round;},"Readiness notification did not wake capture worker");
        const double us=(wake_ns.load()-begin)/1000.0;total_us+=us;max_us=std::max(max_us,us);
        wait([&]{return frames>round;},"Credit did not produce a real encoded frame");
    }
    wait([&]{return checks>=2;},"Worker did not re-enter blocked state");
    const auto stop=Clock::now();host.stop();
    if(Clock::now()-stop>std::chrono::seconds(1)||host.running())throw std::runtime_error("Stop failed to cancel blocked capture");
    std::printf("PASS real MediaHost: 12 gated frames, notify-to-ready mean %.1f us / max %.1f us; stop cancels blocked credit. No visible stimulus or input; wake timing excludes capture/codec.\n",total_us/12,max_us);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL media readiness: %s\n",e.what());return 1;}}
