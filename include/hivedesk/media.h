#pragma once
#include "types.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
namespace hive {
struct CursorPresentation {
    uint64_t session=0,sequence=0;
    CursorComposition composition=CursorComposition::Unknown;
    bool valid=false;
};
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
    void activate();
    void preview(bool enabled);
    bool running() const;
    std::string failure() const;
    void feedback(const StreamFeedback&);
private: struct Impl; std::unique_ptr<Impl> impl_;
};
class MediaViewer {
public:
    MediaViewer(); ~MediaViewer();
    // HWND belongs to UI; decoder/rendering must not depend on UI message pumping.
    bool open(HWND window, std::string& error,
        std::function<void(const CursorPresentation&)> presented={},bool warm=false);
    // Prewarm prepares pixels without touching the hidden swapchain. Selection
    // enables presentation after the UI has assigned its final window/style.
    void activate();
    void preview(bool enabled);
    void peek_opacity(bool visible,bool animate=true);
    void set_display_cadence(bool enabled);
    void submit(EncodedFrame frame);
    void request_stop(); // Invalidate work immediately; close() joins separately.
    void close();
    std::string stats() const;
    bool has_frame() const;
    // First successful Present submission for this open; not a scanout claim.
    bool has_presented_frame() const;
    CursorPresentation cursor_presentation() const;
    StreamFeedback feedback() const;
    ViewerDiagnostics diagnostics() const;
private: struct Impl; std::unique_ptr<Impl> impl_;
};
// Must actually encode/decode a generated 4:4:4 test image and verify reconstructed color/size.
bool media_self_test(std::string& report);
bool media_prewarm_capacity();
}
