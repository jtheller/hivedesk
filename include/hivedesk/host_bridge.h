#pragma once
#include "media.h"
#include "input_channel.h"
namespace hive {
// Local IPC only. The network-facing tray app stays at normal user integrity.
class HostBridge {
public:
    HostBridge();~HostBridge();
    bool start(const StreamSettings&,std::function<void(EncodedFrame)>,std::string&,
        std::function<bool()> ready={},std::function<void(const StreamHealth&)> health={},std::function<void(const SourceDiagnostics&)> diagnostics={},
        std::function<void(const InputAck&)> input_ack={});
    void stop();void request_keyframe();void feedback(const StreamFeedback&);
    // Call after downstream readiness changes, outside downstream queue locks.
    // Wakes a blocked frame-credit handoff without polling the scheduler.
    void notify_ready();
    bool activate();
    bool preview(bool enabled);
    bool input(const InputEvent&);
    bool input(const InputPacket&);
    // Close input authority immediately, then join only its own small-pipe ACK
    // reader. This never waits for video/control IPC or encoder work.
    void revoke_input();
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
