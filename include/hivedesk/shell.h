#pragma once
#include "types.h"
#include "input_diagnostics.h"
#include "clipboard.h"
#include "nested_return.h"
#include <windows.h>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>
namespace hive {
struct RelayPlan;
class MediaViewer;
// The renderer holds this lease until its worker/swap chain has stopped. Shell
// hides retired windows immediately and destroys them only after lease release.
struct ViewerSurface { HWND window{};std::string peer; };
struct ShellConfig {
    std::wstring device_name;
    std::string group_key;
    StreamSettings stream;
    DesktopPreferences desktop;
    std::vector<std::string> device_order;
};
struct ShellCallbacks {
    std::function<void(const std::string&)> select_peer;
    std::function<void()> return_local;
    // A physical local shortcut requests inbound controller teardown. The
    // callback runs on the owner thread, outside the low-level keyboard hook.
    std::function<void()> kick_controller;
    std::function<void(const InputEvent&)> input;
    // Dedicated input worker; immutable routing prevents a handoff from
    // sending an already captured event to the next peer/session.
    std::function<void(const std::string&,uint64_t,const InputEvent&,uint64_t)> captured_input;
    std::function<void(const std::wstring&)> clipboard_changed;
    std::function<void(Clipboard)> rich_clipboard_changed;
    std::function<void(const std::wstring&, const std::string&, const StreamSettings&)> save_settings;
    std::function<void(const std::string&)> connect_address;
    std::function<void()> panel_opened;
    std::function<void(const std::string&,uint64_t)> peek_peer;
    std::function<void(const std::vector<std::shared_ptr<const ViewerSurface>>&)> visible_peers;
    std::function<bool(const std::string&, const CursorState&)> cursor_changed;
    std::function<void()> stream_tick;
    std::function<void()> debug_tick;
    std::function<bool(const std::wstring&,const DesktopPreferences&)> save_preferences;
    std::function<bool()> restart_video;
    std::function<bool()> restart;
    std::function<bool(const std::vector<std::string>&)> save_order;
    std::function<bool(const std::string&,const ReturnRequest&)> return_request;
    std::function<bool(const std::string&,const ReturnReply&)> return_reply;
    std::function<void(const std::string&,uint64_t)> return_finished;
};
class Shell {
public:
    Shell(); ~Shell();
    // UI-thread shutdown while every callback owner is still alive.
    void close();
    bool create(HINSTANCE, const ShellConfig&, ShellCallbacks, std::string& error);
    int run();
    void quit();
    HWND viewer_window() const;
    std::shared_ptr<const ViewerSurface> viewer_surface() const;
    // Retry on a new HWND; never wait for a retired swapchain to release the old one.
    std::future<std::shared_ptr<const ViewerSurface>> renew_viewer_surface(std::shared_ptr<const ViewerSurface> expected);
    void discard_warm_surface(std::shared_ptr<const ViewerSurface>);
    void update_peers(const std::vector<Peer>&);
    void show_remote(const std::string& peer, const std::wstring& label,uint64_t session=0,HWND surface=nullptr);
    void show_reconnecting(const std::string& peer);
    void set_remote_size(uint32_t width, uint32_t height);
    void show_local();
    void cancel_peek(std::shared_ptr<const ViewerSurface>);
    void show_peek(std::shared_ptr<const ViewerSurface>,std::shared_ptr<MediaViewer>,uint64_t epoch,bool ready);
    void set_status(const std::wstring&);
    void show_error(const std::wstring&);
    // Synchronously marshal to UI, complete the write before returning, deduplicate listener events.
    void apply_clipboard(const std::wstring&);
    std::wstring cached_clipboard() const;
    void apply_clipboard(Clipboard);
    Clipboard cached_rich_clipboard() const;
    bool clipboard_busy() const;
    void show_settings();
    void update_thumbnail(const std::string& peer, Thumbnail);
    // JPEG decoding uses the shell worker, independent of connection startup.
    void update_snapshot(const std::string& peer, Snapshot);
    void observe_cursor(const std::string& controller,uint64_t session=0);
    void cursor_input_applied(uint64_t session,uint64_t capture_us);
    // The guard rechecks the authenticated inbound controller lease on the UI
    // thread and again before applying a delayed downstream reply.
    void return_one(const std::string& peer,ReturnRequest,std::function<bool()> authorized);
    void return_previous();
    RelayPlan route_remote(uint64_t host_session,const InputEvent&,bool desktop_available,bool isolated_test=false);
    void returned(const std::string& peer,ReturnReply);
    void update_cursor(const std::string& peer, CursorState);
    void update_cursor_composition(const std::string& peer,uint64_t session,uint64_t sequence,CursorComposition);
    void update_health(const std::string& peer,const StreamHealth&);
    void update_source_diagnostics(const std::string& peer,const SourceDiagnostics&);
    void update_viewer_diagnostics(const std::string& peer,const ViewerDiagnostics&);
    void update_input_diagnostics(const std::string& peer,const InputDiagnostics&);
    bool debug_enabled() const;
    // Atomically exclude a new Settings window from the final restart decision.
    bool prepare_video_restart();
    void cancel_video_restart();
#ifdef HIVEDESK_SHELL_TEST
    void taskbar_interaction_self_test();
    void quick_swap_interaction_self_test();
    void cursor_sync_self_test();
    void nested_return_self_test();
#endif
private: struct Impl; Impl* impl_ = nullptr;
};
// Primary screen injection. Track only injected held keys/buttons, release on explicit handoff.
void inject_input(const InputEvent&);
void release_injected_input();
}
