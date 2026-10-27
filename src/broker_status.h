#pragma once
#include "broker_wire.h"
#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace hive::broker {
struct StatusMessage { Kind kind; Bytes bytes; };

// Status is latest-state data, not a stream of work to replay. Keep at most one
// unsent report of each kind. A consumer may block on the shared media pipe, but
// it never holds this mutex during I/O, so capture can keep replacing reports.
class StatusMailbox {
    static constexpr size_t MaxStatusBytes = 512;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<std::optional<StatusMessage>,2> pending_;
    size_t next_ = 0;
    bool stopped_ = false;

    bool take(StatusMessage& message) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&]{return stopped_||pending_[0]||pending_[1];});
        if(stopped_)return false;
        const auto index=pending_[next_]?next_:1-next_;
        message=std::move(*pending_[index]);pending_[index].reset();next_=1-index;
        return true;
    }
public:
    bool publish(Kind kind,Bytes bytes) {
        if((kind!=Health&&kind!=Diagnostics)||bytes.size()>MaxStatusBytes)return false;
        {
            std::lock_guard lock(mutex_);
            if(stopped_)return false;
            pending_[kind==Health?0:1]=StatusMessage{kind,std::move(bytes)};
        }
        changed_.notify_one();return true;
    }
    void stop() {
        {
            std::lock_guard lock(mutex_);stopped_=true;
            for(auto& message:pending_)message.reset();
        }
        changed_.notify_all();
    }
    template<class Send> bool run(Send&& send) {
        StatusMessage message{Health,{}};
        while(take(message)) {
            // The caller owns cancellation of an already-running pipe write.
            if(!send(message.kind,message.bytes)){stop();return false;}
        }
        return true;
    }
};
}
