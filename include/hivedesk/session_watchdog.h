#pragma once
#include <chrono>
#include <cstdint>

namespace hive {
// Session failure policy, independent of frame freshness and adaptive pacing.
// Hundreds of milliseconds of RTT/scheduling delay must not revoke control.
// Keep queues bounded; abandon unfinished work only after a seconds-long stall.
inline constexpr auto SessionStallTimeout = std::chrono::seconds(2);
inline constexpr uint64_t SessionStallUs =
    std::chrono::duration_cast<std::chrono::microseconds>(SessionStallTimeout).count();
}
