#pragma once
#include "types.h"
#include "session_watchdog.h"
#include <chrono>
#include <cstdint>
#include <array>
#include <string_view>

namespace hive {
inline uint64_t input_now_us() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct InputPacket {
    uint64_t session=0,sequence=0,capture_time_us=0;
    InputEvent event;
    // Receiver-local time, never trusted from the network. The Windows steady
    // clock uses QPC, shared by the agent and its desktop broker on this host.
    uint64_t received_time_us=0;
    // Transport-only route history. Never passed to SendInput or interpreted
    // as physical input. A repeated node consumes the event without injection.
    std::array<uint64_t,16> path{};
    uint8_t path_size=0;
};
inline uint64_t input_node(std::string_view id){uint64_t value=14695981039346656037ull;for(unsigned char c:id){value^=c;value*=1099511628211ull;}return value?value:1;}
inline bool input_visited(const InputPacket& p,uint64_t node){for(unsigned i=0;i<p.path_size&&i<p.path.size();++i)if(p.path[i]==node)return true;return false;}
inline bool input_append(InputPacket& p,uint64_t node){if(!node||p.path_size>=p.path.size()||input_visited(p,node))return false;p.path[p.path_size++]=node;return true;}
struct InputAck {
    uint64_t session=0,sequence=0,capture_time_us=0;
    uint32_t host_queue_us=0,inject_us=0;
    // Accepted by Windows (or an idempotent release), not application/scanout.
    bool injected=false;
};
}
