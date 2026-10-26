#pragma once
#include "hivedesk/video_send_diagnostics.h"
#include <algorithm>
#include <array>
#include <mutex>

namespace hive {
class VideoSendWindow {
public:
    static constexpr size_t capacity=128;
    static constexpr uint64_t max_age_us=2000000;
    struct Attempt {
        uint64_t queued_at=0,dequeued_at=0,submitted_at=0,finished_at=0;
        uint32_t encrypt_us=0;
        uint64_t wire_bytes=0;
        bool submitted=false,completed=false;
    };
    void record(const Attempt& attempt){
        std::lock_guard lock(mutex_);
        if(attempt.submitted)++totals_.submitted_packets;
        if(attempt.completed){++totals_.completed_packets;totals_.completed_bytes+=attempt.wire_bytes;}
        else ++totals_.failed_packets;
        Sample sample;sample.at=attempt.finished_at;sample.encrypt=attempt.encrypt_us;
        sample.queue=elapsed(attempt.dequeued_at,attempt.queued_at);
        if(attempt.submitted){
            sample.send=elapsed(attempt.finished_at,attempt.submitted_at);
            if(last_completion_)sample.submit_gap=elapsed(attempt.submitted_at,last_completion_);
            sample.ready_gap=elapsed(attempt.submitted_at,std::max(last_completion_,attempt.queued_at));
        }
        last_completion_=attempt.finished_at;
        samples_[next_]=sample;next_=(next_+1)%capacity;
    }
    void expired(){std::lock_guard lock(mutex_);++totals_.queue_expired;}
    VideoSendDiagnostics snapshot(uint64_t now)const{
        std::lock_guard lock(mutex_);auto result=totals_;result.available=true;
        result.sample_age_us=last_completion_?elapsed(now,last_completion_):UINT32_MAX;
        std::array<uint32_t,capacity> encrypt{},send{},queue{},submit_gap{},ready_gap{};
        size_t count=0;
        for(const auto& s:samples_){
            if(!s.at||now<s.at||now-s.at>=max_age_us)continue;
            encrypt[count]=s.encrypt;send[count]=s.send;queue[count]=s.queue;submit_gap[count]=s.submit_gap;ready_gap[count]=s.ready_gap;++count;
        }
        result.samples=uint32_t(count);
        const auto summarize=[count](auto& values,uint32_t& p95,uint32_t& maximum){
            if(!count)return;std::sort(values.begin(),values.begin()+count);
            p95=values[(count*95+99)/100-1];maximum=values[count-1];
        };
        summarize(encrypt,result.encrypt_p95_us,result.encrypt_max_us);summarize(send,result.send_p95_us,result.send_max_us);
        summarize(queue,result.queue_p95_us,result.queue_max_us);summarize(submit_gap,result.submit_gap_p95_us,result.submit_gap_max_us);
        summarize(ready_gap,result.ready_gap_p95_us,result.ready_gap_max_us);return result;
    }
private:
    static uint32_t elapsed(uint64_t end,uint64_t begin){return end>=begin?uint32_t(std::min<uint64_t>(UINT32_MAX,end-begin)):0;}
    struct Sample{uint64_t at=0;uint32_t encrypt=0,send=0,queue=0,submit_gap=0,ready_gap=0;};
    mutable std::mutex mutex_;
    std::array<Sample,capacity> samples_{};size_t next_=0;
    uint64_t last_completion_=0;
    VideoSendDiagnostics totals_{};
};
}
