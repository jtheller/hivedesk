#pragma once
#include "types.h"
#include "video_send_diagnostics.h"
#include "input_channel.h"
#include "input_diagnostics.h"
#include "clipboard.h"
#include "nested_return.h"
#include "view_route.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hive {
struct TransportConfig {
    std::string device_id;
    std::string device_name;
    std::string group_key; // Visible Network Key, 4-64 printable ASCII; legacy 64-hex supported. Never log.
    uint16_t port = 47980;
    bool discovery = true;
    bool automatic_lan = false; // Ephemeral ECDH; trusts any on-link HiveDesk peer, no shared key.
    bool datagram_video = true; // Capability override for developer compatibility tests; no user setting.
    uint16_t discovery_port = 47979; // Isolate generated discovery/failure tests.
};
struct TransportCallbacks {
    // All callbacks run on worker threads; caller must marshal UI work.
    std::function<void(const Peer&)> peer;
    std::function<void(const std::string&, const StreamSettings&)> start_stream;
    std::function<void(const std::string&)> stop_stream;
    std::function<void(const std::string&, uint64_t)> cancel_warm;
    std::function<void(const std::string&, uint64_t,bool)> preview;
    std::function<void(const std::string&, EncodedFrame)> frame;
    // Optional receive-side decoder credit. False retains compressed references
    // in the bounded packet lane while ACK/input work continues independently.
    std::function<bool(const std::string&,uint64_t)> frame_credit;
    std::function<void(const std::string&, const InputEvent&)> input;
    std::function<void(const std::string&, const std::wstring&)> clipboard;
    std::function<void(const std::string&, const ClipboardOffer&)> clipboard_offer;
    std::function<void(const std::string&, const std::vector<uint8_t>&)> clipboard_packet;
    std::function<void(const std::string&)> status;
    std::function<void(const std::string&, uint64_t,bool)> snapshot_request;
    std::function<void(const std::string&, Snapshot)> snapshot;
    std::function<void(const std::string&, CursorState)> cursor;
    std::function<void(const std::string&, const StreamFeedback&)> feedback;
    std::function<void(const std::string&, const StreamHealth&)> health;
    std::function<void(const std::string&, const SourceDiagnostics&)> diagnostics;
    // Called outside transport locks when the bounded video queue gains space.
    // Notification only: recheck frame_ready(); another producer may use it.
    std::function<void(const std::string&)> frame_ready;
    // A queued encoded frame expired before transmission. Its dependent frames
    // are rejected until a fresh IDR arrives. Runs outside transport locks.
    std::function<void(const std::string&)> request_keyframe;
    // The source explicitly ended this viewing session. Match peer AND session;
    // this does not stop a reciprocal stream or request automatic reconnection.
    std::function<void(const std::string&, uint64_t)> host_ended;
    // Runs after clipboard and stream-start on the reliable control reader.
    // Return true only once the requested host session and input broker are ready.
    std::function<bool(const std::string&, uint64_t)> input_begin;
    std::function<void(const std::string&, const InputPacket&)> input_packet;
    std::function<void(const std::string&, const InputAck&)> input_ack;
    std::function<void(const std::string&, const ReturnRequest&)> return_request;
    std::function<void(const std::string&, const ReturnReply&)> return_reply;
    std::function<void()> view_route_changed;
    std::function<void(const Peer&,uint64_t,uint32_t)> media_probe_request;
    std::function<void(const std::string&,MediaProbeReport)> media_probe_report;
};
// Explicit developer diagnostic: generated data only, bounded duration/rate.
struct PathProbeResult {
    uint64_t sent_packets=0,received_packets=0,received_bytes=0;
    uint64_t source_elapsed_us=0,receive_span_us=0;
};
class Transport {
public:
    Transport(); ~Transport();
    Transport(const Transport&) = delete; Transport& operator=(const Transport&) = delete;
    bool start(const TransportConfig&, TransportCallbacks, std::string& error);
    void stop();
    // Blocking connect/authenticate; call off UI thread. Explicit address supports LAN discovery restrictions.
    bool connect(const std::string& address, uint16_t port, std::string& peer_id, std::string& error);
    std::vector<Peer> peers() const;
    std::vector<Peer> visible_peers() const;
    // Explicit selection only. Sends bounded on-link magic packets and queues
    // discovery repair; never grants input authority or claims wake success.
    unsigned wake_peer(const std::string& peer);
    bool view_allowed(const std::string&) const;
    void set_view_parent(const std::string&,uint64_t session);
    ViewRoute view_route() const;
    // Nonzero only when all negotiated authenticated channels are ready. Changes on reconnect.
    uint64_t connection_token(const std::string& peer) const;
    void rename(const std::string& name);
    bool request_stream(const std::string& peer, const StreamSettings&);
    void stop_stream(const std::string& peer);
    bool preview(const std::string& peer,uint64_t session,bool enabled);
    bool cancel_warm(const std::string& peer, uint64_t session);
    // Revoke source output before calling. Removes its unsent video and queues a
    // session-scoped notice on the reliable control channel, keeping the pair
    // connected. False if the peer lacks FeatureHostKick or cannot queue it.
    // Notify promptly before slow teardown; call again after joining to remove
    // a last racing output. In-flight packets require receiver session checks.
    bool end_stream(const std::string& peer, uint64_t session);
    bool send_frame(const std::string& peer, const EncodedFrame&);
    bool send_input(const std::string& peer, const InputEvent&);
    // Dedicated input requires FeatureDedicatedInput. Queue Clipboard and Start
    // first, then begin_input; admit captured events only after input_ready.
    bool begin_input(const std::string& peer, uint64_t session);
    bool input_ready(const std::string& peer, uint64_t session) const;
    bool send_input(const std::string& peer, const InputPacket&);
    // Drop unsent old work, cancel its ACK deadline and queue one ordered release.
    bool end_input(const std::string& peer, uint64_t session);
    bool send_input_ack(const std::string& peer, const InputAck&);
    bool send_return_request(const std::string& peer,const ReturnRequest&);
    bool send_return_reply(const std::string& peer,const ReturnReply&);
    InputDiagnostics input_diagnostics(const std::string& peer) const;
    bool send_clipboard(const std::string& peer, const std::wstring&);
    bool send_clipboard_offer(const std::string&,const ClipboardOffer&);
    bool send_clipboard_packet(const std::string&,const std::vector<uint8_t>&);
    bool request_media_probe(const std::string& peer,uint64_t request,uint32_t mode);
    bool send_media_probe_report(const std::string& peer,const MediaProbeReport&);
    bool request_snapshot(const std::string& peer, uint64_t request);
    bool send_snapshot(const std::string& peer, const Snapshot&);
    bool send_cursor(const std::string& peer, const CursorState&);
    bool send_feedback(const std::string& peer, const StreamFeedback&);
    bool send_health(const std::string& peer, const StreamHealth&);
    bool send_diagnostics(const std::string& peer, const SourceDiagnostics&);
    bool frame_ready(const std::string& peer) const;
    // Local, bounded video sender timings. Empty/unavailable after disconnect;
    // lifetime counters reset when the underlying video connection changes.
    VideoSendDiagnostics video_send_diagnostics(const std::string& peer) const;
    // Read-only socket counters for explicit startup diagnostics; no payloads.
    std::string tcp_diagnostics(const std::string& peer) const;
    uint64_t received_video_bytes(const std::string& peer) const;
    bool measure_path(const std::string& peer,bool udp,uint32_t bitrate_bps,uint32_t duration_ms,PathProbeResult& result,std::string& error);
    static std::string generate_group_key();
    static bool self_test(std::string& result);
    static bool discovery_self_test(std::string& result);
private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
