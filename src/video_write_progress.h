#pragma once
#include "hivedesk/session_watchdog.h"
#include <cstdint>
#include <algorithm>

namespace hive {
// Fast writes complete before the first sample. On a slow write, new TCP
// payload can refresh the stall deadline; retransmission-only activity cannot.
class VideoWriteProgress {
public:
    void reset(uint64_t now){deadline_=now+SessionStallUs;next_=now+500000;sampled_=false;}
    void observe(uint64_t now,uint64_t bytes_out,uint32_t retransmitted){
        if(sampled_&&bytes_out>=out_&&bytes_out-out_>uint32_t(retransmitted-retrans_))
            deadline_=now+SessionStallUs;
        out_=bytes_out;retrans_=retransmitted;sampled_=true;next_=now+100000;
    }
    void unavailable(){next_=UINT64_MAX;}
    bool due(uint64_t now)const{return now>=next_;}
    bool expired(uint64_t now)const{return now>=deadline_;}
    uint64_t wake_at()const{return (std::min)(next_,deadline_);}
private:
    uint64_t deadline_=0,next_=0,out_=0;
    uint32_t retrans_=0;bool sampled_=false;
};
}
