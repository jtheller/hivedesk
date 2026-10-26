#pragma once
#include "types.h"
#include <chrono>
#include <cstdint>

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
};
struct InputAck {
    uint64_t session=0,sequence=0,capture_time_us=0;
    uint32_t host_queue_us=0,inject_us=0;
    // Accepted by Windows (or an idempotent release), not application/scanout.
    bool injected=false;
};
}
