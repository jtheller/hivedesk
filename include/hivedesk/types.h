#pragma once
#include <atomic>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace hive {
struct DesktopPreferences {
    bool startup=true,tray_icon=true;
    uint32_t hotkey_modifiers=3,hotkey_key='H'; // MOD_CONTROL | MOD_ALT
    bool debug_overlay=false;
    bool relative_mouse=false;
};
inline bool valid_hive_hotkey(uint32_t modifiers,uint32_t key){
    return modifiers>0&&modifiers<=15&&(modifiers&11)&&
        ((key>='A'&&key<='Z')||(key>='0'&&key<='9')||(key>=0x70&&key<=0x7a));
}
struct Peer {
    std::string id;
    std::string name;
    std::string address;
    uint16_t port = 47980;
    bool authenticated = false;
    uint32_t features = 0;
    std::string version;
};
constexpr uint32_t FeatureDesktopAux = 1;
constexpr uint32_t FeatureAutomaticStream = 2;
constexpr uint32_t FeatureStreamDiagnostics = 4;
constexpr uint32_t FeatureEncodeTiming = 8;
constexpr uint32_t FeatureHostKick = 16;
constexpr uint32_t FeatureAutoRate = 64;
constexpr uint32_t FeatureDedicatedInput = 128;
constexpr uint32_t FeatureFrameTiming = 256;
constexpr uint32_t FeatureCursorComposition = 512;
constexpr uint32_t FeatureBuildVersion = 1024;
constexpr uint32_t FeatureNativeSnapshot = 2048;
// 4096 is reserved for 0.4.0-0.4.2, whose broker rejected activation.
// Mixed versions must use an ordinary cold start, never a broken warm lease.
constexpr uint32_t FeatureWarmStream = 8192;
constexpr uint32_t FeatureRichClipboard = 16384;
// 32768 is reserved for 0.7.0, whose SYSTEM pipe rejected Peek messages.
constexpr uint32_t FeatureLivePeek = 65536;
constexpr uint32_t FeatureByteFeedback = 131072;
constexpr uint32_t FeaturePathProbe = 262144;
constexpr uint32_t FeatureDatagramVideo = 524288;
constexpr uint32_t FeatureDatagramRecovery = 1048576;
constexpr uint32_t FeatureScopedCursor = 2097152;
constexpr uint32_t FeatureNestedReturn = 4194304;
constexpr uint32_t SnapshotWidth = 672, SnapshotHeight = 378;
constexpr size_t MaxSnapshotBytes = 256 * 1024;
constexpr size_t MaxNativeSnapshotBytes = 2 * 1024 * 1024;
inline bool valid_snapshot_dimensions(uint32_t width,uint32_t height) {
    return width&&height&&width<=8192&&height<=8192&&uint64_t(width)*height<=32*1024*1024;
}
struct Snapshot { uint64_t request = 0; std::vector<uint8_t> jpeg; bool native=false; };
struct Thumbnail { std::vector<uint8_t> bgra; uint32_t width=SnapshotWidth,height=SnapshotHeight; };
// Shape pixels retain native alpha and monochrome AND/XOR masks. Position is
// primary-display normalized; revision changes only for host-side pointer moves.
struct CursorState {
    uint64_t session=0,input_capture_us=0;
    bool position_valid=false; // Only genuine host motion, never the initial snapshot.
    bool visible = true;
    uint32_t x = 0, y = 0;
    uint64_t position_revision = 0;
    uint32_t width = 0, height = 0, hotspot_x = 0, hotspot_y = 0;
    std::vector<uint8_t> color, mask;
};
inline bool valid_cursor(const CursorState& c) {
    if(c.x>65535 || c.y>65535) return false;
    if(!c.width) return !c.height && !c.hotspot_x && !c.hotspot_y && c.color.empty() && c.mask.empty();
    if(c.width>256 || !c.height || c.height>256 || c.hotspot_x>=c.width || c.hotspot_y>=c.height) return false;
    return (c.color.empty() || c.color.size()==size_t(c.width)*c.height*4) &&
        c.mask.size()==size_t((c.width+31)/32)*4*c.height*(c.color.empty()?2:1);
}
struct StreamSettings {
    uint32_t fps = 120;
    uint32_t bitrate_mbps = 80;
    uint32_t receiver_millihz = 0;
    uint64_t session = 0; // Nonzero selects negotiated automatic quality/pacing.
    bool diagnostic_constqp = false; // Developer baseline only; never negotiated.
    bool preview = false; // Local broker state: live video without control authority.
    bool warm = false; // One picture, then retain media resources without input authority.
};
enum StreamPressure : uint32_t { PressureEncode=1, PressureDecode=2, PressureNetwork=4 };
struct StreamFeedback {
    uint64_t session=0,capture_time_us=0;
    uint32_t arrival_age_us=0,service_us=0,queue_us=0;
    bool recover=false;
    bool rate_details=false;
    uint64_t feedback_sequence=0,received_bytes=0,received_frames=0;
    uint32_t received_kbps=0,received_millihz=0;
    uint32_t receive_sample_age_us=UINT32_MAX,service_sample_age_us=UINT32_MAX,transit_extra_us=0;
    uint64_t sequence_gaps=0,age_expired=0,queue_overflows=0;
    // Session-local wire progress, including incomplete authenticated packets.
    // Used for congestion estimation only, never image/input authorization.
    bool transport_details=false;
    uint64_t transport_bytes=0,transport_sample_us=0;
};
struct StreamHealth {
    uint64_t session=0;
    uint32_t target_millihz=0,rate_millihz=0,pressure=0;
};
struct SourceDiagnostics {
    uint64_t session=0;
    uint32_t encoded_millihz=0,encode_us=0,capture_wait_us=0;
    uint32_t send_wait_us_per_second=0,feedback_age_us=0,network_extra_us=0;
    uint32_t color_us=0,nvenc_us=0,encode_age_us=UINT32_MAX;
    bool encode_details=false;
    bool rate_details=false,automatic_cbr=false,quality_limited=false;
    uint32_t target_bitrate_bps=0,actual_bitrate_bps=0,average_qp=UINT32_MAX;
    uint32_t rate_reason=0;
    uint32_t receiver_service_us=0,receiver_queue_us=0;
    bool receiver_recover=false;
    uint64_t feedback_sequence=0,source_discarded=0;
    uint64_t sequence_gaps=0,age_expired=0,queue_overflows=0;
};
inline bool valid_diagnostics(const SourceDiagnostics& d){
    return d.session&&d.encoded_millihz<=4000000&&d.encode_us<=10000000&&d.capture_wait_us<=10000000&&d.send_wait_us_per_second<=1000000&&d.color_us<=10000000&&d.nvenc_us<=10000000&&
        (!d.rate_details||((d.average_qp<=51||d.average_qp==UINT32_MAX)&&d.receiver_service_us<=1000000&&d.rate_reason<=7));
}
inline bool valid_feedback(const StreamFeedback& f){
    return f.session&&f.service_us<=1000000&&(!f.rate_details||f.received_millihz<=4000000);
}
struct ViewerDiagnostics {
    bool software_decode=false;
    uint64_t session=0,discarded=0,skipped=0,presented_frames=0;
    uint32_t received_millihz=0,decoded_millihz=0,presented_millihz=0,receive_kbps=0;
    uint32_t decode_us=0,render_us=0,present_us=0,service_p90_us=0,queue_us=0,queue_depth=0;
    uint32_t nvdec_map_us=0,convert_us=0,draw_us=0;
    uint32_t present_gap_p95_us=0,present_gap_max_us=0;
    uint32_t wire_assembly_us=0; // Last complete frame, local receiver clock; not playback queue age.
    bool recovering=false;
    uint32_t service_sample_age_us=UINT32_MAX,receive_sample_age_us=UINT32_MAX,transit_extra_us=0;
    uint64_t sequence_gaps=0,age_expired=0,queue_overflows=0;
};
enum class CursorComposition : uint8_t { Unknown=0, Separate=1, NoSeparate=2 };
inline bool valid_cursor_composition(CursorComposition value) {
    return value==CursorComposition::Unknown||value==CursorComposition::Separate||value==CursorComposition::NoSeparate;
}
struct EncodedFrame {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sequence = 0;
    uint64_t capture_time_us = 0;
    bool keyframe = false;
    uint64_t session = 0;
    // Source steady clock, recorded once when encoded output is complete.
    // Zero means the legacy capture-to-arrival freshness policy. This must not
    // be restamped by the broker or transport after waiting in their queues.
    uint64_t source_ready_us = 0;
    // Describes this image, never the newest asynchronously sampled cursor.
    // Unknown preserves the separate-cursor behavior of legacy peers.
    CursorComposition cursor_composition = CursorComposition::Unknown;
    // Receiver-local observations, never serialized or accepted from the peer.
    // Packet assembly is wire service time, not a compressed playback queue.
    uint64_t receive_started_us = 0, receive_completed_us = 0;
    std::shared_ptr<const std::atomic<uint64_t>> receive_progress;
};
// Allow bounded, intrinsic GPU work without confusing it with queued playback.
// A one-second source job remains an absolute upper bound, never permission to
// add a second of downstream buffering. Legacy packets have no such provenance.
constexpr uint64_t MaxSourceWorkUs = 1000000;
inline bool valid_frame_timing(const EncodedFrame& frame) {
    return !frame.source_ready_us || (frame.capture_time_us && frame.source_ready_us >= frame.capture_time_us &&
        frame.source_ready_us-frame.capture_time_us <= MaxSourceWorkUs);
}
enum class InputKind : uint32_t { Key = 1, Absolute = 2, Relative = 3, Button = 4, Wheel = 5, ReleaseAll = 6 };
// Key: a=scan code, b=1 if keyup, c=1 if extended.
// Absolute: a,b normalized 0..65535 across PRIMARY screen; Relative: signed dx,dy.
// Button: a=1 left/2 right/3 middle/4 X1/5 X2, b=1 if up.
// Wheel: a=signed delta, b=1 if horizontal.
struct InputEvent { InputKind kind = InputKind::ReleaseAll; int32_t a = 0, b = 0, c = 0; };
}
