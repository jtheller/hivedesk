#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace hive {
// Owned by the command worker. The UI generation cancels pending attempts
// immediately, including an attempt waiting for its first decoded frame.
struct ReconnectIntent {
    std::string peer;
    uint64_t epoch=0, next_ms=0;
    unsigned failures=0;
    bool waiting=false;
    void select(std::string id,uint64_t generation) { peer=std::move(id);epoch=generation;next_ms=0;failures=0;waiting=true; }
    bool current(uint64_t generation) const { return !peer.empty()&&epoch==generation; }
    bool due(uint64_t generation,uint64_t now) const { return current(generation)&&waiting&&now>=next_ms; }
    void lost(uint64_t now) {waiting=true;next_ms=now;failures=0;}
    uint64_t delay_ms() const {return 1000ull<<std::min(failures?failures-1:0,3u);}
    void attempted(uint64_t now) {waiting=true;failures=std::min(failures+1,4u);next_ms=now+delay_ms();}
    void connected() {waiting=false;failures=0;}
};
}
