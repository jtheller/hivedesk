#pragma once
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace hive {
// Three ownership slots: converting, latest ready, and currently presenting.
// There is exactly one ready image, so a stalled display never creates playback.
class LatestPicture {
    enum class State {Free,Writing,Ready,Presenting};
    std::array<State,3> states_{State::Free,State::Free,State::Free};
    std::optional<size_t> ready_;
    std::mutex mutex_;std::condition_variable changed_;bool stopped_=false;
public:
    struct Publication {bool accepted=false,replaced=false;};
    std::optional<size_t> reserve(){
        std::lock_guard lock(mutex_);if(stopped_)return {};
        for(size_t i=0;i<states_.size();++i)if(states_[i]==State::Free){states_[i]=State::Writing;return i;}
        return {};
    }
    Publication publish(size_t index){
        std::lock_guard lock(mutex_);
        if(index>=states_.size()||states_[index]!=State::Writing)return {};
        if(stopped_){states_[index]=State::Free;return {};}
        const bool replaced=ready_.has_value();
        if(ready_)states_[*ready_]=State::Free;
        states_[index]=State::Ready;ready_=index;changed_.notify_one();return {true,replaced};
    }
    std::optional<size_t> take(std::optional<size_t> previous={}){
        std::lock_guard lock(mutex_);if(stopped_||!ready_)return {};
        if(previous&&*previous<states_.size()&&states_[*previous]==State::Presenting)states_[*previous]=State::Free;
        const auto result=*ready_;ready_.reset();states_[result]=State::Presenting;return result;
    }
    void release(size_t index){std::lock_guard lock(mutex_);if(index<states_.size())states_[index]=State::Free;}
    template<class Cancel> bool wait_ready(Cancel cancelled){
        std::unique_lock lock(mutex_);
        while(!stopped_&&!ready_&&!cancelled())changed_.wait_for(lock,std::chrono::milliseconds(2));
        return !stopped_&&!cancelled()&&ready_.has_value();
    }
    void stop(){std::lock_guard lock(mutex_);stopped_=true;if(ready_){states_[*ready_]=State::Free;ready_.reset();}changed_.notify_all();}
};
}
