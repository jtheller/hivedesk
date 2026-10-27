#pragma once
#include <array>
#include <cstdint>

namespace hive {
// Debug-only, bounded asynchronous timing trace. Never records input values,
// clipboard contents or pixels. Producers never wait for the disk/consumer.
enum class ExperienceEvent : uint32_t { Clock=0, Arrival=1, Prepared=2, Present=3, InputSent=4, InputAck=5, Lost=6 };
void experience_enabled(bool);
bool experience_enabled();
void experience_trace(ExperienceEvent,uint64_t session,uint64_t sequence,std::array<uint64_t,10> values={}) noexcept;
}
