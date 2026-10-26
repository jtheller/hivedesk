#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace hive::broker {
// A downstream slot becoming available must wake the frame-credit waiter. A
// fixed Sleep(2) here can cost an entire scheduler tick for every encoded frame.
class ReadySignal {
public:
    void notify() {
        // Pair notification with the wait mutex even though the readiness state
        // is owned elsewhere. This closes the predicate-check / wait race.
        std::lock_guard lock(mutex_);
        changed_.notify_all();
    }
    bool wait(const std::atomic<bool>& stopped,const std::function<bool()>& ready) {
        std::unique_lock lock(mutex_);
        const auto available=[&]{return stopped.load()||!ready||ready();};
        // Events handle the normal path. The long fallback also accommodates
        // older callers that supply only a readiness predicate.
        while(!changed_.wait_for(lock,std::chrono::milliseconds(100),available)){}
        return !stopped.load();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
};
}
