#pragma once
#include <cstdint>

namespace hive {
// Local measurements. ACK roundtrip includes host queue/injection and the
// return network trip; it is not one-way input or input-to-photon latency.
struct InputDiagnostics {
    bool available=false,dedicated=false,ready=false;
    uint64_t session=0,connection=0;
    uint64_t enqueued=0,coalesced=0,sent=0,acknowledged=0,failed=0;
    uint32_t queued=0,inflight=0,samples=0,sample_age_us=UINT32_MAX;
    uint32_t capture_to_send_p50_us=0,capture_to_send_p95_us=0,capture_to_send_p99_us=0,capture_to_send_max_us=0;
    uint32_t ack_roundtrip_p50_us=0,ack_roundtrip_p95_us=0,ack_roundtrip_p99_us=0,ack_roundtrip_max_us=0;
    uint32_t host_queue_p95_us=0,host_queue_max_us=0,inject_p95_us=0,inject_max_us=0;
};
}
