#pragma once
#include "types.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
namespace hive {
class MediaHost {
public:
    MediaHost(); ~MediaHost();
    // Primary screen only. HEVC 4:4:4 hardware encoding, no silent 4:2:0 fallback.
    bool start(const StreamSettings&, std::function<void(EncodedFrame)> output, std::string& error,
        std::function<bool()> ready={},std::function<void(const StreamHealth&)> health={},std::function<void(const SourceDiagnostics&)> diagnostics={});
    void stop();
    void request_keyframe();
    // Call after the supplied ready() predicate becomes true. Wakes the encoder
    // directly rather than waiting for the compatibility timeout.
    void notify_ready();
    bool running() const;
    void feedback(const StreamFeedback&);
private: struct Impl; std::unique_ptr<Impl> impl_;
};
class MediaViewer {
public:
    MediaViewer(); ~MediaViewer();
    // HWND belongs to UI; decoder/rendering must not depend on UI message pumping.
    bool open(HWND window, std::string& error);
    void submit(EncodedFrame frame);
    void close();
    std::string stats() const;
    bool has_frame() const;
    StreamFeedback feedback() const;
    ViewerDiagnostics diagnostics() const;
private: struct Impl; std::unique_ptr<Impl> impl_;
};
// Must actually encode/decode a generated 4:4:4 test image and verify reconstructed color/size.
bool media_self_test(std::string& report);
}
