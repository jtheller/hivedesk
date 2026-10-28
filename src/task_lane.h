#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace hive {
// A bounded number of workers, not one thread per click. Jobs own their leases
// and must complete even when cancelled so dependent retirement can drain.
class TaskLane {
    std::mutex mutex_;std::condition_variable changed_;
    std::deque<std::function<void()>> priority_,background_;
    bool stopped_=false;std::thread worker_;
public:
    TaskLane():worker_([this]{for(;;){
        std::function<void()> job;
        {std::unique_lock lock(mutex_);changed_.wait(lock,[&]{return stopped_||!priority_.empty()||!background_.empty();});
            if(priority_.empty()&&background_.empty())return;
            auto& queue=priority_.empty()?background_:priority_;job=std::move(queue.front());queue.pop_front();}
        job();
    }}){}
    ~TaskLane(){stop();}
    void post(std::function<void()> job,bool priority=false){
        {std::lock_guard lock(mutex_);(priority?priority_:background_).push_back(std::move(job));}changed_.notify_one();
    }
    void stop(){
        {std::lock_guard lock(mutex_);stopped_=true;}changed_.notify_all();if(worker_.joinable())worker_.join();
    }
};
}
