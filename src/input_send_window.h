#pragma once
#include "hivedesk/input_channel.h"
#include "input_credit.h"
#include "hivedesk/input_diagnostics.h"
#include <algorithm>
#include <array>
#include <deque>
#include <iterator>

namespace hive {
// Protected by the owning input connection's queue mutex. Eight packets on
// low RTT links; measured propagation may increase the bounded ACK window. Adjacent motion is
// coalesced before encryption; key/button/wheel/release boundaries never move.
class InputSendWindow {
public:
    static constexpr size_t capacity=128,credit=8,sample_capacity=256;
    static constexpr uint64_t deadline_us=SessionStallUs,sample_age_us=2000000;
    struct Pending { InputPacket packet;uint64_t queued_at=0; };
    struct Flight { InputPacket packet;uint64_t sent_at=0;uint32_t capture_to_send=0; };
    void reset(uint64_t session) { *this=InputSendWindow{};session_=session; }
    bool enqueue(InputPacket packet,uint64_t now) {
        if(!session_||packet.session!=session_||!packet.sequence||packet.sequence<=last_enqueued_||!packet.capture_time_us)return false;
        ++totals_.enqueued;last_enqueued_=packet.sequence;
        const auto moving=[](InputKind kind){return kind==InputKind::Absolute||kind==InputKind::Relative;};
        if(!pending_.empty()&&moving(packet.event.kind)&&pending_.back().packet.event.kind==packet.event.kind&&
           pending_.back().packet.path_size==packet.path_size&&pending_.back().packet.path==packet.path) {
            auto& previous=pending_.back();
            if(packet.event.kind==InputKind::Relative) {
                packet.event.a=int32_t(std::clamp<int64_t>(int64_t(previous.packet.event.a)+packet.event.a,INT32_MIN,INT32_MAX));
                packet.event.b=int32_t(std::clamp<int64_t>(int64_t(previous.packet.event.b)+packet.event.b,INT32_MIN,INT32_MAX));
                // Relative displacement includes older samples: preserve their
                // age so merging cannot hide a stalled queue indefinitely.
                packet.capture_time_us=previous.packet.capture_time_us;
            }
            previous.packet=packet;++totals_.coalesced;return true;
        }
        if(pending_.size()>=capacity){++totals_.failed;return false;}
        pending_.push_back({packet,now});credit_state(now);return true;
    }
    bool writable() const {return !pending_.empty()&&flight_.size()<credits_.limit();}
    bool empty() const {return pending_.empty()&&flight_.empty();}
    uint64_t next_deadline() const {
        uint64_t at=flight_.empty()?0:flight_.front().sent_at;
        if(!pending_.empty()&&(!at||pending_.front().queued_at<at))at=pending_.front().queued_at;
        return at?at+deadline_us:0;
    }
    bool expired(uint64_t now) const {const auto at=next_deadline();return at&&now>=at;}
    InputPacket take(uint64_t now) {
        auto value=pending_.front();pending_.pop_front();
        flight_.push_back({value.packet,now,elapsed(now,value.packet.capture_time_us)});++totals_.sent;credit_state(now);
        return value.packet;
    }
    void submitted(uint64_t session,uint64_t sequence,uint64_t now) {
        if(session!=session_)return;
        const auto found=std::find_if(flight_.begin(),flight_.end(),[&](const Flight& f){return f.packet.sequence==sequence;});
        if(found!=flight_.end()){found->sent_at=now;found->capture_to_send=elapsed(now,found->packet.capture_time_us);}
    }
    bool acknowledge(const InputAck& ack,uint64_t now) {
        if(ack.session!=session_||ack.sequence<=last_ack_)return true;
        const auto found=std::find_if(flight_.begin(),flight_.end(),[&](const Flight& f){return f.packet.sequence==ack.sequence;});
        if(found==flight_.end()||ack.capture_time_us!=found->packet.capture_time_us)return false;
        Sample sample{now,found->capture_to_send,elapsed(now,found->sent_at),ack.host_queue_us,ack.inject_us};
        credits_.observe(sample.roundtrip,ack.host_queue_us,ack.inject_us,ack.injected);
        samples_[next_sample_]=sample;next_sample_=(next_sample_+1)%sample_capacity;last_sample_=now;
        totals_.acknowledged+=uint64_t(std::distance(flight_.begin(),found))+1;
        if(!ack.injected)++totals_.failed;
        last_ack_=ack.sequence;flight_.erase(flight_.begin(),std::next(found));credit_state(now);return true;
    }
    uint64_t last_sequence() const {return last_enqueued_;}
    void finish() {pending_.clear();flight_.clear();credit_since_=0;}
    uint64_t session() const {return session_;}
    InputDiagnostics snapshot(uint64_t now) const {
        auto result=totals_;
        if(credit_since_){const auto age=elapsed(now,credit_since_);result.credit_wait_total_us+=age;result.credit_wait_max_us=std::max(result.credit_wait_max_us,age);}
        result.credit_limit=uint32_t(credits_.limit());
        result.available=result.dedicated=true;result.session=session_;
        result.queued=uint32_t(pending_.size());result.inflight=uint32_t(flight_.size());
        result.sample_age_us=last_sample_?elapsed(now,last_sample_):UINT32_MAX;
        std::array<uint32_t,sample_capacity> capture{},roundtrip{},queue{},inject{};size_t count=0;
        for(const auto& s:samples_)if(s.at&&now>=s.at&&now-s.at<sample_age_us){capture[count]=s.capture;roundtrip[count]=s.roundtrip;queue[count]=s.queue;inject[count]=s.inject;++count;}
        result.samples=uint32_t(count);
        const auto summary=[&](auto& values,uint32_t& p50,uint32_t& p95,uint32_t& p99,uint32_t& maximum){if(!count)return;std::sort(values.begin(),values.begin()+count);p50=values[(count*50+99)/100-1];p95=values[(count*95+99)/100-1];p99=values[(count*99+99)/100-1];maximum=values[count-1];};
        summary(capture,result.capture_to_send_p50_us,result.capture_to_send_p95_us,result.capture_to_send_p99_us,result.capture_to_send_max_us);
        summary(roundtrip,result.ack_roundtrip_p50_us,result.ack_roundtrip_p95_us,result.ack_roundtrip_p99_us,result.ack_roundtrip_max_us);
        uint32_t ignored50=0,ignored99=0;
        summary(queue,ignored50,result.host_queue_p95_us,ignored99,result.host_queue_max_us);summary(inject,ignored50,result.inject_p95_us,ignored99,result.inject_max_us);return result;
    }
private:
    static uint32_t elapsed(uint64_t end,uint64_t begin){return end>=begin?uint32_t(std::min<uint64_t>(UINT32_MAX,end-begin)):0;}
    struct Sample {uint64_t at=0;uint32_t capture=0,roundtrip=0,queue=0,inject=0;};
    void credit_state(uint64_t now){
        const bool blocked=!pending_.empty()&&flight_.size()>=credits_.limit();
        if(blocked&&!credit_since_){credit_since_=now;++totals_.credit_stalls;}
        else if(!blocked&&credit_since_){const auto age=elapsed(now,credit_since_);totals_.credit_wait_total_us+=age;totals_.credit_wait_max_us=std::max(totals_.credit_wait_max_us,age);credit_since_=0;}
    }
    InputCredit credits_;
    uint64_t credit_since_=0;
    uint64_t session_=0,last_enqueued_=0,last_ack_=0,last_sample_=0;
    size_t next_sample_=0;
    std::deque<Pending> pending_;std::deque<Flight> flight_;
    std::array<Sample,sample_capacity> samples_{};
    InputDiagnostics totals_{};
};
}
