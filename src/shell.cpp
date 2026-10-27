#include "hivedesk/shell.h"
#include "panel_composition.h"
#include "debug_model.h"
#include <optional>
#include "hivedesk/network_key.h"
#include "hivedesk/desktop_aux.h"
#include "hivedesk/cursor_composition.h"
#include "hivedesk/adaptive.h"
#include "hivedesk/auto_rate.h"
#include "hivedesk/gpu_policy_restart.h"
#include "input_capture.h"
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <thread>
#include <filesystem>

namespace hive {
namespace {
constexpr UINT UiTask=WM_APP+41, TrayMessage=WM_APP+42, ClipboardApply=WM_APP+43, KickControllerMessage=WM_APP+47, CaptureActionMessage=WM_APP+48;
constexpr ULONG_PTR InputMarker=0x48495645;
constexpr COLORREF Background=RGB(19,22,29), Card=RGB(34,39,49), Selected=RGB(37,66,80);
constexpr COLORREF Accent=RGB(94,217,213), Text=RGB(240,243,247), Muted=RGB(162,172,188);
constexpr int Name=101, Save=111, Close=113, Startup=117, TrayIcon=118, Hotkey=119, DebugOverlay=120, RelativeMouse=121;
std::wstring wide(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(!n) return std::wstring(s.begin(),s.end());
    std::wstring r(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),r.data(),n); return r;
}
std::string utf8(const std::wstring& s) {
    if(s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string r(n,'\0'); WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),r.data(),n,nullptr,nullptr); return r;
}
std::wstring control_text(HWND h) { int n=GetWindowTextLengthW(h); std::wstring r(n+1,L'\0'); GetWindowTextW(h,r.data(),n+1); r.resize(n); return r; }
void fill(HDC dc,RECT rc,COLORREF c) { HBRUSH b=CreateSolidBrush(c); FillRect(dc,&rc,b); DeleteObject(b); }
void draw_text(HDC dc,HFONT font,const std::wstring& s,RECT rc,COLORREF c) {
    HGDIOBJ old=SelectObject(dc,font); SetTextColor(dc,c); SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,s.c_str(),static_cast<int>(s.size()),&rc,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS); SelectObject(dc,old);
}
bool number(const std::wstring& s,uint32_t& out) {
    if(s.empty()||s.size()>10) return false;
    uint64_t v=0; for(wchar_t c:s) { if(c<L'0'||c>L'9') return false; v=v*10+(c-L'0'); }
    if(v>UINT32_MAX) return false; out=static_cast<uint32_t>(v); return true;
}
}
struct Shell::Impl {
    HINSTANCE instance{};HICON app_icon{},app_small_icon{}; HWND owner{},viewer{},panel{},settings{},prior_focus{};
    HANDLE settings_process{};HWND health_badge{};
    gpu_policy_restart::SettingsAdmission settings_restart;
    StreamHealth stream_health{};SlowIndicator slow_indicator;ULONGLONG last_stream_tick=0;
    SourceDiagnostics source_diagnostics{};ViewerDiagnostics viewer_diagnostics{};ULONGLONG source_stats_time=0,health_time=0;
    struct PendingSource {std::string peer;SourceDiagnostics value;uint64_t generation;ULONGLONG received;};
    std::mutex source_mutex;std::optional<PendingSource> pending_source;
    InputDiagnostics input_diagnostics{};
    DebugSurface debug_surface;HFONT debug_font{};std::array<DiagnosticPulse,6> debug_pulses{};
    HFONT font{},title_font{},small_font{}; HBRUSH background_brush{},edit_brush{};
    DWORD ui_thread{}; UINT taskbar_created{}; int dpi=96;
    ShellConfig config; ShellCallbacks callbacks; std::vector<Peer> peers;
    std::string active_id,previous_id,pending_id;
    std::map<std::string,HBITMAP> thumbnails;
    std::map<std::string,HBITMAP> prior_thumbnails;
    std::map<std::string,ULONGLONG> thumbnail_fades;
    std::string cursor_controller;
    HCURSOR sent_cursor{},remote_cursor{};
    bool cursor_sent=false,sent_cursor_visible=false,remote_cursor_visible=true,remote_cursor_received=false;
    uint64_t pointer_revision=1,sent_pointer_revision=0,remote_pointer_revision=0;
    CursorState remote_pointer;
    PresentedCursorComposition remote_cursor_composition;
    POINT physical_pointer{};
    ULONGLONG last_local_motion=0;
    InputCapture input_capture;uint64_t input_session=0,last_capture_pointer_revision=0;
    std::wstring status=L"Ready on your local network";
    bool remote=false,shutting_down=false,selection_armed=false,reconnecting=false;
    bool relative_mode=false,raw_mouse_available=false;
    PanelComposition composition;
    COLORREF accent=GetSysColor(COLOR_HIGHLIGHT);
    uint32_t remote_width=0,remote_height=0;
    int panel_mode=0,panel_width=200,panel_height=0,hovered=-1,scroll=0;
    bool panel_closing=false,panel_summoned=false,tray_present=false,recording_hotkey=false,hotkey_registered=false;
    int hotkey_id=81;
    ULONGLONG outside_since=0;
    mutable std::mutex cache_mutex; std::wstring clipboard_cache;
    std::atomic<uint64_t> generation{0};
    std::mutex work_mutex; std::condition_variable work_ready; std::deque<std::function<void()>> work; bool stop_worker=false; std::thread worker;
    struct ClipboardJob { std::mutex mutex; std::wstring value; std::string error; bool done=false; };
    std::mutex jobs_mutex; std::map<uint64_t,std::shared_ptr<ClipboardJob>> jobs; std::atomic<uint64_t> next_job{1};
    int px(int v) const { return MulDiv(v,dpi,96); }
    void enqueue(std::function<void()> f) { { std::lock_guard lock(work_mutex);if(stop_worker)return;work.push_back(std::move(f)); }work_ready.notify_one(); }
    void dispatch(std::function<void()> f) {
        if(GetCurrentThreadId()==ui_thread) { f();return; }
        auto task=std::make_unique<std::function<void()>>(std::move(f));
        if(owner&&PostMessageW(owner,UiTask,0,reinterpret_cast<LPARAM>(task.get())))task.release();
    }
    void publish_input() {
        InputCaptureState state;state.generation=generation.load();state.session=input_session;state.peer=active_id;
        state.remote=remote&&!reconnecting&&!shutting_down;state.relative=relative_mode;state.recording_hotkey=recording_hotkey;
        state.panel_visible=panel&&IsWindowVisible(panel);state.panel_summoned=panel_summoned;
        state.settings_visible=settings&&IsWindowVisible(settings);
        if(state.panel_visible)GetWindowRect(panel,&state.panel_rect);if(state.settings_visible)GetWindowRect(settings,&state.settings_rect);
        state.screen_width=uint32_t(std::max(1,GetSystemMetrics(SM_CXSCREEN)));state.screen_height=uint32_t(std::max(1,GetSystemMetrics(SM_CYSCREEN)));
        state.remote_width=remote_width;state.remote_height=remote_height;
        state.hotkey_key=config.desktop.hotkey_key;state.hotkey_modifiers=config.desktop.hotkey_modifiers;input_capture.update(std::move(state));
    }
    void sample_physical_pointer() {
        const auto pointer=input_capture.physical_pointer();
        if(pointer.revision!=last_capture_pointer_revision){last_capture_pointer_revision=pointer.revision;++pointer_revision;physical_pointer=pointer.point;last_local_motion=pointer.motion_tick;}
    }
    void hide_panel(bool animate=false) {
        panel_summoned=false;
        if(animate&&panel_mode&&!panel_closing){panel_closing=true;SetTimer(owner,2,composition.hide(),nullptr);publish_input();return;}
        if(animate&&panel_closing)return;
        panel_closing=false;KillTimer(owner,2);panel_mode=0;outside_since=0;hovered=-1;
        if(panel)ShowWindow(panel,SW_HIDE);
        publish_input();
    }
    void remember_view() {
        // Capture only this app's visible viewer, never settings or another foreground app.
        // Hide our own Switcher and wait for composition before reading back the last view.
        if(active_id.empty()||!viewer||!IsWindowVisible(viewer)||(settings&&IsWindowVisible(settings))||GetForegroundWindow()!=viewer)return;
        hide_panel();UpdateWindow(viewer);DwmFlush();
        RECT client{};GetClientRect(viewer,&client);if(client.right<=0||client.bottom<=0)return;
        HDC source=GetDC(viewer);if(!source)return;
        HDC destination=CreateCompatibleDC(source);HBITMAP image=CreateCompatibleBitmap(source,672,378);
        if(!destination||!image){if(destination)DeleteDC(destination);if(image)DeleteObject(image);ReleaseDC(viewer,source);return;}
        HGDIOBJ old=SelectObject(destination,image);SetStretchBltMode(destination,HALFTONE);SetBrushOrgEx(destination,0,0,nullptr);
        BOOL copied=StretchBlt(destination,0,0,672,378,source,0,0,client.right,client.bottom,SRCCOPY);
        SelectObject(destination,old);DeleteDC(destination);ReleaseDC(viewer,source);
        if(copied){auto& cached=thumbnails[active_id];if(cached)DeleteObject(cached);cached=image;}else DeleteObject(image);
    }
    void reset_remote_cursor() {
        stream_health={};source_diagnostics={};viewer_diagnostics={};input_diagnostics={};source_stats_time=health_time=0;debug_pulses={};slow_indicator.reset();if(health_badge)ShowWindow(health_badge,SW_HIDE);
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));if(remote_cursor)DestroyCursor(remote_cursor);
        remote_cursor=nullptr;remote_cursor_visible=true;remote_cursor_received=false;remote_pointer_revision=0;remote_pointer={};
        remote_cursor_composition.reset();
    }
    HCURSOR selected_remote_cursor() const {
        return remote_cursor_composition.visible(remote_cursor_visible,relative_mode)?
            (remote_cursor?remote_cursor:LoadCursorW(nullptr,IDC_ARROW)):nullptr;
    }
    void apply_remote_cursor() {
        POINT point{};GetCursorPos(&point);
        if(remote&&WindowFromPoint(point)==viewer)
            SetCursor(selected_remote_cursor());
    }
    void apply_remote_position() {
        sample_physical_pointer();
        if(!remote||relative_mode||!remote_cursor_received||!remote_width||!remote_height)return;
        if(remote_pointer.position_revision==remote_pointer_revision)return;
        remote_pointer_revision=remote_pointer.position_revision;
        POINT point{};GetCursorPos(&point);
        // Never pull against recent local movement or take the pointer out of our UI.
        if(own_surface(point)||GetTickCount64()-last_local_motion<100)return;
        const double width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
        const double scale=std::min(width/remote_width,height/remote_height);
        const double x=(width-remote_width*scale)/2+(remote_pointer.x/65535.0)*(remote_width-1)*scale;
        const double y=(height-remote_height*scale)/2+(remote_pointer.y/65535.0)*(remote_height-1)*scale;
        INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE;
        input.mi.dx=LONG((x+.5)*65536/width);input.mi.dy=LONG((y+.5)*65536/height);input.mi.dwExtraInfo=InputMarker;
        SendInput(1,&input,sizeof(input));
    }
    void sample_cursor() {
        sample_physical_pointer();
        if(cursor_controller.empty()||!callbacks.cursor_changed)return;
        CURSORINFO info{sizeof(info)};if(!GetCursorInfo(&info))return;
        const int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
        const bool visible=(info.flags&CURSOR_SHOWING)&&info.ptScreenPos.x>=0&&info.ptScreenPos.y>=0&&info.ptScreenPos.x<width&&info.ptScreenPos.y<height;
        const bool shape_changed=!cursor_sent||info.hCursor!=sent_cursor;
        if(cursor_sent&&!shape_changed&&visible==sent_cursor_visible&&pointer_revision==sent_pointer_revision)return;
        CursorState state;state.visible=visible;state.position_revision=pointer_revision;
        // The revision and position describe physical/programmatic host motion,
        // not the delayed echo of this controller's injected mouse movement.
        const POINT position=cursor_sent?physical_pointer:info.ptScreenPos;
        state.x=uint32_t(std::clamp(position.x,0L,LONG(std::max(1,width-1)))*65535LL/std::max(1,width-1));
        state.y=uint32_t(std::clamp(position.y,0L,LONG(std::max(1,height-1)))*65535LL/std::max(1,height-1));
        if(shape_changed&&info.hCursor&&!read_cursor_shape(info.hCursor,state))return;
        if(callbacks.cursor_changed(cursor_controller,state)) {
            cursor_sent=true;sent_cursor=info.hCursor;sent_cursor_visible=visible;sent_pointer_revision=pointer_revision;
        }
    }
    void local(bool notify) {
        ++generation;remote=false;reconnecting=false;input_session=0;publish_input();
        remember_view();
        selection_armed=false;pending_id.clear();
        if(!active_id.empty())previous_id=active_id;active_id.clear();
        ShowWindow(viewer,SW_HIDE);hide_panel();ReleaseCapture();ClipCursor(nullptr);reset_remote_cursor();
        if(prior_focus&&IsWindow(prior_focus))SetForegroundWindow(prior_focus);prior_focus=nullptr;
        if(notify&&callbacks.return_local)callbacks.return_local();
    }
    void select(const std::string& id) {
        if(id.empty()){local(true);return;}
        if(id==active_id&&remote){hide_panel();return;}
        auto it=std::find_if(peers.begin(),peers.end(),[&](const Peer& p){return p.id==id;});
        if(it==peers.end()){status=L"Computer is no longer in the device list.";InvalidateRect(panel,nullptr,FALSE);return;}
        ++generation;remote=false;reconnecting=false;input_session=0;publish_input();
        remember_view();
        reset_remote_cursor();
        remote_width=remote_height=0;selection_armed=true;pending_id=id;ReleaseCapture();ClipCursor(nullptr);hide_panel();
        if(callbacks.select_peer)callbacks.select_peer(id);
    }
    void show_remote_ui(const std::string& id,const std::wstring& label,uint64_t session) {
        if(shutting_down||!selection_armed||(!pending_id.empty()&&pending_id!=id))return;
        reconnecting=false;InvalidateRect(health_badge,nullptr,FALSE);
        if(active_id!=id)previous_id=active_id;
        if(!remote&&!IsWindowVisible(viewer))prior_focus=GetForegroundWindow();
        if(!remote||active_id!=id||input_session!=session)remote_cursor_composition.reset(session);
        active_id=id;input_session=session;remote=true;SetWindowTextW(viewer,(L"HiveDesk - "+label).c_str());
        SetWindowPos(viewer,HWND_TOP,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),SWP_SHOWWINDOW);
        SetForegroundWindow(viewer);SetFocus(viewer);confine_mouse();hide_panel();apply_remote_position();apply_remote_cursor();
        publish_input();
    }
    void confine_mouse() {
        if(!remote){ClipCursor(nullptr);return;}
        int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
        RECT r{0,0,width,height};
        if(relative_mode){int x=width/2,y=height/2;r={x,y,x+1,y+1};SetCursorPos(x,y);}
        ClipCursor(&r);
    }
    void set_relative(bool enabled) {
        relative_mode=enabled&&raw_mouse_available;confine_mouse();
        apply_remote_cursor();
        publish_input();
        status=relative_mode?L"Relative mouse on. Ctrl+Alt+` returns locally.":L"Absolute desktop mouse";
        InvalidateRect(panel,nullptr,FALSE);
    }
    bool own_surface(POINT p) const {
        RECT r{};if(IsWindowVisible(panel)){GetWindowRect(panel,&r);if(PtInRect(&r,p))return true;}
        if(settings&&IsWindowVisible(settings)){GetWindowRect(settings,&r);if(PtInRect(&r,p))return true;}return false;
    }
    void log_hotkey_conflict(){status=L"Show Hive shortcut is already in use. Choose another in Settings.";OutputDebugStringW(status.c_str());}
    void tray(bool remove=false) {
        remove=remove||!config.desktop.tray_icon;
        if(!remove&&tray_present)return;
        NOTIFYICONDATAW n{};n.cbSize=sizeof(n);n.hWnd=owner;n.uID=1;n.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;n.uCallbackMessage=TrayMessage;
        n.hIcon=app_small_icon;wcscpy_s(n.szTip,L"HiveDesk - Ctrl+Alt+` returns to this computer");
        if(remove){Shell_NotifyIconW(NIM_DELETE,&n);tray_present=false;RemovePropW(owner,L"HiveDesk.TrayVisible");}
        else if(Shell_NotifyIconW(NIM_ADD,&n)){tray_present=true;SetPropW(owner,L"HiveDesk.TrayVisible",reinterpret_cast<HANDLE>(1));n.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&n);}
    }
    void tray_menu() {
        HMENU m=CreatePopupMenu();AppendMenuW(m,MF_STRING,1,L"Computers");AppendMenuW(m,MF_STRING,2,L"Settings");
        AppendMenuW(m,MF_STRING,3,L"Return to this computer");AppendMenuW(m,MF_SEPARATOR,0,nullptr);AppendMenuW(m,MF_STRING,5,L"Restart HiveDesk");AppendMenuW(m,MF_STRING,4,L"Quit HiveDesk");
        POINT p{};GetCursorPos(&p);SetForegroundWindow(owner);
        int chosen=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,owner,nullptr);DestroyMenu(m);PostMessageW(owner,WM_NULL,0,0);
        if(chosen==1)show_panel(2);else if(chosen==2)settings_ui();else if(chosen==3)local(true);else if(chosen==4)SendMessageW(owner,WM_CLOSE,0,0);
        else if(chosen==5){if(!callbacks.restart||!callbacks.restart())MessageBoxW(owner,L"Could not start restart helper. HiveDesk is still running.",L"HiveDesk",MB_OK|MB_ICONERROR);}
    }
    std::pair<std::wstring,std::wstring> card_label(int index) const {
        if(index==0)return {config.device_name.empty()?L"This computer":config.device_name,L"This computer"};
        if(index<1||index>static_cast<int>(peers.size()))return {L"No other computers",L"Open HiveDesk on another computer"};
        const Peer& p=peers[index-1];return {wide(p.name.empty()?p.address:p.name),wide(p.address)+(p.authenticated?L"":L" / Discovered")};
    }
    int preview_index() const {
        if(!previous_id.empty())for(size_t i=0;i<peers.size();++i)if(peers[i].id==previous_id)return static_cast<int>(i)+1;
        if(!active_id.empty())return 0;
        for(size_t i=0;i<peers.size();++i)if(peers[i].authenticated)return static_cast<int>(i)+1;return 0;
    }
    int tile_height() const { return (panel_width-2*px(12))*9/16; }
    int tile_stride() const { return tile_height()+px(16); }
    int visible_tiles() const { return std::max(1,(panel_height-px(8))/tile_stride()); }
    void system_color() {
        DWORD value=0;BOOL opaque=FALSE;
        if(SUCCEEDED(DwmGetColorizationColor(&value,&opaque)))accent=RGB((value>>16)&255,(value>>8)&255,value&255);
        if(panel)InvalidateRect(panel,nullptr,FALSE);
    }
    void show_panel(int mode) {
        if(settings&&IsWindowVisible(settings))return;
        if(panel_mode==mode&&!panel_closing)return;
        const bool entering=panel_mode==0,reversing=panel_closing;const int previous_height=panel_height;
        panel_closing=false;KillTimer(owner,2);panel_mode=mode;hovered=-1;outside_since=0;
        panel_width=px(200);panel_height=mode==1?tile_height()+px(24):GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(panel,HWND_TOPMOST,0,0,panel_width,panel_height,SWP_NOACTIVATE);
        InvalidateRect(panel,nullptr,FALSE);UpdateWindow(panel);
        composition.show(float(panel_width),float(panel_height),float(previous_height),entering,reversing);
        SetWindowPos(panel,HWND_TOPMOST,0,0,panel_width,panel_height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        publish_input();
        if(entering&&callbacks.panel_opened)callbacks.panel_opened();
    }
    int card_at(POINT p) const {
        if(p.x<px(12)||p.x>=panel_width-px(12)||p.y<px(12))return -1;
        if(panel_mode==1)return p.y<px(12)+tile_height()?preview_index():-1;
        const int row=(p.y-px(12))/tile_stride();
        if(row>=visible_tiles()||(p.y-px(12))%tile_stride()>=tile_height())return -1;
        const int index=row+scroll;return index<=static_cast<int>(peers.size())?index:-1;
    }
    DebugRows debug_rows() {
        const auto peer=std::find_if(peers.begin(),peers.end(),[&](const Peer& p){return p.id==active_id&&p.authenticated;});
        return build_debug_rows({remote_width,remote_height,stream_health,source_diagnostics,viewer_diagnostics,input_diagnostics,GetTickCount64(),source_stats_time,health_time,
            wide(HIVEDESK_VERSION),peer==peers.end()?std::wstring{}:wide(peer->version)},debug_pulses);
    }
    void paint_health() {
        RECT rect{};GetClientRect(health_badge,&rect);DebugRows rows;
        if(reconnecting)rows={{{L"Reconnecting...  Ctrl + Alt + ` to cancel",DiagnosticTone::Warning}}};
        else if(config.desktop.debug_overlay)rows=debug_rows();
        else rows={{{(stream_health.pressure&PressureNetwork)?L"Slow connection":L"Device busy",DiagnosticTone::Warning}}};
        if(debug_surface.render(rect.right,rect.bottom,dpi,debug_font,rows,reconnecting||!config.desktop.debug_overlay))debug_surface.present(health_badge);
    }
    void timer() {
        // Latest-state mailbox: no per-report UI messages or stale report FIFO.
        std::optional<PendingSource> incoming;
        {std::lock_guard lock(source_mutex);incoming=std::move(pending_source);pending_source.reset();}
        if(incoming&&incoming->generation==generation.load()&&incoming->peer==active_id&&incoming->value.session==input_session){source_diagnostics=incoming->value;source_stats_time=incoming->received;}
        sample_cursor();
        if(GetTickCount64()-last_stream_tick>=100){last_stream_tick=GetTickCount64();if(callbacks.stream_tick)callbacks.stream_tick();}
        const bool warning=slow_indicator.update(GetTickCount64(),stream_health);
        if(health_badge) {
            if(remote&&(reconnecting||warning||config.desktop.debug_overlay)&&IsWindowVisible(viewer)&&GetForegroundWindow()==viewer) {
                if(!reconnecting&&config.desktop.debug_overlay&&callbacks.debug_tick)callbacks.debug_tick();
                const int width=px(reconnecting?280:config.desktop.debug_overlay?460:136),height=px(!reconnecting&&config.desktop.debug_overlay?474:30);RECT bounds{};GetWindowRect(health_badge,&bounds);
                const int inset=px(16);
                if(!IsWindowVisible(health_badge)||bounds.left!=inset||bounds.top!=inset||bounds.right-bounds.left!=width||bounds.bottom-bounds.top!=height){SetWindowPos(health_badge,HWND_TOPMOST,inset,inset,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);InvalidateRect(health_badge,nullptr,FALSE);}
                if(config.desktop.debug_overlay||reconnecting)InvalidateRect(health_badge,nullptr,FALSE);
            }else if(IsWindowVisible(health_badge))ShowWindow(health_badge,SW_HIDE);
        }
        if(panel_mode&&!thumbnail_fades.empty())InvalidateRect(panel,nullptr,FALSE);
        const auto fade_time=GetTickCount64();
        for(auto it=thumbnail_fades.begin();it!=thumbnail_fades.end();) {
            if(fade_time-it->second>=180){auto old=prior_thumbnails.find(it->first);if(old!=prior_thumbnails.end()){DeleteObject(old->second);prior_thumbnails.erase(old);}it=thumbnail_fades.erase(it);}
            else ++it;
        }
        if(settings_process){
            if(WaitForSingleObject(settings_process,0)==WAIT_OBJECT_0){CloseHandle(settings_process);settings_process=nullptr;settings=nullptr;settings_restart.close();if(recording_hotkey){recording_hotkey=false;hotkey_registered=RegisterHotKey(owner,hotkey_id,config.desktop.hotkey_modifiers|MOD_NOREPEAT,config.desktop.hotkey_key)!=FALSE;}}
            else if(!settings)settings=external_settings_window();
        }
        publish_input();
        POINT p{};GetCursorPos(&p);ULONGLONG now=GetTickCount64();
        const bool corner=p.x>=0&&p.x<=px(3)&&p.y>=0&&p.y<=px(3);
        // Start the fade on the first corner sample; no hover dwell before showing it.
        if(!panel_mode){if(corner)show_panel(1);return;}
        if(panel_closing){if(p.x>=0&&p.x<panel_width&&p.y>=0&&p.y<panel_height)show_panel(panel_mode);return;}
        if(panel_mode==1&&p.x>=0&&p.x<px(42)&&p.y>px(32))show_panel(2);
        if(!panel_summoned&&(p.x<0||p.x>panel_width+px(40)||p.y<0||p.y>panel_height+px(30))) {if(!outside_since)outside_since=now;if(now-outside_since>250)hide_panel(true);}else outside_since=0;
    }
    void paint_panel() {
        PAINTSTRUCT ps{};BeginPaint(panel,&ps);RECT r{};GetClientRect(panel,&r);
        const int width=std::max(1L,r.right),height=std::max(1L,r.bottom);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!dc||!bitmap){if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);EndPaint(panel,&ps);return;}
        HGDIOBJ old=SelectObject(dc,bitmap);
        auto shade=[&](int amount){return RGB(GetRValue(accent)*amount/100,GetGValue(accent)*amount/100,GetBValue(accent)*amount/100);};
        fill(dc,r,shade(16));
        auto card=[&](int index,int y){
            const bool selected=index==0?active_id.empty():peers[index-1].id==active_id;
            RECT box{px(12),y,panel_width-px(12),y+tile_height()};
            fill(dc,box,shade(45));
            const auto cached=thumbnails.find(index==0?std::string{}:peers[index-1].id);
            if(cached!=thumbnails.end()) {
                auto draw_bitmap=[&](HBITMAP bitmap,BYTE opacity) {
                    HDC source=CreateCompatibleDC(dc);HGDIOBJ previous=SelectObject(source,bitmap);
                    SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
                    if(opacity==255)StretchBlt(dc,box.left,box.top,box.right-box.left,box.bottom-box.top,source,0,0,672,378,SRCCOPY);
                    else {BLENDFUNCTION blend{AC_SRC_OVER,0,opacity,0};AlphaBlend(dc,box.left,box.top,box.right-box.left,box.bottom-box.top,source,0,0,672,378,blend);}
                    SelectObject(source,previous);DeleteDC(source);
                };
                const auto fade=thumbnail_fades.find(cached->first);BYTE opacity=255;
                if(fade!=thumbnail_fades.end()) {
                    opacity=BYTE(std::min<ULONGLONG>(180,GetTickCount64()-fade->second)*255/180);
                    const auto prior=prior_thumbnails.find(cached->first);if(prior!=prior_thumbnails.end())draw_bitmap(prior->second,255);
                }
                draw_bitmap(cached->second,opacity);
            } else {
                const int cx=(box.left+box.right)/2,cy=(box.top+box.bottom)/2-px(8);
                HPEN pen=CreatePen(PS_SOLID,px(2),RGB(230,230,230));HGDIOBJ old_pen=SelectObject(dc,pen),old_brush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
                Rectangle(dc,cx-px(27),cy-px(16),cx+px(27),cy+px(15));
                MoveToEx(dc,cx,cy+px(15),nullptr);LineTo(dc,cx,cy+px(21));
                MoveToEx(dc,cx-px(12),cy+px(21),nullptr);LineTo(dc,cx+px(12),cy+px(21));
                SelectObject(dc,old_brush);SelectObject(dc,old_pen);DeleteObject(pen);
            }
            const auto name=card_label(index).first;
            HGDIOBJ previous=SelectObject(dc,small_font);SIZE extent{};GetTextExtentPoint32W(dc,name.c_str(),static_cast<int>(name.size()),&extent);SelectObject(dc,previous);
            RECT badge{box.left,box.bottom-px(24),std::min(box.right,box.left+extent.cx+px(16)),box.bottom};
            fill(dc,badge,accent);
            const COLORREF ink=(GetRValue(accent)*299+GetGValue(accent)*587+GetBValue(accent)*114)>150000?RGB(0,0,0):RGB(255,255,255);
            RECT label=badge;label.left+=px(8);label.right-=px(8);draw_text(dc,small_font,name,label,ink);
            if(selected||hovered==index){const int edge=px(selected?3:2);const COLORREF border=selected?accent:RGB(255,255,255);
                fill(dc,{box.left-edge,box.top-edge,box.right+edge,box.top},border);fill(dc,{box.left-edge,box.bottom,box.right+edge,box.bottom+edge},border);
                fill(dc,{box.left-edge,box.top,box.left,box.bottom},border);fill(dc,{box.right,box.top,box.right+edge,box.bottom},border);}
        };
        if(panel_mode==1)card(preview_index(),px(12));
        else for(int i=scroll;i<=static_cast<int>(peers.size())&&i<scroll+visible_tiles();++i)card(i,px(12)+(i-scroll)*tile_stride());
        GdiFlush();auto* rgba=static_cast<uint32_t*>(pixels);for(size_t i=0;i<size_t(width)*height;++i)rgba[i]|=0xff000000u;
        try{composition.present(pixels,width,height);}catch(const std::exception&){OutputDebugStringW(L"HiveDesk Switcher composition update failed\n");}
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);EndPaint(panel,&ps);
    }

    void read_clipboard(bool notify) {
        if(!IsClipboardFormatAvailable(CF_UNICODETEXT)||!OpenClipboard(owner))return;
        HANDLE data=GetClipboardData(CF_UNICODETEXT);std::wstring value;bool valid=false;
        if(data){SIZE_T bytes=GlobalSize(data);const wchar_t* ptr=static_cast<const wchar_t*>(GlobalLock(data));
            if(ptr&&bytes>=sizeof(wchar_t)&&bytes<=16*1024*1024){size_t count=bytes/sizeof(wchar_t),len=0;while(len<count&&ptr[len])++len;if(len<count){value.assign(ptr,len);valid=true;}}
            if(ptr)GlobalUnlock(data);
        }
        CloseClipboard();if(!valid)return;
        {std::lock_guard lock(cache_mutex);if(value==clipboard_cache)return;clipboard_cache=value;}
        if(notify)enqueue([this,value]{{std::lock_guard lock(cache_mutex);if(value!=clipboard_cache)return;}if(callbacks.clipboard_changed)callbacks.clipboard_changed(value);});
    }
    void write_clipboard(const std::wstring& value) {
        if(value.size()>8*1024*1024)throw std::runtime_error("Clipboard text exceeds 16 MiB.");
        size_t bytes=(value.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
        if(!memory)throw std::runtime_error("Cannot allocate clipboard text.");
        void* ptr=GlobalLock(memory);if(!ptr){GlobalFree(memory);throw std::runtime_error("Cannot lock clipboard storage.");}
        memcpy(ptr,value.c_str(),bytes);GlobalUnlock(memory);
        if(!OpenClipboard(owner)){GlobalFree(memory);throw std::runtime_error("Clipboard is busy; handoff refused.");}
        bool success=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,memory);CloseClipboard();
        if(!success){GlobalFree(memory);throw std::runtime_error("Could not replace the text clipboard.");}
        {std::lock_guard lock(cache_mutex);clipboard_cache=value;}
    }
    void apply_sync(const std::wstring& value) {
        if(GetCurrentThreadId()==ui_thread){write_clipboard(value);return;}
        auto job=std::make_shared<ClipboardJob>();job->value=value;uint64_t token=next_job.fetch_add(1);
        {std::lock_guard lock(jobs_mutex);jobs.emplace(token,job);}
        DWORD_PTR result=0;bool sent=SendMessageTimeoutW(owner,ClipboardApply,static_cast<WPARAM>(token),0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&result)!=0;
        {std::lock_guard lock(jobs_mutex);jobs.erase(token);}
        if(!sent)throw std::runtime_error("Clipboard UI handoff timed out.");
        std::lock_guard result_lock(job->mutex);if(!job->done)throw std::runtime_error(job->error.empty()?"Clipboard UI handoff failed.":job->error);
    }
    HWND child(const wchar_t* type,const wchar_t* label,DWORD style,int id,int x,int y,int w,int h) {
        HWND control=CreateWindowExW(wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,label,WS_CHILD|WS_VISIBLE|style,px(x),px(y),px(w),px(h),settings,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return control;
    }
    HWND external_settings_window() const {
        struct Search {DWORD process;HWND window;};Search search{GetProcessId(settings_process),nullptr};
        EnumWindows([](HWND window,LPARAM arg)->BOOL {auto& search=*reinterpret_cast<Search*>(arg);DWORD process=0;GetWindowThreadProcessId(window,&process);
            if(process==search.process&&IsWindowVisible(window)&&GetWindow(window,GW_OWNER)==nullptr){search.window=window;return FALSE;}return TRUE;
        },reinterpret_cast<LPARAM>(&search));return search.window;
    }
    bool launch_settings() {
        if(settings_process&&WaitForSingleObject(settings_process,0)==WAIT_TIMEOUT){settings=external_settings_window();if(settings){ShowWindow(settings,SW_SHOWNORMAL);SetForegroundWindow(settings);}return true;}
        if(settings_process){CloseHandle(settings_process);settings_process=nullptr;settings=nullptr;}
        wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
        const auto exe=std::filesystem::path(module).parent_path()/L"settings"/L"HiveDesk.Settings.exe";
        if(!std::filesystem::exists(exe))return false;
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        std::wstring command=L"\""+exe.wstring()+L"\"";
        if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,exe.parent_path().c_str(),&startup,&process)){
            settings_restart.close();MessageBoxW(owner,L"Could not open Settings. Reinstall HiveDesk to restore the settings files.",L"HiveDesk",MB_OK|MB_ICONERROR);return true;}
        CloseHandle(process.hThread);settings_process=process.hProcess;return true;
    }
    LRESULT settings_command(WPARAM sender,const COPYDATASTRUCT* packet) {
        DWORD process=0;GetWindowThreadProcessId(reinterpret_cast<HWND>(sender),&process);
        if(!settings_process||process!=GetProcessId(settings_process)||!packet||packet->dwData!=0x48445631||!packet->lpData||packet->cbData<2||packet->cbData>8192||packet->cbData%2)return 0;
        const auto* chars=static_cast<const wchar_t*>(packet->lpData);const size_t count=packet->cbData/sizeof(wchar_t);if(chars[count-1]!=0)return 0;
        std::vector<std::wstring> fields;size_t start=0;for(size_t i=0;i<count;++i)if(!chars[i]){fields.emplace_back(chars+start,i-start);start=i+1;}
        if(fields.empty())return 0;
        if(fields[0]==L"restart-video"&&fields.size()==1)return callbacks.restart_video&&callbacks.restart_video()?1:0;
        if(fields[0]==L"preferences"&&(fields.size()==6||fields.size()==7||fields.size()==8)){
            DesktopPreferences desktop=config.desktop;uint32_t startup=0,tray_icon=0,debug=0,relative=0;
            if(!number(fields[2],startup)||startup>1||!number(fields[3],tray_icon)||tray_icon>1||!number(fields[4],desktop.hotkey_modifiers)||!number(fields[5],desktop.hotkey_key))return 0;
            desktop.startup=startup!=0;desktop.tray_icon=tray_icon!=0;
            if(fields.size()>=7){if(!number(fields[6],debug)||debug>1)return 0;desktop.debug_overlay=debug!=0;}
            if(fields.size()==8){if(!number(fields[7],relative)||relative>1)return 0;desktop.relative_mouse=relative!=0;}
            return apply_preferences(fields[1],desktop);
        }
        if(fields[0]==L"record"&&fields.size()==2){
            if(fields[1]==L"1"){UnregisterHotKey(owner,hotkey_id);hotkey_registered=false;recording_hotkey=true;publish_input();return 1;}
            if(fields[1]==L"0"){recording_hotkey=false;hotkey_registered=RegisterHotKey(owner,hotkey_id,config.desktop.hotkey_modifiers|MOD_NOREPEAT,config.desktop.hotkey_key)!=FALSE;publish_input();return hotkey_registered?1:0;}
        }
        return 0;
    }
    bool apply_preferences(const std::wstring& name,const DesktopPreferences& desktop){
        if(name.empty()||name.size()>128||utf8(name).size()>256||name.find_first_not_of(L" \t\r\n")==std::wstring::npos||!valid_hive_hotkey(desktop.hotkey_modifiers,desktop.hotkey_key))return false;
        const bool hotkey_changed=!hotkey_registered||config.desktop.hotkey_modifiers!=desktop.hotkey_modifiers||config.desktop.hotkey_key!=desktop.hotkey_key;
        const int next_id=hotkey_id==81?82:81;
        if(hotkey_changed&&!RegisterHotKey(owner,next_id,desktop.hotkey_modifiers|MOD_NOREPEAT,desktop.hotkey_key))return false;
        if(callbacks.save_preferences&&!callbacks.save_preferences(name,desktop)){if(hotkey_changed)UnregisterHotKey(owner,next_id);return false;}
        if(hotkey_changed){UnregisterHotKey(owner,hotkey_id);hotkey_id=next_id;hotkey_registered=true;}
        config.device_name=name;config.desktop=desktop;set_relative(desktop.relative_mouse);publish_input();tray();if(health_badge){ShowWindow(health_badge,SW_HIDE);InvalidateRect(health_badge,nullptr,FALSE);}InvalidateRect(panel,nullptr,FALSE);return true;
    }
    void settings_ui() {
        if(!settings_restart.open())return;
        local(true);if(launch_settings())return;
        if(settings&&IsWindow(settings)){ShowWindow(settings,SW_SHOWNORMAL);SetForegroundWindow(settings);return;}
        int w=px(530),h=px(450);
        settings=CreateWindowExW(WS_EX_APPWINDOW,L"HiveDesk.Settings",L"HiveDesk settings",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,(GetSystemMetrics(SM_CXSCREEN)-w)/2,(GetSystemMetrics(SM_CYSCREEN)-h)/2,w,h,nullptr,nullptr,instance,this);
        if(!settings){settings_restart.close();return;}
        child(L"STATIC",L"Computer name",0,0,24,24,450,22);
        child(L"EDIT",config.device_name.c_str(),ES_AUTOHSCROLL|WS_TABSTOP,Name,24,52,450,30);SendDlgItemMessageW(settings,Name,EM_SETLIMITTEXT,128,0);
        HWND startup=child(L"BUTTON",L"Start when I sign in",BS_AUTOCHECKBOX|WS_TABSTOP,Startup,24,108,450,28);SendMessageW(startup,BM_SETCHECK,config.desktop.startup?BST_CHECKED:BST_UNCHECKED,0);
        HWND icon=child(L"BUTTON",L"Show tray icon",BS_AUTOCHECKBOX|WS_TABSTOP,TrayIcon,24,148,450,28);SendMessageW(icon,BM_SETCHECK,config.desktop.tray_icon?BST_CHECKED:BST_UNCHECKED,0);
        child(L"STATIC",L"Without the icon, open HiveDesk again for Settings.",0,0,24,185,450,24);
        child(L"STATIC",L"Show Hive",0,0,24,226,450,22);
        HWND hotkey=child(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,Hotkey,24,254,450,130);
        for(const auto* label:{L"Keep current shortcut",L"Ctrl + Alt + H",L"Ctrl + Shift + H",L"Win + Alt + H"})SendMessageW(hotkey,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendMessageW(hotkey,CB_SETCURSEL,0,0);
        HWND debug=child(L"BUTTON",L"Debug overlay",BS_AUTOCHECKBOX|WS_TABSTOP,DebugOverlay,24,302,200,26);SendMessageW(debug,BM_SETCHECK,config.desktop.debug_overlay?BST_CHECKED:BST_UNCHECKED,0);
        HWND relative=child(L"BUTTON",L"Relative mouse",BS_AUTOCHECKBOX|WS_TABSTOP,RelativeMouse,244,302,230,26);SendMessageW(relative,BM_SETCHECK,config.desktop.relative_mouse?BST_CHECKED:BST_UNCHECKED,0);EnableWindow(relative,raw_mouse_available);
        child(L"BUTTON",L"Save",BS_PUSHBUTTON|WS_TABSTOP,Save,24,350,120,32);
        child(L"STATIC",wide(HIVEDESK_VERSION).c_str(),0,0,164,355,185,24);
        child(L"BUTTON",L"Close",BS_PUSHBUTTON|WS_TABSTOP,Close,354,350,120,32);
        ShowWindow(settings,SW_SHOW);SetForegroundWindow(settings);SetFocus(GetDlgItem(settings,Name));
    }
    void save_settings() {
        auto desktop=config.desktop;desktop.startup=SendDlgItemMessageW(settings,Startup,BM_GETCHECK,0,0)==BST_CHECKED;desktop.tray_icon=SendDlgItemMessageW(settings,TrayIcon,BM_GETCHECK,0,0)==BST_CHECKED;
        desktop.debug_overlay=SendDlgItemMessageW(settings,DebugOverlay,BM_GETCHECK,0,0)==BST_CHECKED;
        desktop.relative_mouse=SendDlgItemMessageW(settings,RelativeMouse,BM_GETCHECK,0,0)==BST_CHECKED;
        const auto selected=SendDlgItemMessageW(settings,Hotkey,CB_GETCURSEL,0,0);
        if(selected>=1&&selected<=3){desktop.hotkey_key='H';desktop.hotkey_modifiers=selected==1?3:selected==2?6:9;}
        if(!apply_preferences(control_text(GetDlgItem(settings,Name)),desktop))MessageBoxW(settings,L"Could not save. Check the name and choose an available shortcut.",L"HiveDesk",MB_OK|MB_ICONINFORMATION);
    }
    static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        Impl* s=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){s=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(window,message,w,l);
        if(window==s->health_badge) {
            if(message==WM_NCHITTEST)return HTTRANSPARENT;
            if(message==WM_ERASEBKGND)return 1;
            if(message==WM_PAINT){PAINTSTRUCT ps{};BeginPaint(window,&ps);EndPaint(window,&ps);s->paint_health();return 0;}
        }

        if(window==s->owner||(!s->owner&&message==WM_NCCREATE)) {
            if(message==s->taskbar_created&&s->taskbar_created){s->tray_present=false;s->tray();return 0;}
            switch(message) {
            case WM_HOTKEY:
                if(int(w)==s->hotkey_id){if(s->panel_mode)s->hide_panel(true);else{s->show_panel(2);s->panel_summoned=s->panel_mode!=0;}s->publish_input();}return 0;
            case CaptureActionMessage:{
                const auto action=CaptureAction(w);
                if(action==CaptureAction::KickController){if(!s->shutting_down&&s->callbacks.kick_controller)s->callbacks.kick_controller();return 0;}
                if(s->shutting_down||uint64_t(l)!=s->generation.load())return 0;
                switch(action){
                case CaptureAction::ReturnLocal:s->local(true);break;
                case CaptureAction::Failed:s->local(true);s->status=L"Input channel stalled. Returned to this computer.";break;
                case CaptureAction::HidePanel:s->hide_panel();break;
                case CaptureAction::HidePanelAnimated:s->hide_panel(true);break;
                case CaptureAction::Summon:if(s->panel_mode)s->hide_panel(true);else{s->show_panel(2);s->panel_summoned=s->panel_mode!=0;}s->publish_input();break;
                default:break;
                }return 0;
            }
            case KickControllerMessage:if(!s->shutting_down&&s->callbacks.kick_controller)s->callbacks.kick_controller();return 0;
            case WM_COPYDATA:return s->settings_command(w,reinterpret_cast<const COPYDATASTRUCT*>(l));
            case WM_APP+44:return s->relative_mode?1:0;
            case WM_APP+45:s->settings_ui();return 0;
            case WM_APP+46:return s->hotkey_registered?1:0;
            case WM_APP+49:return s->raw_mouse_available?1:0;
            case UiTask:{std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(l));if(task&&!s->shutting_down)(*task)();return 0;}
            case ClipboardApply:{
                std::shared_ptr<ClipboardJob> job;{std::lock_guard lock(s->jobs_mutex);auto it=s->jobs.find(static_cast<uint64_t>(w));if(it!=s->jobs.end())job=it->second;}
                if(job){std::lock_guard lock(job->mutex);try{s->write_clipboard(job->value);job->done=true;}catch(const std::exception& e){job->error=e.what();}return job->done?1:0;}return 0;
            }
            case WM_TIMER:if(w==1)s->timer();else if(w==2&&s->panel_closing)s->hide_panel();else if(w==3){KillTimer(window,3);s->show_panel(2);s->panel_summoned=s->panel_mode!=0;s->publish_input();}return 0;
            case WM_DWMCOLORIZATIONCOLORCHANGED:case WM_THEMECHANGED:case WM_SETTINGCHANGE:s->system_color();return 0;
            case WM_CLIPBOARDUPDATE:s->read_clipboard(true);return 0;
            case TrayMessage:
                if(LOWORD(l)==WM_CONTEXTMENU||LOWORD(l)==WM_RBUTTONUP)s->tray_menu();
                else if(LOWORD(l)==WM_LBUTTONDBLCLK||LOWORD(l)==NIN_KEYSELECT){KillTimer(s->owner,3);s->settings_ui();}
                else if(LOWORD(l)==NIN_SELECT){SetTimer(s->owner,3,GetDoubleClickTime(),nullptr);}return 0;
            case WM_CLOSE:s->local(true);s->shutting_down=true;s->tray(true);PostQuitMessage(0);return 0;
            }
        }
        if(window==s->panel) {
            switch(message) {
            case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
            case WM_ERASEBKGND:return 1;
            case WM_PAINT:s->paint_panel();return 0;
            case WM_MOUSEMOVE:{POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};int h=s->card_at(p);if(h!=s->hovered){s->hovered=h;InvalidateRect(window,nullptr,FALSE);}return 0;}
            case WM_LBUTTONUP:{
                POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
                int index=s->card_at(p);if(index==0)s->select({});else if(index>0&&index<=static_cast<int>(s->peers.size()))s->select(s->peers[index-1].id);return 0;
            }
            case WM_RBUTTONUP:s->tray_menu();return 0;
            case WM_MOUSEWHEEL:{int visible=s->visible_tiles();s->scroll=std::clamp(s->scroll-GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA,0,std::max(0,static_cast<int>(s->peers.size())+1-visible));InvalidateRect(window,nullptr,FALSE);return 0;}
            }
        }
        if(window==s->settings) {
            switch(message) {
            case WM_CLOSE:DestroyWindow(window);return 0;
            case WM_DESTROY:s->settings=nullptr;s->settings_restart.close();s->publish_input();return 0;
            case WM_ERASEBKGND:{RECT r{};GetClientRect(window,&r);FillRect(reinterpret_cast<HDC>(w),&r,s->background_brush);return 1;}
            case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:SetTextColor(reinterpret_cast<HDC>(w),Text);SetBkColor(reinterpret_cast<HDC>(w),Background);return reinterpret_cast<LRESULT>(s->background_brush);
            case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(w),Text);SetBkColor(reinterpret_cast<HDC>(w),Card);return reinterpret_cast<LRESULT>(s->edit_brush);
            case WM_COMMAND:
                switch(LOWORD(w)) {
                case Save:s->save_settings();return 0;
                case Close:DestroyWindow(window);return 0;
                }break;
            }
        }
        if(window==s->viewer) {
            switch(message){
            case WM_SETCURSOR:if(s->remote){s->apply_remote_cursor();return TRUE;}break;
            case WM_CLOSE:s->local(true);return 0;case WM_ERASEBKGND:return 1;case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(window,&ps);EndPaint(window,&ps);return 0;}}
        }
        return DefWindowProcW(window,message,w,l);
    }
    bool create(HINSTANCE app,const ShellConfig& initial,ShellCallbacks cb,std::string& error) {
        instance=app;app_icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_SHARED));if(!app_icon)app_icon=LoadIconW(nullptr,IDI_APPLICATION);
        app_small_icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));if(!app_small_icon)app_small_icon=app_icon;
        config=initial;callbacks=std::move(cb);ui_thread=GetCurrentThreadId();
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);dpi=static_cast<int>(GetDpiForSystem());
        background_brush=CreateSolidBrush(Background);edit_brush=CreateSolidBrush(Card);
        font=CreateFontW(-px(16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        debug_font=CreateFontW(-px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Consolas");
        small_font=CreateFontW(-px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        title_font=CreateFontW(-px(24),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for(const wchar_t* name:{L"HiveDesk.Owner",L"HiveDesk.Viewer",L"HiveDesk.Switcher",L"HiveDesk.Settings",L"HiveDesk.Health"}) {
            WNDCLASSEXW c{};c.cbSize=sizeof(c);c.lpfnWndProc=window_proc;c.hInstance=instance;c.hCursor=LoadCursorW(nullptr,IDC_ARROW);c.hbrBackground=background_brush;c.lpszClassName=name;c.hIcon=app_icon;c.hIconSm=app_small_icon;
            if(!RegisterClassExW(&c)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){error="Could not register native UI classes.";return false;}
        }
        owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"HiveDesk.Owner",L"HiveDesk",WS_OVERLAPPED,0,0,0,0,nullptr,nullptr,instance,this);
        // Consume a launcher's STARTF_USESHOWWINDOW override on the invisible,
        // zero-size tool window, so the first real settings/viewer show is honored.
        if(owner){ShowWindow(owner,SW_SHOWNOACTIVATE);ShowWindow(owner,SW_HIDE);}
        viewer=CreateWindowExW(WS_EX_APPWINDOW,L"HiveDesk.Viewer",L"HiveDesk",WS_POPUP,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),nullptr,nullptr,instance,this);
        panel=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,L"HiveDesk.Switcher",L"HiveDesk computers",WS_POPUP,0,0,px(200),px(123),nullptr,nullptr,instance,this);
        health_badge=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,L"HiveDesk.Health",L"Stream status",WS_POPUP,0,0,px(136),px(30),viewer,nullptr,instance,this);

        if(!owner||!viewer||!panel){error="Could not create native desktop windows.";return false;}
        try{composition.create(panel);}catch(const std::exception& e){error=e.what();return false;}system_color();
        // GetAsyncKeyState cannot distinguish remote-injected modifiers. Start
        // physical shortcut state empty and populate it from non-injected hooks.
        publish_input();
        if(!input_capture.start(instance,owner,CaptureActionMessage,[this](const std::string& peer,uint64_t session,const InputEvent& event,uint64_t captured_us){
            if(callbacks.captured_input)callbacks.captured_input(peer,session,event,captured_us);
            else if(callbacks.input)callbacks.input(event);
        },error))return false;
        raw_mouse_available=input_capture.raw_mouse_available();
        relative_mode=config.desktop.relative_mouse&&raw_mouse_available;publish_input();
        if(!AddClipboardFormatListener(owner)){error="Could not listen for clipboard text changes.";return false;}
        hotkey_registered=RegisterHotKey(owner,hotkey_id,config.desktop.hotkey_modifiers|MOD_NOREPEAT,config.desktop.hotkey_key)!=FALSE;
        if(!hotkey_registered)log_hotkey_conflict();
        taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");tray();SetTimer(owner,1,16,nullptr);read_clipboard(false);
        worker=std::thread([this]{for(;;){std::function<void()> f;{std::unique_lock lock(work_mutex);work_ready.wait(lock,[this]{return stop_worker||!work.empty();});if(stop_worker)break;f=std::move(work.front());work.pop_front();}
            try{f();}catch(const std::exception& e){std::wstring message=L"Operation failed: "+wide(e.what());dispatch([this,message]{status=message;InvalidateRect(panel,nullptr,FALSE);});}
            catch(...){dispatch([this]{status=L"Operation failed";InvalidateRect(panel,nullptr,FALSE);});}
        }});return true;
    }
    ~Impl() {
        shutting_down=true;++generation;publish_input();input_capture.stop();ReleaseCapture();ClipCursor(nullptr);
        {std::lock_guard lock(work_mutex);stop_worker=true;work.clear();}work_ready.notify_one();if(worker.joinable())worker.join();
        if(owner){UnregisterHotKey(owner,hotkey_id);KillTimer(owner,1);KillTimer(owner,3);RemoveClipboardFormatListener(owner);tray(true);}
        if(settings_process){if(auto window=external_settings_window())PostMessageW(window,WM_CLOSE,0,0);CloseHandle(settings_process);settings_process=nullptr;settings=nullptr;}
        if(settings)DestroyWindow(settings);if(panel)DestroyWindow(panel);if(health_badge)DestroyWindow(health_badge);if(viewer)DestroyWindow(viewer);
        if(owner){MSG m{};while(PeekMessageW(&m,owner,UiTask,UiTask,PM_REMOVE))delete reinterpret_cast<std::function<void()>*>(m.lParam);DestroyWindow(owner);}
        for(auto& item:thumbnails)DeleteObject(item.second);thumbnails.clear();
        for(auto& item:prior_thumbnails)DeleteObject(item.second);prior_thumbnails.clear();
        reset_remote_cursor();
        if(debug_font)DeleteObject(debug_font);
        if(font)DeleteObject(font);if(small_font)DeleteObject(small_font);if(title_font)DeleteObject(title_font);if(background_brush)DeleteObject(background_brush);if(edit_brush)DeleteObject(edit_brush);
    }
};
Shell::Shell():impl_(new Impl){}
Shell::~Shell(){delete impl_;}
bool Shell::create(HINSTANCE instance,const ShellConfig& config,ShellCallbacks callbacks,std::string& error){return impl_->create(instance,config,std::move(callbacks),error);}
int Shell::run(){MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){if(impl_->settings&&!impl_->settings_process&&IsDialogMessageW(impl_->settings,&m))continue;TranslateMessage(&m);DispatchMessageW(&m);}return static_cast<int>(m.wParam);}
HWND Shell::viewer_window()const{return impl_->viewer;}
void Shell::update_peers(const std::vector<Peer>& peers){impl_->dispatch([s=impl_,peers]{s->peers=peers;s->scroll=std::min(s->scroll,static_cast<int>(peers.size()));InvalidateRect(s->panel,nullptr,FALSE);});}
void Shell::show_remote(const std::string& peer,const std::wstring& label,uint64_t session){uint64_t g=impl_->generation.load();impl_->dispatch([s=impl_,peer,label,session,g]{if(g==s->generation.load())s->show_remote_ui(peer,label,session);});}
void Shell::show_reconnecting(const std::string& peer){const auto g=impl_->generation.load();impl_->dispatch([s=impl_,peer,g]{
    if(g!=s->generation.load()||!s->selection_armed||s->pending_id!=peer||s->reconnecting)return;
    // Keep the selection armed so a first frame can restore immersion, while
    // local()/Escape still invalidates every delayed UI completion.
    s->show_remote_ui(peer,L"Reconnecting",0);s->reconnecting=true;s->publish_input();
    s->reset_remote_cursor();InvalidateRect(s->health_badge,nullptr,FALSE);s->timer();
});}
void Shell::set_remote_size(uint32_t width,uint32_t height){uint64_t g=impl_->generation.load();impl_->dispatch([s=impl_,width,height,g]{if(g==s->generation.load()){s->remote_width=width;s->remote_height=height;s->publish_input();}});}
void Shell::show_local(){impl_->dispatch([s=impl_]{s->local(false);});}
void Shell::quit(){impl_->dispatch([s=impl_]{PostMessageW(s->owner,WM_CLOSE,0,0);});}
void Shell::set_status(const std::wstring& status){impl_->dispatch([s=impl_,status]{s->status=status;InvalidateRect(s->panel,nullptr,FALSE);});}
void Shell::show_error(const std::wstring& message){impl_->dispatch([s=impl_,message]{s->local(true);s->status=message;MessageBoxW(s->owner,message.c_str(),L"HiveDesk - Could not open desktop",MB_OK|MB_ICONERROR);});}
void Shell::apply_clipboard(const std::wstring& value){impl_->apply_sync(value);}
std::wstring Shell::cached_clipboard()const{std::lock_guard lock(impl_->cache_mutex);return impl_->clipboard_cache;}
void Shell::show_settings(){impl_->dispatch([s=impl_]{s->settings_ui();});}
bool Shell::prepare_video_restart(){return impl_->settings_restart.prepare();}
void Shell::cancel_video_restart(){impl_->settings_restart.cancel();}
void Shell::update_thumbnail(const std::string& peer,Thumbnail thumbnail) {
    if(thumbnail.bgra.size()!=size_t(SnapshotWidth)*SnapshotHeight*4)return;
    impl_->dispatch([s=impl_,peer,thumbnail=std::move(thumbnail)] {
        if(std::none_of(s->peers.begin(),s->peers.end(),[&](const Peer& p){return p.id==peer;}))return;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=SnapshotWidth;info.bmiHeader.biHeight=-LONG(SnapshotHeight);
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits=nullptr;
        HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);if(!bitmap)return;
        std::memcpy(bits,thumbnail.bgra.data(),thumbnail.bgra.size());
        auto prior=s->prior_thumbnails.find(peer);if(prior!=s->prior_thumbnails.end()){DeleteObject(prior->second);s->prior_thumbnails.erase(prior);}
        auto& cached=s->thumbnails[peer];if(cached)s->prior_thumbnails[peer]=cached;cached=bitmap;
        BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        s->thumbnail_fades[peer]=GetTickCount64()-(animations?0:180);InvalidateRect(s->panel,nullptr,FALSE);
    });
}
void Shell::observe_cursor(const std::string& controller) {
    impl_->dispatch([s=impl_,controller] {
        s->cursor_controller=controller;s->cursor_sent=false;s->sent_cursor=nullptr;s->sent_pointer_revision=0;
        ++s->pointer_revision;GetCursorPos(&s->physical_pointer);s->sample_cursor();
    });
}
void Shell::update_health(const std::string& peer,const StreamHealth& health) {
    const auto generation=impl_->generation.load();
    impl_->dispatch([s=impl_,peer,health,generation] {
        if(generation!=s->generation.load()||(peer!=s->active_id&&peer!=s->pending_id))return;
        const bool changed=s->stream_health.pressure!=health.pressure;s->stream_health=health;s->health_time=GetTickCount64();
        if((changed||s->config.desktop.debug_overlay)&&s->health_badge)InvalidateRect(s->health_badge,nullptr,FALSE);
    });
}
bool Shell::debug_enabled()const{return impl_->config.desktop.debug_overlay;}
void Shell::update_source_diagnostics(const std::string& peer,const SourceDiagnostics& value){
    const auto generation=impl_->generation.load();const auto received=GetTickCount64();
    std::lock_guard lock(impl_->source_mutex);impl_->pending_source=Impl::PendingSource{peer,value,generation,received};
}
void Shell::update_viewer_diagnostics(const std::string& peer,const ViewerDiagnostics& value){
    const auto generation=impl_->generation.load();impl_->dispatch([s=impl_,peer,value,generation]{if(generation!=s->generation.load()||peer!=s->active_id)return;s->viewer_diagnostics=value;if(s->config.desktop.debug_overlay)InvalidateRect(s->health_badge,nullptr,FALSE);});
}
void Shell::update_input_diagnostics(const std::string& peer,const InputDiagnostics& value){
    const auto generation=impl_->generation.load();impl_->dispatch([s=impl_,peer,value,generation]{if(generation!=s->generation.load()||peer!=s->active_id||value.session!=s->input_session)return;s->input_diagnostics=value;if(s->config.desktop.debug_overlay)InvalidateRect(s->health_badge,nullptr,FALSE);});
}
void Shell::update_cursor(const std::string& peer,CursorState cursor) {
    impl_->dispatch([s=impl_,peer,cursor=std::move(cursor)] {
        if(peer!=s->pending_id&&peer!=s->active_id)return;
        HCURSOR next=nullptr;
        if(cursor.width) {
            next=make_cursor(cursor);if(!next)return;
        }
        s->remote_cursor_visible=cursor.visible;s->remote_cursor_received=true;s->remote_pointer=cursor;
        if(next) {
            HCURSOR old=s->remote_cursor;s->remote_cursor=next;
            if(old){if(GetCursor()==old)SetCursor(s->selected_remote_cursor());DestroyCursor(old);}
        }
        s->apply_remote_position();s->apply_remote_cursor();
    });
}
void Shell::update_cursor_composition(const std::string& peer,uint64_t session,uint64_t sequence,CursorComposition mode) {
    const auto generation=impl_->generation.load();
    impl_->dispatch([s=impl_,peer,session,sequence,mode,generation] {
        if(generation!=s->generation.load()||!s->remote||s->reconnecting||peer!=s->active_id||session!=s->input_session)return;
        if(s->remote_cursor_composition.present(session,sequence,mode))s->apply_remote_cursor();
    });
}
}
