#pragma once
#include <cstdint>

namespace hive {
// Local transport evidence only; these fields never enter the peer protocol.
// Timing percentiles cover the latest 128 attempts within the last two seconds.
// A successful WSASend completion is not a receiver/display acknowledgement.
struct VideoSendDiagnostics {
    bool available=false;
    uint32_t packet_transport=0; // 0 absent, 1 negotiating, 2 UDP, 3 TCP fallback, 4 recovering.
    uint32_t packet_rtt_us=0,packet_min_rtt_us=0,packet_window=0,packet_inflight=0;
    uint64_t packet_owned_bytes=0,packet_sent=0,packet_retransmitted=0,packet_received=0,packet_invalid=0,packet_expired=0;
    uint64_t packet_send_expired=0,packet_receive_expired=0;
    uint64_t connection=0;
    uint64_t submitted_packets=0,completed_packets=0,completed_bytes=0,failed_packets=0,queue_expired=0;
    uint32_t samples=0,sample_age_us=UINT32_MAX;
    uint32_t inflight=0,write_limit=2; // Owned writes; byte budget also applies above two.
    uint64_t owned_bytes=0,write_budget_bytes=0;
    bool tcp_available=false;
    uint32_t tcp_sample_age_us=UINT32_MAX,tcp_rtt_us=0,tcp_min_rtt_us=0;
    uint32_t tcp_ideal_bytes=0,tcp_cwnd_bytes=0,tcp_send_window_bytes=0,tcp_inflight_bytes=0;
    uint32_t encrypt_p95_us=0,encrypt_max_us=0,send_p95_us=0,send_max_us=0;
    uint32_t queue_p95_us=0,queue_max_us=0;
    // submit_gap includes periods with no supplied frame (e.g. a static screen).
    // ready_gap excludes those periods: it starts at the later of previous
    // completion and this frame's enqueue, and includes encryption/dispatch.
    // Overlapped submissions before the prior completion have zero gap.
    uint32_t submit_gap_p95_us=0,submit_gap_max_us=0,ready_gap_p95_us=0,ready_gap_max_us=0;
};
}
