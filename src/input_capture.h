#pragma once
#include "hivedesk/types.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hive {
// Posted to the owner; never executes UI, clipboard, or network work in a hook.
enum class CaptureAction : WPARAM { ReturnLocal=1, KickController, HidePanel, HidePanelAnimated, Summon, Failed };
struct InputCaptureState {
    uint64_t generation=0,session=0;
    std::string peer;
    bool remote=false,relative=false,settings_visible=false,panel_visible=false,panel_summoned=false,recording_hotkey=false;
    bool windowed=false,panel_dragging=false;HWND viewer{};
    RECT panel_rect{},settings_rect{},viewer_rect{};
    uint32_t screen_width=1,screen_height=1,remote_width=0,remote_height=0;
    uint32_t hotkey_key=0,hotkey_modifiers=0;
};
struct PhysicalPointer { POINT point{};uint64_t revision=0,motion_tick=0; };
struct InputCaptureDiagnostics {
    uint64_t captured=0,dispatched=0,coalesced=0,overflows=0,expired=0;
    uint32_t queue_depth=0,queue_peak=0,dispatch_p95_us=0,dispatch_max_us=0;
};
class InputCapture {
public:
    using Send=std::function<void(const std::string&,uint64_t,const InputEvent&,uint64_t)>;
    InputCapture();~InputCapture();
    InputCapture(const InputCapture&)=delete;InputCapture& operator=(const InputCapture&)=delete;
    bool start(HINSTANCE,HWND owner,UINT action_message,Send,std::string& error);
    void stop();
    void update(InputCaptureState);
    bool raw_mouse_available()const;
    PhysicalPointer physical_pointer()const;
    InputCaptureDiagnostics diagnostics()const;
    // Headless regression: exercises the real worker and routing snapshots
    // with an in-memory sink; installs no hooks and never injects OS input.
    static bool worker_self_test(std::string& result);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
