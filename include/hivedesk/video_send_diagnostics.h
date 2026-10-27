#pragma once
#include <cstdint>

namespace hive {
// Local transport evidence only; these fields never enter the peer protocol.
// Timing percentiles cover the latest 128 attempts within the last two seconds.
// A successful WSASend completion is not a receiver/display acknowledgement.
struct VideoSendDiagnostics {
    bool available=false;
    uint64_t connection=0;
    uint64_t submitted_packets=0,completed_packets=0,completed_bytes=0,failed_packets=0,queue_expired=0;
    uint32_t samples=0,sample_age_us=UINT32_MAX;
    uint32_t inflight=0; // Owned overlapped writes, bounded to two.
    uint32_t encrypt_p95_us=0,encrypt_max_us=0,send_p95_us=0,send_max_us=0;
    uint32_t queue_p95_us=0,queue_max_us=0;
    // submit_gap includes periods with no supplied frame (e.g. a static screen).
    // ready_gap excludes those periods: it starts at the later of previous
    // completion and this frame's enqueue, and includes encryption/dispatch.
    // Overlapped submissions before the prior completion have zero gap.
    uint32_t submit_gap_p95_us=0,submit_gap_max_us=0,ready_gap_p95_us=0,ready_gap_max_us=0;
};
}
