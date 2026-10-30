#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace hive {
// Bytes arriving without a usable picture/input channel are not readiness.
// Permit a slow first frame to make progress, but never renew Connecting forever.
struct StreamStartupDeadline {
    uint64_t idle_until,hard_until,bytes;
    StreamStartupDeadline(uint64_t now,uint64_t received):idle_until(now+8000),hard_until(now+16000),bytes(received){}
    void progress(uint64_t now,uint64_t received){if(received>bytes){bytes=received;idle_until=std::min(hard_until,now+8000);}}
    bool expired(uint64_t now)const{return now>=std::min(idle_until,hard_until);}
};
// Owned by the command worker. The UI generation cancels pending attempts
// immediately, including an attempt waiting for its first decoded frame.
struct ReconnectIntent {
    std::string peer;
    uint64_t epoch=0, next_ms=0;
    unsigned failures=0;
    bool waiting=false;
    uint64_t wake_started=0,wake_next=0;unsigned wake_attempts=0;
    void select(std::string id,uint64_t generation) { peer=std::move(id);epoch=generation;next_ms=0;failures=0;waiting=true;wake_started=wake_next=0;wake_attempts=0; }
    bool current(uint64_t generation) const { return !peer.empty()&&epoch==generation; }
    bool due(uint64_t generation,uint64_t now) const { return current(generation)&&waiting&&now>=next_ms; }
    void lost(uint64_t now) {waiting=true;next_ms=now;failures=0;wake_started=wake_next=0;wake_attempts=0;}
    bool wake_due(uint64_t generation,uint64_t now){
        if(!current(generation)||!waiting||now<wake_next||(wake_attempts&&(now-wake_started>=30000||wake_attempts>=10)))return false;
        if(!wake_attempts)wake_started=now;++wake_attempts;wake_next=now+3000;return true;
    }
    uint64_t delay_ms() const {return 1000ull<<std::min(failures?failures-1:0,3u);}
    void attempted(uint64_t now) {waiting=true;failures=std::min(failures+1,4u);next_ms=now+delay_ms();}
    void connected() {waiting=false;failures=0;}
};
}
