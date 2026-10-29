#pragma once
#include <algorithm>
#include <cstdint>

namespace hive::datagram {
// Authenticated round-trip evidence, independent of video/decoder credits.
// Call only for negotiated recovery peers. All times are local microseconds.
class Recovery {
    uint64_t sequence_=0,pending_=0,sent_at_=0,next_probe_=0,eligible_=0,last_reply_=0;
    unsigned replies_=0,failures_=0;
public:
    static constexpr uint64_t ReplyTimeout=2000000,CommitTimeout=3000000;
    void fallback(uint64_t now){
        const auto delay=std::min<uint64_t>(60000000,5000000ull<<std::min(failures_,4u));
        failures_=std::min(failures_+1,5u);eligible_=now+delay;
        pending_=0;replies_=0;last_reply_=0;next_probe_=eligible_;
    }
    void stable(){failures_=0;}
    uint64_t probe(uint64_t now,bool committing){
        if(pending_){if(now-sent_at_<ReplyTimeout)return 0;pending_=0;replies_=0;}
        if(now<next_probe_)return 0;
        if(last_reply_&&now-last_reply_>=ReplyTimeout)replies_=0;
        pending_=++sequence_;sent_at_=now;next_probe_=now+(committing?200000:1000000);return pending_;
    }
    bool reply(uint64_t id,uint64_t now){
        if(!id||id!=pending_||now<sent_at_||now-sent_at_>=ReplyTimeout)return false;
        pending_=0;last_reply_=now;replies_=std::min(replies_+1,3u);return true;
    }
    bool ready(uint64_t now)const{return replies_>=3&&now>=eligible_&&now>=last_reply_&&now-last_reply_<ReplyTimeout;}
    void committing(uint64_t now){pending_=0;next_probe_=now;}
    uint64_t wake()const{return pending_?sent_at_+ReplyTimeout:next_probe_;}
};
}
