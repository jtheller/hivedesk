#include "hivedesk/shell.h"
#include "hivedesk/config.h"
#include "hivedesk/media.h"
#include "panel_composition.h"
#include "taskbar_peek.h"
#include "viewer_geometry.h"
#include "debug_model.h"
#include <optional>
#include "hivedesk/network_key.h"
#include "hivedesk/desktop_aux.h"
#include "hivedesk/cursor_composition.h"
#include "cursor_position.h"
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
constexpr UINT UiTask=WM_APP+41, TrayMessage=WM_APP+42, KickControllerMessage=WM_APP+47, CaptureActionMessage=WM_APP+48, ViewerLayoutMessage=WM_APP+50;
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
    HANDLE settings_process{};HWND health_badge{},waiting{};
    bool fallback_settings_ready=false;
    std::atomic<std::shared_ptr<const ViewerSurface>> viewer_surface;
    std::vector<std::shared_ptr<const ViewerSurface>> retired_viewers;
    std::map<std::string,std::shared_ptr<const ViewerSurface>> warm_surfaces;
    std::vector<std::string> warm_visible;
    void publish_visible() {
        if(!callbacks.visible_peers)return;
        std::vector<std::string> ids;
        if(panel_mode&&!panel_closing){
            const int first=panel_mode==1?preview_index():scroll;
            const int last=panel_mode==1?first+1:std::min(int(peers.size())+1,scroll+visible_tiles());
            for(int i=first;i<last;++i)if(i>0&&i<=int(peers.size())){
                const auto& p=peers[i-1];
                if(p.authenticated&&(p.features&FeatureWarmStream)&&p.id!=active_id)ids.push_back(p.id);
            }
        }
        if(ids==warm_visible)return;
        warm_visible=ids;
        std::vector<std::shared_ptr<const ViewerSurface>> surfaces;
        for(const auto& id:ids){
            auto& surface=warm_surfaces[id];
            if(!surface){
                const auto window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,L"HiveDesk.Viewer",L"HiveDesk",WS_POPUP,
                    0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),nullptr,nullptr,instance,this);
                if(window){SetPropW(window,L"HiveDesk.PeekSurface",reinterpret_cast<HANDLE>(1));surface=std::make_shared<const ViewerSurface>(ViewerSurface{window,id});}
            }
            if(surface)surfaces.push_back(surface);
        }
        callbacks.visible_peers(surfaces);
    }
    std::string peek_id;uint64_t peek_epoch=0;bool peek_live=false;
    std::shared_ptr<const ViewerSurface> peek_surface;
    std::shared_ptr<MediaViewer> peek_media;
    struct FadingPeek {std::shared_ptr<const ViewerSurface> surface;std::shared_ptr<MediaViewer> media;ULONGLONG until;};
    std::vector<FadingPeek> fading_peeks;
    void end_peek(bool animate=true,HWND adopt=nullptr){
        if(peek_surface){
            if(peek_surface->window!=adopt){
                try{peek_media->peek_opacity(false,animate);}catch(...){}
                if(animate)fading_peeks.push_back({peek_surface,peek_media,GetTickCount64()+200});
                else ShowWindow(peek_surface->window,SW_HIDE);
            }
            peek_surface.reset();peek_media.reset();
        }
        peek_live=false;RemovePropW(panel,L"HiveDesk.PeekReady");
        if(!peek_id.empty()){peek_id.clear();++peek_epoch;if(callbacks.peek_peer)callbacks.peek_peer({},peek_epoch);}
        if(!animate){for(const auto& old:fading_peeks)if(old.surface->window!=adopt)ShowWindow(old.surface->window,SW_HIDE);fading_peeks.clear();}
        publish_input();
    }
    void hover_peek(int index){
        if(pressed_card>=0)return;
        std::string id;
        if(panel_mode&&!panel_closing&&pressed_card<0&&!taskbar_peek.active()&&index>0&&index<=int(peers.size())){
            const auto& p=peers[index-1];if(p.id!=active_id&&p.authenticated&&(p.features&FeatureLivePeek))id=p.id;
        }
        if(id==peek_id)return;
        end_peek();peek_id=id;
        if(!id.empty()){++peek_epoch;if(callbacks.peek_peer)callbacks.peek_peer(id,peek_epoch);}
        publish_input();
    }
    void show_peek_ui(std::shared_ptr<const ViewerSurface> surface,std::shared_ptr<MediaViewer> media,uint64_t epoch,bool ready){
        if(shutting_down||!panel_mode||panel_closing||epoch!=peek_epoch||surface->peer!=peek_id||surface->window==viewer||dragging||taskbar_peek.active())return;
        std::erase_if(fading_peeks,[&](const auto& p){return p.surface==surface;});
        peek_surface=surface;peek_media=media;
        SetWindowPos(surface->window,HWND_TOPMOST,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),SWP_NOACTIVATE|SWP_SHOWWINDOW);
        SetWindowPos(panel,HWND_TOPMOST,0,0,0,0,SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE);
        if(ready&&!peek_live){try{media->peek_opacity(true);}catch(...){end_peek(false);return;}peek_live=true;}
        SetPropW(panel,L"HiveDesk.PeekReady",reinterpret_cast<HANDLE>(ULONG_PTR(peek_live)));publish_input();
    }
    void tick_peek(){
        std::erase_if(fading_peeks,[&](const auto& old){if(GetTickCount64()<old.until)return false;if((!peek_surface||peek_surface!=old.surface)&&old.surface->window!=viewer)ShowWindow(old.surface->window,SW_HIDE);return true;});
        if(peek_surface&&peek_media->stats().starts_with("Media error:"))end_peek(false);
    }
    HWND transition_viewer{},transition_waiting{};
    PanelComposition transition_composition;
    void finish_transition(){
        if(transition_viewer&&transition_viewer!=viewer)ShowWindow(transition_viewer,SW_HIDE);
        if(transition_waiting){ShowWindow(transition_waiting,SW_HIDE);DestroyWindow(transition_waiting);}
        transition_viewer=transition_waiting=nullptr;transition_composition=PanelComposition{};
    }
    PanelComposition waiting_composition;
    bool waiting_visible=false,waiting_fading=false,waiting_retry=false;
    static constexpr UINT ConnectionHintDelayMs=650;
    ULONGLONG connection_hint_due=0;bool connection_hint_visible=false;
    void cancel_connection_hint(){KillTimer(owner,5);connection_hint_due=0;connection_hint_visible=false;}
    void reveal_connection_hint(){
        if(!reconnecting||!connection_hint_due){KillTimer(owner,5);return;}
        const auto now=GetTickCount64();
        // A queued timer from an earlier selection must not reveal the next one.
        if(now<connection_hint_due){SetTimer(owner,5,UINT(connection_hint_due-now),nullptr);return;}
        KillTimer(owner,5);connection_hint_visible=true;
        if(waiting_visible&&!waiting_fading){try{paint_waiting(false);}catch(const std::exception&){local(true);}}
    }
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
    bool windowed=false,sizing_viewer=false,dragging=false,viewer_layout_pending=false,viewer_maximized=false,viewer_restore_pending=false;HWND drag_focus{};
    int pressed_card=-1,drop_slot=-1;POINT drag_origin{};std::string dragged_peer;ULONGLONG last_drag_scroll=0;
    HWND drag_ghost{};POINT drag_anchor{};std::vector<uint32_t> panel_pixels;int pixels_width=0,pixels_height=0;
    double window_ratio=16.0/9;
    RECT viewer_client() const {RECT r{};GetClientRect(viewer,&r);MapWindowPoints(viewer,nullptr,reinterpret_cast<POINT*>(&r),2);return r;}
    void sort_peers(){
        const auto rank=[&](const std::string& id){return std::find(config.device_order.begin(),config.device_order.end(),id)-config.device_order.begin();};
        std::stable_sort(peers.begin(),peers.end(),[&](const Peer& a,const Peer& b){const auto ar=rank(a.id),br=rank(b.id);return ar==br?a.id<b.id:ar<br;});
    }
    void cancel_drag(bool restore_focus=true,bool commit_visual=true){
        if(drag_ghost){DestroyWindow(drag_ghost);drag_ghost=nullptr;}
        if(dragging)composition.finish_cards(commit_visual);
        const auto restore=drag_focus;drag_focus=nullptr;pressed_card=-1;dragging=false;drop_slot=-1;dragged_peer.clear();
        if(GetCapture()==panel)ReleaseCapture();
        if(restore_focus&&restore&&IsWindow(restore)&&GetForegroundWindow()==panel)SetForegroundWindow(restore);
        publish_input();InvalidateRect(panel,nullptr,FALSE);
    }
    std::vector<RECT> visible_cards() const {
        std::vector<RECT> cards;
        for(int i=scroll;i<=int(peers.size())&&i<scroll+visible_tiles();++i){const int y=px(12)+(i-scroll)*tile_stride();cards.push_back({px(12),y,panel_width-px(12),y+tile_height()});}
        return cards;
    }
    void drag_layers(){
        const auto shade=RGB(GetRValue(accent)*16/100,GetGValue(accent)*16/100,GetBValue(accent)*16/100);
        // The source may scroll out of view; keep drag mode without hiding an
        // unrelated visible card or starting the hotkey reveal timeline.
        composition.reveal_cards(visible_cards(),shade,pressed_card>=scroll?pressed_card-scroll:INT_MAX);
    }
    void begin_drag_visual(){
        paint_panel();const auto cards=visible_cards();const int index=pressed_card-scroll;
        if(index<0||index>=int(cards.size()))return;
        const RECT box=cards[index];const int w=box.right-box.left,h=box.bottom-box.top;
        if(panel_pixels.empty()||box.bottom>pixels_height)return;
        drag_ghost=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,
            L"HiveDesk.Drag",L"HiveDesk dragged computer",WS_POPUP,0,0,w,h,panel,nullptr,instance,this);
        if(!drag_ghost)return;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!dc||!bitmap){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);DestroyWindow(drag_ghost);drag_ghost=nullptr;return;}
        for(int y=0;y<h;++y)std::memcpy(static_cast<uint32_t*>(pixels)+y*w,panel_pixels.data()+size_t(y+box.top)*pixels_width+box.left,size_t(w)*4);
        const auto old=SelectObject(dc,bitmap);POINT source{},position=drag_origin;ClientToScreen(panel,&position);position.x-=drag_anchor.x;position.y-=drag_anchor.y;
        SIZE size{w,h};BLENDFUNCTION blend{AC_SRC_OVER,0,235,AC_SRC_ALPHA};
        UpdateLayeredWindow(drag_ghost,nullptr,&position,&size,dc,&source,0,&blend,ULW_ALPHA);
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ShowWindow(drag_ghost,SW_SHOWNOACTIVATE);drag_layers();
    }
    void move_drag_visual(POINT point){
        if(drag_ghost){POINT screen=point;ClientToScreen(panel,&screen);SetWindowPos(drag_ghost,HWND_TOPMOST,screen.x-drag_anchor.x,screen.y-drag_anchor.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);}
        composition.drag_slot(pressed_card,drop_slot<0?-1:drop_slot+1,scroll,tile_stride());
    }
    void reorder_peer(const std::string& id,int slot){
        auto next=peers;const auto it=std::find_if(next.begin(),next.end(),[&](const Peer& p){return p.id==id;});if(it==next.end())return;
        const auto from=int(it-next.begin());auto moved=*it;next.erase(it);if(slot>from)--slot;slot=std::clamp(slot,0,int(next.size()));next.insert(next.begin()+slot,std::move(moved));
        auto order=config.device_order;
        // Keep offline devices in their saved slots; replace the online subsequence.
        for(const auto& peer:peers)if(std::find(order.begin(),order.end(),peer.id)==order.end())order.push_back(peer.id);
        size_t i=0;for(auto& key:order)if(std::any_of(peers.begin(),peers.end(),[&](const Peer& p){return p.id==key;}))key=next[i++].id;
        if(callbacks.save_order&&!callbacks.save_order(order)){status=L"Could not save computer order.";return;}
        config.device_order=std::move(order);peers=std::move(next);InvalidateRect(panel,nullptr,FALSE);
    }
    void size_to_source(){
        if(!windowed)return;
        if(remote_width&&remote_height)window_ratio=double(remote_width)/remote_height;
        // Native maximize owns the work-area bounds. The renderer letterboxes
        // the source; keep the new ratio for the later restored window.
        if(IsZoomed(viewer)||IsIconic(viewer))return;
        RECT r{};GetWindowRect(viewer,&r);
        RECT frame{};AdjustWindowRectExForDpi(&frame,DWORD(GetWindowLongW(viewer,GWL_STYLE)),FALSE,DWORD(GetWindowLongW(viewer,GWL_EXSTYLE)),GetDpiForWindow(viewer));
        constrain_viewer(r,WMSZ_BOTTOMRIGHT,frame.right-frame.left,frame.bottom-frame.top,window_ratio,px(240));
        RECT work_area{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work_area,0);
        if(r.bottom-r.top>work_area.bottom-work_area.top){r.bottom=r.top+work_area.bottom-work_area.top;constrain_viewer(r,WMSZ_BOTTOM,frame.right-frame.left,frame.bottom-frame.top,window_ratio,1);}
        const LONG w=r.right-r.left,h=r.bottom-r.top;r.left=std::clamp(r.left,work_area.left,std::max(work_area.left,work_area.right-w));r.top=std::clamp(r.top,work_area.top,std::max(work_area.top,work_area.bottom-h));
        SetWindowPos(viewer,nullptr,r.left,r.top,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void request_viewer_layout(){
        if(!viewer_layout_pending&&owner)viewer_layout_pending=PostMessageW(owner,ViewerLayoutMessage,0,0)!=FALSE;
    }
    void sync_viewer_ui(){
        viewer_layout_pending=false;
        if(!viewer)return;
        if(viewer_restore_pending){viewer_restore_pending=false;size_to_source();}
        if(waiting_visible&&!IsIconic(viewer)){
            const auto r=viewer_client();SetWindowPos(waiting,HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            const auto width=ULONG_PTR(r.right-r.left),height=ULONG_PTR(r.bottom-r.top);
            // Moving the HWND needs no GPU upload. Also preserve an in-progress
            // first-frame fade instead of restarting its opaque cover on move.
            if(!waiting_fading&&(ULONG_PTR(GetPropW(waiting,L"HiveDesk.SurfaceWidth"))!=width||ULONG_PTR(GetPropW(waiting,L"HiveDesk.SurfaceHeight"))!=height))
                try{paint_waiting(false);}catch(const std::exception&){local(true);}
        }
        else if(waiting_visible)ShowWindow(waiting,SW_HIDE);
        publish_input();
    }
    std::map<std::string,HBITMAP> thumbnails;
    std::map<std::string,HBITMAP> full_snapshots;
    std::map<std::string,HBITMAP> prior_thumbnails;
    std::map<std::string,ULONGLONG> thumbnail_fades;
    std::string cursor_controller;
    uint64_t cursor_host_session=0,physical_host_session=0,physical_host_input_us=0;
    CursorPositionGate cursor_position_gate;
    unsigned cursor_trace_count=0;
#ifdef HIVEDESK_SHELL_TEST
    std::function<void(const INPUT&)> cursor_position_test_sink;
#endif
    HCURSOR sent_cursor{},remote_cursor{};
    bool cursor_sent=false,sent_cursor_visible=false,remote_cursor_visible=true,remote_cursor_received=false;
    uint64_t pointer_revision=1,sent_pointer_revision=0;
    CursorState remote_pointer;
    PresentedCursorComposition remote_cursor_composition;
    POINT physical_pointer{};
    ULONGLONG last_local_motion=0;
    InputCapture input_capture;uint64_t input_session=0,last_capture_pointer_revision=0;
    NestedReturn nested_return;
    std::function<bool()> return_authorized;
    std::string return_parent_peer;uint64_t return_parent_session=0,return_parent_id=0;
    std::wstring status=L"Ready on your local network";
    bool remote=false,shutting_down=false,selection_armed=false,reconnecting=false;
    bool relative_mode=false,raw_mouse_available=false;
    PanelComposition composition;
    TaskbarPeek taskbar_peek;
    bool taskbar_focus_guard=false,taskbar_corner_armed=true;
    COLORREF accent=GetSysColor(COLOR_HIGHLIGHT);
    uint32_t remote_width=0,remote_height=0;
    int panel_mode=0,panel_width=200,panel_height=0,hovered=-1,scroll=0;
    bool panel_closing=false,panel_summoned=false,tray_present=false,recording_hotkey=false,hotkey_registered=false;
    int hotkey_id=81;
    ULONGLONG outside_since=0;
    ClipboardDesktop clipboard_desktop;
    std::atomic<uint64_t> generation{0};
    std::mutex work_mutex; std::condition_variable work_ready; std::deque<std::function<void()>> work; bool stop_worker=false; std::thread worker;

    int px(int v) const { return MulDiv(v,dpi,96); }
    void enqueue(std::function<void()> f) { { std::lock_guard lock(work_mutex);if(stop_worker)return;work.push_back(std::move(f)); }work_ready.notify_one(); }
    void dispatch(std::function<void()> f) {
        if(GetCurrentThreadId()==ui_thread) { f();return; }
        auto task=std::make_unique<std::function<void()>>(std::move(f));
        if(owner&&PostMessageW(owner,UiTask,0,reinterpret_cast<LPARAM>(task.get())))task.release();
    }
    InputCaptureState input_state(bool keep_taskbar_guard=false) {
        InputCaptureState state;state.generation=generation.load();state.session=input_session;state.peer=active_id;
        state.returning=nested_return.busy();
        if(!keep_taskbar_guard&&!taskbar_peek.active()&&GetForegroundWindow()==viewer)taskbar_focus_guard=false;
        state.local_shell=nested_return.busy()||taskbar_peek.active()||taskbar_focus_guard||!peek_id.empty()||!fading_peeks.empty();
        state.remote=remote&&!reconnecting&&!shutting_down&&!state.local_shell&&(!windowed||(GetForegroundWindow()==viewer&&!sizing_viewer&&!IsIconic(viewer)));
        state.windowed=windowed;state.viewer=viewer;state.viewer_rect=viewer_client();state.panel_dragging=pressed_card>=0;state.relative=relative_mode;state.recording_hotkey=recording_hotkey;
        state.panel=panel;state.panel_visible=panel&&(GetWindowLongPtrW(panel,GWL_STYLE)&WS_VISIBLE);state.panel_summoned=panel_summoned;
        state.settings_visible=settings&&IsWindowVisible(settings);
        if(state.panel_visible)GetWindowRect(panel,&state.panel_rect);if(state.settings_visible)GetWindowRect(settings,&state.settings_rect);
        state.screen_width=uint32_t(std::max(1,GetSystemMetrics(SM_CXSCREEN)));state.screen_height=uint32_t(std::max(1,GetSystemMetrics(SM_CYSCREEN)));
        state.remote_width=remote_width;state.remote_height=remote_height;
        state.hotkey_key=config.desktop.hotkey_key;state.hotkey_modifiers=config.desktop.hotkey_modifiers;return state;
    }
    void publish_input(bool keep_taskbar_guard=false) {input_capture.update(input_state(keep_taskbar_guard));}
    void sample_physical_pointer() {
        const auto pointer=input_capture.physical_pointer();
        if(pointer.revision!=last_capture_pointer_revision){last_capture_pointer_revision=pointer.revision;++pointer_revision;physical_pointer=pointer.point;last_local_motion=pointer.motion_tick;
            physical_host_session=pointer.host_session;physical_host_input_us=pointer.host_input_us;}
    }
    void hide_panel(bool animate=false,bool keep_taskbar=false) {
        end_peek(animate);
        // Popup dismissal is independent of the native taskbar interaction.
        // Restoring z-order here would steal the Start click still in flight.
        if(!keep_taskbar){if(animate)taskbar_peek.leave();else taskbar_peek.reset();}
        cancel_drag();panel_summoned=false;
        if(animate&&panel_mode&&!panel_closing){panel_closing=true;publish_visible();SetTimer(owner,2,composition.hide(),nullptr);publish_input();return;}
        if(animate&&panel_closing)return;
        panel_closing=false;KillTimer(owner,2);panel_mode=0;outside_since=0;hovered=-1;publish_visible();
        if(panel)ShowWindow(panel,SW_HIDE);
        confine_mouse();publish_input();
    }
    void collect_viewers() {
        std::erase_if(warm_surfaces,[&](const auto& entry){
            if(std::find(warm_visible.begin(),warm_visible.end(),entry.first)!=warm_visible.end()||
                (entry.second&&entry.second.use_count()!=1))return false;
            if(entry.second)DestroyWindow(entry.second->window);return true;
        });
        std::erase_if(retired_viewers,[](const auto& surface){
            if(surface.use_count()!=1)return false;
            DestroyWindow(surface->window);return true;
        });
    }
    bool replace_viewer(const std::string& peer,POINT drop,bool use_warm=true) {
        // A reused flip-model HWND can expose its last DWM image before the
        // asynchronous waiting-cover commit. Never show that HWND again for a
        // new selection, even if its render worker is still finishing a frame.
        const DWORD style=windowed?WS_OVERLAPPEDWINDOW:WS_POPUP;
        RECT bounds{0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
        if(windowed){
            RECT work_area{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work_area,0);
            int w=int((work_area.right-work_area.left)*.84),h=int(w/window_ratio);
            if(h>(work_area.bottom-work_area.top)*.84){h=int((work_area.bottom-work_area.top)*.84);w=int(h*window_ratio);}
            bounds={0,0,w,h};AdjustWindowRectExForDpi(&bounds,style,FALSE,WS_EX_APPWINDOW,UINT(dpi));
            w=bounds.right-bounds.left;h=bounds.bottom-bounds.top;
            bounds.left=std::clamp(drop.x-w/2,work_area.left,std::max(work_area.left,work_area.right-w));bounds.top=std::clamp(drop.y-px(24),work_area.top,std::max(work_area.top,work_area.bottom-h));bounds.right=bounds.left+w;bounds.bottom=bounds.top+h;
        }
        std::shared_ptr<const ViewerSurface> surface;
        if(auto warm=warm_surfaces.find(peer);use_warm&&warm!=warm_surfaces.end()){surface=warm->second;warm_surfaces.erase(warm);}
        const HWND next=surface?surface->window:CreateWindowExW(WS_EX_APPWINDOW,L"HiveDesk.Viewer",L"HiveDesk",style,
            bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,instance,this);
        if(surface){
            SetWindowLongPtrW(next,GWL_STYLE,style);
            auto ex=GetWindowLongPtrW(next,GWL_EXSTYLE);SetWindowLongPtrW(next,GWL_EXSTYLE,(ex&~(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE))|WS_EX_APPWINDOW);
            SetWindowPos(next,windowed?HWND_NOTOPMOST:HWND_TOPMOST,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                SWP_NOACTIVATE|SWP_FRAMECHANGED);
        }
        if(!next)return false;
        const HWND next_waiting=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,
            L"HiveDesk.Waiting",L"HiveDesk connection",WS_POPUP,0,0,GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN),next,nullptr,instance,this);
        if(!next_waiting){if(surface)retired_viewers.push_back(surface);else DestroyWindow(next);return false;}
        PanelComposition next_composition;
        try{
            next_composition.create(next_waiting);
            next_composition.cover(float(GetSystemMetrics(SM_CXSCREEN)),float(GetSystemMetrics(SM_CYSCREEN)),false,true);
        }catch(const std::exception&){DestroyWindow(next_waiting);if(surface)retired_viewers.push_back(surface);else DestroyWindow(next);return false;}
        if(!surface)surface=std::make_shared<const ViewerSurface>(ViewerSurface{next,peer});
        if(!IsWindowVisible(viewer)&&!remote&&GetForegroundWindow()!=panel)prior_focus=GetForegroundWindow();
        finish_transition();transition_viewer=viewer;
        // The waiting HWND also retains its previous snapshot/fade in DWM.
        // Give it a fresh target, rather than racing a reset commit against Show.
        KillTimer(owner,4);waiting_visible=waiting_fading=false;
        transition_waiting=waiting;transition_composition=std::move(waiting_composition);
        waiting=next_waiting;waiting_composition=std::move(next_composition);
        // Move the remaining owned popup before retiring its owner.
        SetWindowLongPtrW(health_badge,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(next));
        viewer=next;viewer_maximized=viewer_restore_pending=false;
        if(auto previous=viewer_surface.exchange(std::move(surface)))retired_viewers.push_back(std::move(previous));
        return true;
    }
    void hide_waiting() {
        finish_transition();
        KillTimer(owner,4);waiting_visible=waiting_fading=false;
        if(waiting)ShowWindow(waiting,SW_HIDE);
    }
    void paint_waiting(bool /*entering*/) {
        const auto client=viewer_client();const int width=std::max(1L,client.right-client.left),height=std::max(1L,client.bottom-client.top);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* pixels=nullptr;
        HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!dc||!bitmap){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);throw std::runtime_error("Could not create waiting screen");}
        const auto previous=SelectObject(dc,bitmap);fill(dc,{0,0,width,height},Background);
        const auto full=full_snapshots.find(pending_id),thumb=thumbnails.find(pending_id);
        const HBITMAP snapshot=full!=full_snapshots.end()?full->second:thumb!=thumbnails.end()?thumb->second:nullptr;
        RemovePropW(waiting,L"HiveDesk.SnapshotWidth");RemovePropW(waiting,L"HiveDesk.SnapshotHeight");
        if(snapshot){
            BITMAP dimensions{};GetObjectW(snapshot,sizeof(dimensions),&dimensions);
            SetPropW(waiting,L"HiveDesk.SnapshotWidth",reinterpret_cast<HANDLE>(ULONG_PTR(dimensions.bmWidth)));
            SetPropW(waiting,L"HiveDesk.SnapshotHeight",reinterpret_cast<HANDLE>(ULONG_PTR(dimensions.bmHeight)));
            HDC source=CreateCompatibleDC(dc);const auto prior=SelectObject(source,snapshot);
            const double scale=std::min(double(width)/dimensions.bmWidth,double(height)/dimensions.bmHeight);
            const int w=int(dimensions.bmWidth*scale),h=int(dimensions.bmHeight*scale);
            SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
            StretchBlt(dc,(width-w)/2,(height-h)/2,w,h,source,0,0,dimensions.bmWidth,dimensions.bmHeight,SRCCOPY);
            SelectObject(source,prior);DeleteDC(source);
        }
        auto peer=std::find_if(peers.begin(),peers.end(),[&](const Peer& p){return p.id==pending_id;});
        const auto label=peer==peers.end()?wide(pending_id):wide(peer->name);
        std::wstring message=waiting_retry?L"Reconnecting":L"Connecting";
        if(snapshot)message+=L"  \x00b7  Snapshot";
        message+=L"  \x00b7  Ctrl + Alt + ` to cancel";
        RemovePropW(waiting,L"HiveDesk.ConnectionHintVisible");
        if(connection_hint_visible){
        const int left=px(24),bottom=height-px(24);
        RECT badge{left,bottom-px(74),std::min(width-left,left+px(480)),bottom};fill(dc,badge,Background);
        fill(dc,{left,badge.top,left+px(3),bottom},accent);
        draw_text(dc,title_font,label,{left+px(16),badge.top+px(5),badge.right-px(12),badge.top+px(38)},Text);
        draw_text(dc,small_font,message,{left+px(16),badge.top+px(39),badge.right-px(12),bottom-px(5)},Muted);
        SetPropW(waiting,L"HiveDesk.ConnectionHintVisible",reinterpret_cast<HANDLE>(1));
        }
        GdiFlush();auto* rgba=static_cast<uint32_t*>(pixels);for(size_t i=0;i<size_t(width)*height;++i)rgba[i]|=0xff000000u;
        try{waiting_composition.present(pixels,width,height,false);waiting_composition.cover(float(width),float(height),false);}
        catch(...){SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);throw;}
        SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);
        SetWindowTextW(waiting,(L"HiveDesk - "+message+L" - "+label).c_str());
    }
    void begin_waiting(const std::string& id,bool retry,bool adopting=false) {
        KillTimer(owner,4);waiting_fading=false;waiting_retry=retry;
        cancel_connection_hint();connection_hint_due=GetTickCount64()+ConnectionHintDelayMs;
        SetTimer(owner,5,ConnectionHintDelayMs,nullptr);
        if(active_id!=id)previous_id=active_id;
        if(!IsWindowVisible(viewer)&&!prior_focus)prior_focus=GetForegroundWindow();
        active_id=id;remote=true;reconnecting=true;input_session=0;publish_input();
        reset_remote_cursor();
        if(adopting){hide_waiting();SetWindowPos(viewer,windowed?HWND_NOTOPMOST:HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);SetForegroundWindow(viewer);return;}
        try{
            paint_waiting(false);
            // Submit the complete opaque snapshot before exposing either HWND.
            // Commit() alone is asynchronous and can expose a black new surface.
            waiting_composition.ready();
        }catch(const std::exception&){
            // Never expose the previous swap chain if composition failed.
            local(true);status=L"Could not display the connection screen.";return;
        }
        waiting_visible=true;
        const auto r=viewer_client();const int width=r.right-r.left,height=r.bottom-r.top;
        SetWindowPos(waiting,HWND_TOP,r.left,r.top,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        SetWindowPos(viewer,windowed?HWND_NOTOPMOST:HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
        // Showing an owner can change its owned popup visibility. Reassert the
        // prepared cover after the viewer becomes visible, before activation.
        SetWindowPos(waiting,HWND_TOP,r.left,r.top,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        finish_transition();collect_viewers();
        SetForegroundWindow(viewer);SetFocus(viewer);SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    }
    void reset_remote_cursor() {
        cursor_position_gate.reset(0);cursor_trace_count=0;
        stream_health={};source_diagnostics={};viewer_diagnostics={};input_diagnostics={};source_stats_time=health_time=0;debug_pulses={};slow_indicator.reset();if(health_badge)ShowWindow(health_badge,SW_HIDE);
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));if(remote_cursor)DestroyCursor(remote_cursor);
        remote_cursor=nullptr;remote_cursor_visible=true;remote_cursor_received=false;remote_pointer={};
        remote_cursor_composition.reset();
    }
    HCURSOR selected_remote_cursor() const {
        return remote_cursor_composition.visible(remote_cursor_visible,relative_mode)?
            (remote_cursor?remote_cursor:LoadCursorW(nullptr,IDC_ARROW)):nullptr;
    }
    void apply_remote_cursor() {
        POINT point{};GetCursorPos(&point);
        const auto r=viewer_client();
        if(remote&&WindowFromPoint(point)==viewer&&PtInRect(&r,point))
            SetCursor(selected_remote_cursor());
    }
    void apply_remote_position() {
        sample_physical_pointer();
        if(!remote||relative_mode||!remote_cursor_received||!remote_width||!remote_height)return;
        const auto decision=cursor_position_gate.observe(remote_pointer,input_capture.cursor_position_allowed(input_session,remote_pointer.input_capture_us));
        if(decision!=CursorPositionGate::Result::Revision&&cursor_trace_count<8){
            ++cursor_trace_count;log("Cursor position: session="+std::to_string(input_session)+" revision="+std::to_string(remote_pointer.position_revision)+
                " decision="+std::to_string(int(decision))+" xy="+std::to_string(remote_pointer.x)+","+std::to_string(remote_pointer.y)+
                " input_us="+std::to_string(remote_pointer.input_capture_us));
        }
        if(decision!=CursorPositionGate::Result::Allowed)return;
        POINT point{};GetCursorPos(&point);
        // Never pull against recent local movement or take the pointer out of our UI.
        if(own_surface(point)||GetTickCount64()-last_local_motion<100)return;
        const auto client=viewer_client();
        if(windowed&&(GetForegroundWindow()!=viewer||sizing_viewer||!PtInRect(&client,point)))return;
        const double width=client.right-client.left,height=client.bottom-client.top;
        const double scale=std::min(width/remote_width,height/remote_height);
        const double x=(width-remote_width*scale)/2+(remote_pointer.x/65535.0)*(remote_width-1)*scale;
        const double y=(height-remote_height*scale)/2+(remote_pointer.y/65535.0)*(remote_height-1)*scale;
        INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE;
        input.mi.dx=LONG((client.left+x+.5)*65536/GetSystemMetrics(SM_CXSCREEN));input.mi.dy=LONG((client.top+y+.5)*65536/GetSystemMetrics(SM_CYSCREEN));input.mi.dwExtraInfo=InputMarker;
#ifdef HIVEDESK_SHELL_TEST
        if(cursor_position_test_sink){cursor_position_test_sink(input);return;}
#endif
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
        state.session=cursor_host_session;state.position_valid=cursor_sent&&cursor_host_session&&physical_host_session==cursor_host_session;
        state.input_capture_us=state.position_valid?physical_host_input_us:0;
        // The revision and position describe physical/programmatic host motion,
        // not the delayed echo of this controller's injected mouse movement.
        const POINT position=cursor_sent?physical_pointer:info.ptScreenPos;
        state.position_valid=state.position_valid&&cursor_on_primary(position.x,position.y,width,height);
        state.x=uint32_t(std::clamp(position.x,0L,LONG(std::max(1,width-1)))*65535LL/std::max(1,width-1));
        state.y=uint32_t(std::clamp(position.y,0L,LONG(std::max(1,height-1)))*65535LL/std::max(1,height-1));
        if(shape_changed&&info.hCursor&&!read_cursor_shape(info.hCursor,state))return;
        if(callbacks.cursor_changed(cursor_controller,state)) {
            cursor_sent=true;sent_cursor=info.hCursor;sent_cursor_visible=visible;sent_pointer_revision=pointer_revision;
        }
    }
    NestedReturn::View return_view()const {
        return {selection_armed?(pending_id.empty()?active_id:pending_id):active_id,input_session,generation.load()};
    }
    bool return_parent_valid()const{return !return_authorized||return_authorized();}
    void return_complete(std::optional<NestedReturn::Completion> done) {
        if(!done)return;
        return_authorized={};
        if(callbacks.return_finished)callbacks.return_finished(done->view.peer,done->view.session);
        if(done->close)local(true);
        else {publish_input();confine_mouse();}
        log(std::string(done->summon?"Remote Hive: peer=":"Nested return: peer=")+done->view.peer+" session="+std::to_string(done->view.session)+" result="+std::to_string(int(done->result))+" closed="+std::to_string(done->close));
        if(!done->parent.peer.empty()) {
            if(callbacks.return_reply)callbacks.return_reply(done->parent.peer,{done->parent.request.session,done->parent.request.id,done->result,done->summon});
        }else if(done->result!=ReturnResult::Returned){
            status=done->summon?L"Remote Hive unavailable. Update every computer in the chain and try again.":L"Could not return one level. Ctrl+Alt+Backspace returns directly to this computer.";
        }
    }
    void return_begin(NestedReturn::Parent parent={},std::function<bool()> authorized={},bool summon=false) {
        summon=summon||parent.request.summon;
        const auto respond=[&](ReturnResult result){if(!parent.peer.empty()&&callbacks.return_reply)callbacks.return_reply(parent.peer,{parent.request.session,parent.request.id,result,summon});};
        if(shutting_down||(authorized&&!authorized())){respond(ReturnResult::Stale);return;}
        if(!parent.peer.empty()){
            if(return_parent_peer!=parent.peer||return_parent_session!=parent.request.session){
                return_parent_peer=parent.peer;return_parent_session=parent.request.session;return_parent_id=0;
            }
            if(parent.request.id<=return_parent_id){respond(ReturnResult::Stale);return;}
            return_parent_id=parent.request.id;
        }
        if(nested_return.busy()){respond(ReturnResult::Busy);return;}
        const auto view=return_view();
        if(view.peer.empty()){
            if(summon){toggle_panel();respond(ReturnResult::Returned);}
            else if(parent.peer.empty())local(true);else respond(ReturnResult::Leaf);
            return;
        }
        const auto hops=parent.peer.empty()?16:parent.request.hops;
        if(!hops){respond(ReturnResult::Limit);return;}
        const auto peer=std::find_if(peers.begin(),peers.end(),[&](const Peer& p){return p.id==view.peer;});
        // Connecting has no active input lease yet. An old peer cannot report
        // a nested view; retain the pre-upgrade one-hop escape for that boundary.
        if(summon&&(!view.session||peer==peers.end()||!(peer->features&FeatureRemoteHive))){
            status=L"Remote Hive unavailable. Update every computer in the chain and try again.";
            respond(ReturnResult::Stale);return;
        }
        if(!summon&&(!view.session||peer==peers.end()||!(peer->features&FeatureNestedReturn))){
            local(true);respond(ReturnResult::Returned);return;
        }
        // Reveal the child beneath this layer's own Hive/Peek before asking
        // the deepest layer to show its panel. Keep the active viewer alive.
        if(summon)hide_panel(true);
        return_authorized=std::move(authorized);
        const auto request=nested_return.begin(view,std::move(parent),uint8_t(hops-1),GetTickCount64(),summon);
        publish_input();ClipCursor(nullptr);
        if(!callbacks.return_request||!callbacks.return_request(view.peer,request))
            return_complete(nested_return.reply(view.peer,{request.session,request.id,ReturnResult::Stale,summon},return_view(),return_parent_valid()));
    }
    void local(bool notify) {
        return_complete(nested_return.cancel());
        cancel_connection_hint();
        ++generation;remote=false;reconnecting=false;input_session=0;publish_input();
        // Remove video before its cover, including Escape during a fade.
        ShowWindow(viewer,SW_HIDE);
        hide_waiting();
        selection_armed=false;pending_id.clear();
        if(!active_id.empty())previous_id=active_id;active_id.clear();
        ShowWindow(viewer,SW_HIDE);hide_panel();ReleaseCapture();ClipCursor(nullptr);reset_remote_cursor();
        if(prior_focus&&IsWindow(prior_focus))SetForegroundWindow(prior_focus);prior_focus=nullptr;
        if(notify&&callbacks.return_local)callbacks.return_local();
    }
    void select(const std::string& id,bool as_window=false,POINT drop={}) {
        if(id.empty()){local(true);return;}
        if(id==active_id&&remote&&!reconnecting&&as_window==windowed){hide_panel();if(IsIconic(viewer))ShowWindow(viewer,SW_RESTORE);SetForegroundWindow(viewer);confine_mouse();publish_input();return;}
        auto it=std::find_if(peers.begin(),peers.end(),[&](const Peer& p){return p.id==id;});
        if(it==peers.end()){status=L"Computer is no longer in the device list.";InvalidateRect(panel,nullptr,FALSE);return;}
        ++generation;input_session=0;publish_input();
        const bool adopting=peek_live&&peek_surface&&peek_surface->peer==id&&!as_window;
        const HWND adopted=adopting?peek_surface->window:nullptr;end_peek(false,adopted);
        windowed=as_window;window_ratio=16.0/9;
        if(const auto snapshot=full_snapshots.find(id);snapshot!=full_snapshots.end()){BITMAP b{};GetObjectW(snapshot->second,sizeof(b),&b);if(b.bmWidth>0&&b.bmHeight>0)window_ratio=double(b.bmWidth)/b.bmHeight;}
        if(!replace_viewer(id,drop)){local(true);status=L"Could not create the desktop window.";return;}
        remote=false;reconnecting=false;publish_input();
        reset_remote_cursor();
        remote_width=remote_height=0;selection_armed=true;pending_id=id;ReleaseCapture();ClipCursor(nullptr);hide_panel();
        begin_waiting(id,false,adopting);
        if(!selection_armed)return;
        if(callbacks.select_peer)callbacks.select_peer(id);
    }
    void show_remote_ui(const std::string& id,const std::wstring& label,uint64_t session) {
        if(shutting_down||!selection_armed||(!pending_id.empty()&&pending_id!=id))return;
        cancel_connection_hint();reconnecting=false;InvalidateRect(health_badge,nullptr,FALSE);
        if(active_id!=id)previous_id=active_id;
        if(!remote&&!IsWindowVisible(viewer))prior_focus=GetForegroundWindow();
        if(!remote||active_id!=id||input_session!=session){remote_cursor_composition.reset(session);cursor_position_gate.reset(session);cursor_trace_count=0;}
        active_id=id;input_session=session;remote=true;SetWindowTextW(viewer,(L"HiveDesk - "+label).c_str());
        if(waiting_visible&&!waiting_fading){
            waiting_fading=true;
            try{SetTimer(owner,4,waiting_composition.hide(),nullptr);}catch(const std::exception&){hide_waiting();}
        }
        SetWindowPos(viewer,windowed?HWND_NOTOPMOST:HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
        SetForegroundWindow(viewer);SetFocus(viewer);confine_mouse();hide_panel();apply_remote_position();apply_remote_cursor();
        publish_input();
    }
    void confine_mouse() {
        if(!remote||nested_return.busy()||reconnecting||panel_mode||taskbar_peek.active()||taskbar_focus_guard||sizing_viewer||GetForegroundWindow()!=viewer||(windowed&&!relative_mode)){ClipCursor(nullptr);return;}
        RECT r=viewer_client();
        if(relative_mode){int x=(r.left+r.right)/2,y=(r.top+r.bottom)/2;r={x,y,x+1,y+1};SetCursorPos(x,y);}
        ClipCursor(&r);
    }
    void set_relative(bool enabled) {
        relative_mode=enabled&&raw_mouse_available;confine_mouse();
        apply_remote_cursor();
        publish_input();
        status=relative_mode?L"Relative mouse on. Ctrl+Alt+` returns one level; Ctrl+Alt+Backspace returns locally.":L"Absolute desktop mouse";
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
        n.hIcon=app_small_icon;wcscpy_s(n.szTip,L"HiveDesk - Ctrl+Alt+` back one level; Ctrl+Alt+Backspace local");
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
    void begin_panel_cycle(POINT pointer) {
        end_peek(false);
        // A new opening owns a fresh taskbar state. Pointer exit during the
        // previous opening never revokes the lease, but reopening always does.
        taskbar_peek.reset();
        taskbar_corner_armed=!TaskbarPeek::corner(pointer,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),dpi);
    }
    void show_panel(int mode,bool hotkey=false) {
        if(settings&&IsWindowVisible(settings))return;
        if(panel_mode==mode&&!panel_closing)return;
        const bool entering=panel_mode==0,reversing=panel_closing;const int previous_height=panel_height;
        if(entering||reversing){POINT pointer{};GetCursorPos(&pointer);begin_panel_cycle(pointer);}
        panel_closing=false;KillTimer(owner,2);panel_mode=mode;hovered=-1;outside_since=0;
        panel_width=px(200);panel_height=mode==1?tile_height()+px(24):GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(panel,HWND_TOPMOST,0,0,panel_width,panel_height,SWP_NOACTIVATE);
        // Upload even while hidden. UpdateWindow alone depends on visibility /
        // update-region state and can leave the previous surface after a failed
        // connection. Commit this image and its new clips/opacity together.
        if(!paint_panel(false)){hide_panel();return;}
        try{composition.show(float(panel_width),float(panel_height),float(previous_height),entering,reversing);
            if(hotkey&&entering)composition.reveal_cards(visible_cards(),RGB(GetRValue(accent)*16/100,GetGValue(accent)*16/100,GetBValue(accent)*16/100));
        }
        catch(const std::exception&){hide_panel();return;}
        SetWindowPos(panel,HWND_TOPMOST,0,0,panel_width,panel_height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        publish_input();
        confine_mouse();
        if(entering&&callbacks.panel_opened)callbacks.panel_opened();
        publish_visible();
    }
    void toggle_panel() {
        if(panel_mode&&!panel_closing)hide_panel(true,taskbar_peek.active());
        else{show_panel(2,true);panel_summoned=panel_mode!=0;}
        publish_input();
    }
    int click_card_at(POINT p) const {
        // The compact hot-corner preview owns the top/left screen edges too.
        // This extension is click-only: Peek requires the actual inset card.
        if(panel_mode==1)return p.x>=0&&p.x<panel_width-px(12)&&p.y>=0&&p.y<px(12)+tile_height()?preview_index():-1;
        return card_at(p);
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
        if(reconnecting)rows={{{waiting_retry?L"Reconnecting...  Ctrl + Alt + ` to cancel":L"Connecting...  Ctrl + Alt + ` to cancel",DiagnosticTone::Warning}}};
        else if(config.desktop.debug_overlay)rows=debug_rows();
        else rows={{{(stream_health.pressure&PressureNetwork)?L"Slow connection":L"Reduced frame rate",DiagnosticTone::Warning}}};
        if(debug_surface.render(rect.right,rect.bottom,dpi,debug_font,rows,reconnecting||!config.desktop.debug_overlay))debug_surface.present(health_badge);
    }
    void timer() {
        return_complete(nested_return.tick(GetTickCount64(),return_view(),return_parent_valid()));
        tick_peek();publish_visible();collect_viewers();
        // Latest-state mailbox: no per-report UI messages or stale report FIFO.
        std::optional<PendingSource> incoming;
        {std::lock_guard lock(source_mutex);incoming=std::move(pending_source);pending_source.reset();}
        if(incoming&&incoming->generation==generation.load()&&incoming->peer==active_id&&incoming->value.session==input_session){source_diagnostics=incoming->value;source_stats_time=incoming->received;}
        sample_cursor();
        if(GetTickCount64()-last_stream_tick>=100){last_stream_tick=GetTickCount64();if(callbacks.stream_tick)callbacks.stream_tick();}
        const bool warning=slow_indicator.update(GetTickCount64(),stream_health);
        if(health_badge) {
            if(remote&&!waiting_visible&&(reconnecting?connection_hint_visible:(warning||config.desktop.debug_overlay))&&IsWindowVisible(viewer)&&GetForegroundWindow()==viewer) {
                if(!reconnecting&&config.desktop.debug_overlay&&callbacks.debug_tick)callbacks.debug_tick();
                const auto client=viewer_client();
                const int width=std::min(px(reconnecting?280:config.desktop.debug_overlay?460:184),std::max(px(80),int(client.right-client.left)-px(32))),height=std::min(px(!reconnecting&&config.desktop.debug_overlay?474:30),std::max(px(30),int(client.bottom-client.top)-px(32)));RECT bounds{};GetWindowRect(health_badge,&bounds);
                const int left=client.left+px(16),top=client.top+px(16);
                if(!IsWindowVisible(health_badge)||bounds.left!=left||bounds.top!=top||bounds.right-bounds.left!=width||bounds.bottom-bounds.top!=height){SetWindowPos(health_badge,HWND_TOP,left,top,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);InvalidateRect(health_badge,nullptr,FALSE);}
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
        POINT p{};GetCursorPos(&p);update_panel_pointer(p,GetTickCount64());
    }
    void update_panel_pointer(POINT p,ULONGLONG now) {
        if(panel_mode&&!panel_closing){const int h=card_at(p);hover_peek(h);}else hover_peek(-1);
        taskbar_peek.tick();
        if(panel_mode==2&&!panel_closing&&pressed_card<0){
            const bool trigger=TaskbarPeek::corner(p,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),dpi);
            if(!trigger)taskbar_corner_armed=true;
            if(!taskbar_peek.active()&&trigger&&taskbar_corner_armed){
                taskbar_corner_armed=false;end_peek(false);
                taskbar_focus_guard=true;publish_input(true);ClipCursor(nullptr);
                if(!taskbar_peek.enter(panel,viewer))taskbar_focus_guard=false;
                publish_input();
            }
        }
        const bool corner=p.x>=0&&p.x<=px(3)&&p.y>=0&&p.y<=px(3);
        // Start the fade on the first corner sample; no hover dwell before showing it.
        if(!panel_mode){if(corner&&!input_capture.remote_pointer_active())show_panel(1);return;}
        if(panel_closing){if(!taskbar_peek.active()&&p.x>=0&&p.x<panel_width&&p.y>=0&&p.y<panel_height)show_panel(panel_mode);return;}
        if(panel_mode==1&&p.x>=0&&p.x<px(42)&&p.y>px(32))show_panel(2);
        if(pressed_card>=0){
            if(dragging&&p.x>=0&&p.x<panel_width&&now-last_drag_scroll>=120){
                const int direction=p.y<px(36)?-1:p.y>panel_height-px(36)?1:0;
                const int next=std::clamp(scroll+direction,0,std::max(0,int(peers.size())+1-visible_tiles()));
                if(next!=scroll){scroll=next;drop_slot=std::clamp(int((p.y-px(12)+tile_stride()/2)/tile_stride())+scroll-1,0,int(peers.size()));composition.finish_cards(false);paint_panel(false);drag_layers();move_drag_visual(p);}last_drag_scroll=now;
            }return;
        }
        if(!panel_summoned&&(p.x<0||p.x>panel_width+px(40)||p.y<0||p.y>panel_height+px(30))) {if(!outside_since)outside_since=now;if(now-outside_since>250)hide_panel(true,taskbar_peek.active());}else outside_since=0;
    }
    bool paint_panel(bool commit=true) {
        PAINTSTRUCT ps{};BeginPaint(panel,&ps);RECT r{};GetClientRect(panel,&r);
        const int width=std::max(1L,r.right),height=std::max(1L,r.bottom);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!dc||!bitmap){if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);EndPaint(panel,&ps);return false;}
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
        panel_pixels.assign(rgba,rgba+size_t(width)*height);pixels_width=width;pixels_height=height;
        bool rendered=false;
        try{composition.present(pixels,width,height,commit);rendered=true;}
        catch(const std::exception&){
            // A failed DComp device/surface must recover independently of a
            // later successful video connection. Rebuild and upload once.
            try{composition=PanelComposition{};composition.create(panel);composition.present(pixels,width,height,false);
                composition.show(float(width),float(height),0,true,false);rendered=true;}
            catch(const std::exception& e){OutputDebugStringA(e.what());}
        }
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);EndPaint(panel,&ps);
        return rendered;
    }

    void read_clipboard(bool) {clipboard_desktop.refresh();}
    void write_clipboard(const std::wstring& value) {clipboard_desktop.apply(text_clipboard(value));}
    void apply_sync(const std::wstring& value) {write_clipboard(value);}
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
        if(fields[0]==L"preferences"&&(fields.size()==6||fields.size()==7||fields.size()==8||fields.size()==9)){
            DesktopPreferences desktop=config.desktop;uint32_t startup=0,tray_icon=0,debug=0,relative=0;
            if(!number(fields[2],startup)||startup>1||!number(fields[3],tray_icon)||tray_icon>1||!number(fields[4],desktop.hotkey_modifiers)||!number(fields[5],desktop.hotkey_key))return 0;
            desktop.startup=startup!=0;desktop.tray_icon=tray_icon!=0;
            if(fields.size()>=7){if(!number(fields[6],debug)||debug>1)return 0;desktop.debug_overlay=debug!=0;}
            if(fields.size()>=8){if(!number(fields[7],relative)||relative>1)return 0;desktop.relative_mouse=relative!=0;}
            // Accept an older Settings client's trailing boolean, but never
            // restore the retired repeat-presentation experiment.
            if(fields.size()==9){uint32_t retired=0;if(!number(fields[8],retired)||retired>1)return 0;}
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
        fallback_settings_ready=false;
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
        child(L"STATIC",L"Saved automatically",0,Save,24,350,320,22);
        child(L"STATIC",wide(HIVEDESK_VERSION).c_str(),0,0,24,379,185,24);
        child(L"BUTTON",L"Close",BS_PUSHBUTTON|WS_TABSTOP,Close,354,350,120,32);
        fallback_settings_ready=true;
        ShowWindow(settings,SW_SHOW);SetForegroundWindow(settings);SetFocus(GetDlgItem(settings,Name));
    }
    bool save_settings() {
        KillTimer(settings,6);
        auto desktop=config.desktop;desktop.startup=SendDlgItemMessageW(settings,Startup,BM_GETCHECK,0,0)==BST_CHECKED;desktop.tray_icon=SendDlgItemMessageW(settings,TrayIcon,BM_GETCHECK,0,0)==BST_CHECKED;
        desktop.debug_overlay=SendDlgItemMessageW(settings,DebugOverlay,BM_GETCHECK,0,0)==BST_CHECKED;
        desktop.relative_mouse=SendDlgItemMessageW(settings,RelativeMouse,BM_GETCHECK,0,0)==BST_CHECKED;
        const auto selected=SendDlgItemMessageW(settings,Hotkey,CB_GETCURSEL,0,0);
        if(selected>=1&&selected<=3){desktop.hotkey_key='H';desktop.hotkey_modifiers=selected==1?3:selected==2?6:9;}
        const bool saved=apply_preferences(control_text(GetDlgItem(settings,Name)),desktop);
        SetDlgItemTextW(settings,Save,saved?L"Saved automatically":L"Not saved. Check name / shortcut.");return saved;
    }
    static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        Impl* s=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){s=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(window,message,w,l);
        if(window==s->drag_ghost){if(message==WM_NCHITTEST)return HTTRANSPARENT;if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;}
        if(message==WM_CLOSE&&window!=s->viewer&&std::any_of(s->retired_viewers.begin(),s->retired_viewers.end(),
            [window](const auto& surface){return surface->window==window;}))return 0;
        if(window==s->health_badge) {
            if(message==WM_NCHITTEST)return HTTRANSPARENT;
            if(message==WM_ERASEBKGND)return 1;
            if(message==WM_PAINT){PAINTSTRUCT ps{};BeginPaint(window,&ps);EndPaint(window,&ps);s->paint_health();return 0;}
        }

        if(window==s->owner||(!s->owner&&message==WM_NCCREATE)) {
            if(message==s->taskbar_created&&s->taskbar_created){s->tray_present=false;s->tray();return 0;}
            switch(message) {
            case WM_HOTKEY:
                if(int(w)==s->hotkey_id)s->toggle_panel();
                else if(w==83&&!s->recording_hotkey)s->return_begin({}, {},true);return 0;
            case CaptureActionMessage:{
                const auto action=CaptureAction(w);
                if(action==CaptureAction::KickController){if(!s->shutting_down&&s->callbacks.kick_controller)s->callbacks.kick_controller();return 0;}
                if(s->shutting_down||uint64_t(l)!=s->generation.load())return 0;
                switch(action){
                case CaptureAction::ReturnLocal:s->local(true);break;
                case CaptureAction::ReturnOne:s->return_begin();break;
                case CaptureAction::Failed:s->local(true);s->status=L"Input channel stalled. Returned to this computer.";break;
                case CaptureAction::HidePanel:s->hide_panel();break;
                case CaptureAction::HidePanelAnimated:s->hide_panel(true,s->taskbar_peek.active());break;
                case CaptureAction::Summon:s->toggle_panel();break;
                case CaptureAction::SummonRemote:s->return_begin({}, {},true);break;
                default:break;
                }return 0;
            }
            case KickControllerMessage:if(!s->shutting_down&&s->callbacks.kick_controller)s->callbacks.kick_controller();return 0;
            case WM_COPYDATA:return s->settings_command(w,reinterpret_cast<const COPYDATASTRUCT*>(l));
            case ViewerLayoutMessage:s->sync_viewer_ui();return 0;
            case WM_APP+44:return s->relative_mode?1:0;
            case WM_APP+45:s->settings_ui();return 0;
            case WM_APP+46:return s->hotkey_registered?1:0;
            case WM_APP+49:return s->raw_mouse_available?1:0;
            case UiTask:{std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(l));if(task&&!s->shutting_down)(*task)();return 0;}
            case WM_TIMER:if(w==1)s->timer();else if(w==2&&s->panel_closing)s->hide_panel(false,s->taskbar_peek.active());else if(w==3){KillTimer(window,3);s->show_panel(2,true);s->panel_summoned=s->panel_mode!=0;s->publish_input();}else if(w==4&&s->waiting_fading)s->hide_waiting();else if(w==5)s->reveal_connection_hint();return 0;
            case WM_DWMCOLORIZATIONCOLORCHANGED:case WM_THEMECHANGED:case WM_SETTINGCHANGE:s->system_color();return 0;
            case WM_CLIPBOARDUPDATE:s->read_clipboard(true);return 0;
            case TrayMessage:
                if(LOWORD(l)==WM_CONTEXTMENU||LOWORD(l)==WM_RBUTTONUP)s->tray_menu();
                else if(LOWORD(l)==WM_LBUTTONDBLCLK||LOWORD(l)==NIN_KEYSELECT){KillTimer(s->owner,3);s->settings_ui();}
                else if(LOWORD(l)==NIN_SELECT){SetTimer(s->owner,3,GetDoubleClickTime(),nullptr);}return 0;
            case WM_CLOSE:s->local(true);s->shutting_down=true;s->tray(true);PostQuitMessage(0);return 0;
            }
        }
        if(window!=s->viewer&&GetPropW(window,L"HiveDesk.PeekSurface")){
            if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
            if(message==WM_NCHITTEST)return HTTRANSPARENT;
            if(message==WM_ERASEBKGND)return 1;
        }
        if(window==s->panel) {
            switch(message) {
            case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
            case WM_ERASEBKGND:return 1;
            case WM_PAINT:s->paint_panel();return 0;
            case WM_LBUTTONDOWN:{
                POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};s->cancel_drag();s->pressed_card=s->click_card_at(p);s->drag_origin=p;
                if(s->pressed_card>0)s->dragged_peer=s->peers[s->pressed_card-1].id;
                if(s->pressed_card>=0){
                    const int top=s->px(12)+(s->panel_mode==1?0:s->pressed_card-s->scroll)*s->tile_stride();
                    s->drag_anchor={p.x-s->px(12),p.y-top};
                    // Background/no-activate windows cannot retain capture
                    // outside their bounds. Activate only for the gesture.
                    s->drag_focus=GetForegroundWindow();SetForegroundWindow(window);SetCapture(window);s->composition.finish_cards();s->publish_input();
                }return 0;
            }
            case WM_CAPTURECHANGED:case WM_CANCELMODE:s->cancel_drag();return 0;
            case WM_MOUSEMOVE:{
                POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
                if(s->pressed_card>0&&!s->dragging&&(abs(p.x-s->drag_origin.x)>=GetSystemMetrics(SM_CXDRAG)||abs(p.y-s->drag_origin.y)>=GetSystemMetrics(SM_CYDRAG))){s->end_peek(false);s->dragging=true;if(s->panel_mode==1)s->show_panel(2);s->begin_drag_visual();}
                if(s->dragging){
                    const bool outside=p.x>=s->panel_width;
                    s->drop_slot=outside?-1:std::clamp(int((p.y-s->px(12)+s->tile_stride()/2)/s->tile_stride())+s->scroll-1,0,int(s->peers.size()));
                    SetCursor(LoadCursorW(nullptr,outside?IDC_SIZEALL:IDC_SIZENS));s->move_drag_visual(p);return 0;
                }
                int h=s->card_at(p);s->hover_peek(h);if(h!=s->hovered){s->hovered=h;InvalidateRect(window,nullptr,FALSE);}return 0;
            }
            case WM_LBUTTONUP:{
                POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const bool dragged=s->dragging;const auto id=s->dragged_peer;const int slot=std::clamp(int((p.y-s->px(12)+s->tile_stride()/2)/s->tile_stride())+s->scroll-1,0,int(s->peers.size()));
                const int pressed=s->pressed_card,index=s->click_card_at(p);
                const bool opening=(dragged&&p.x>=s->panel_width)||(!dragged&&pressed>0&&pressed==index&&index<=int(s->peers.size())&&s->peers[index-1].id==id);
                if(opening&&s->drag_focus&&s->drag_focus!=s->viewer)s->prior_focus=s->drag_focus;
                s->cancel_drag(!opening,false);
                if(dragged){
                    if(p.x>=s->panel_width){ClientToScreen(window,&p);s->select(id,true,p);}
                    else {if(slot>=0&&p.x>=0&&p.y>=0&&p.y<s->panel_height)s->reorder_peer(id,slot);s->paint_panel();}
                }else if(pressed>=0&&pressed==index){
                    if(index==0)s->select({});else if(index>0&&index<=int(s->peers.size())&&s->peers[index-1].id==id)s->select(id);
                }
                return 0;
            }
            case WM_RBUTTONUP:s->tray_menu();return 0;
            case WM_MOUSEWHEEL:{s->composition.finish_cards();int visible=s->visible_tiles();s->scroll=std::clamp(s->scroll-GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA,0,std::max(0,static_cast<int>(s->peers.size())+1-visible));
                if(s->dragging){s->paint_panel(false);s->drag_layers();POINT p{};GetCursorPos(&p);ScreenToClient(window,&p);s->move_drag_visual(p);}else InvalidateRect(window,nullptr,FALSE);return 0;}
            }
        }
        if(window==s->settings) {
            switch(message) {
            case WM_CLOSE:
                if(s->fallback_settings_ready&&!s->save_settings()&&MessageBoxW(window,L"Changes could not be saved. Close without saving?",L"HiveDesk",MB_YESNO|MB_ICONWARNING)!=IDYES)return 0;
                DestroyWindow(window);return 0;
            case WM_DESTROY:s->fallback_settings_ready=false;s->settings=nullptr;s->settings_restart.close();s->publish_input();return 0;
            case WM_TIMER:if(w==6&&s->fallback_settings_ready)s->save_settings();return 0;
            case WM_ERASEBKGND:{RECT r{};GetClientRect(window,&r);FillRect(reinterpret_cast<HDC>(w),&r,s->background_brush);return 1;}
            case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:SetTextColor(reinterpret_cast<HDC>(w),Text);SetBkColor(reinterpret_cast<HDC>(w),Background);return reinterpret_cast<LRESULT>(s->background_brush);
            case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(w),Text);SetBkColor(reinterpret_cast<HDC>(w),Card);return reinterpret_cast<LRESULT>(s->edit_brush);
            case WM_COMMAND:
                switch(LOWORD(w)) {
                case Close:SendMessageW(window,WM_CLOSE,0,0);return 0;
                case Name:if(s->fallback_settings_ready&&HIWORD(w)==EN_CHANGE){SetDlgItemTextW(window,Save,L"Saving...");SetTimer(window,6,350,nullptr);}return 0;
                case Startup:case TrayIcon:case DebugOverlay:case RelativeMouse:
                    if(s->fallback_settings_ready&&HIWORD(w)==BN_CLICKED)s->save_settings();return 0;
                case Hotkey:if(s->fallback_settings_ready&&HIWORD(w)==CBN_SELCHANGE)s->save_settings();return 0;
                }break;
            }
        }
        if(window==s->waiting){
            switch(message){
            case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
            case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;
            case WM_ERASEBKGND:return 1;
            case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(window,&ps);EndPaint(window,&ps);return 0;}
            }
        }
        if(window==s->viewer) {
            switch(message){
            case WM_SETCURSOR:if(s->remote&&LOWORD(l)==HTCLIENT){s->apply_remote_cursor();return TRUE;}break;
            case WM_ACTIVATE:
                if(LOWORD(w)!=WA_INACTIVE&&s->taskbar_peek.active())s->hide_panel(true);
                s->publish_input();s->confine_mouse();if(LOWORD(w)==WA_INACTIVE)ShowWindow(s->health_badge,SW_HIDE);break;
            case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:
                // Also handle a click if Explorer could not acquire foreground
                // on entry and the viewer therefore never lost activation.
                if(s->taskbar_peek.active()){s->hide_panel(true);s->publish_input();return 0;}break;
            case WM_ENTERMENULOOP:case WM_ENTERSIZEMOVE:s->sizing_viewer=true;s->publish_input();ClipCursor(nullptr);ShowWindow(s->health_badge,SW_HIDE);return 0;
            case WM_EXITMENULOOP:case WM_EXITSIZEMOVE:s->sizing_viewer=false;s->size_to_source();s->sync_viewer_ui();s->confine_mouse();return 0;
            case WM_SIZING:if(s->windowed){
                RECT frame{};AdjustWindowRectExForDpi(&frame,DWORD(GetWindowLongW(window,GWL_STYLE)),FALSE,DWORD(GetWindowLongW(window,GWL_EXSTYLE)),GetDpiForWindow(window));
                RECT previous{};GetWindowRect(window,&previous);
                constrain_viewer(*reinterpret_cast<RECT*>(l),UINT(w),frame.right-frame.left,frame.bottom-frame.top,s->window_ratio,s->px(240),&previous);return TRUE;
            }break;
            case WM_GETMINMAXINFO:if(s->windowed){
                RECT r{0,0,s->px(240),LONG(std::lround(s->px(240)/s->window_ratio))};AdjustWindowRectExForDpi(&r,DWORD(GetWindowLongW(window,GWL_STYLE)),FALSE,DWORD(GetWindowLongW(window,GWL_EXSTYLE)),GetDpiForWindow(window));
                auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={r.right-r.left,r.bottom-r.top};return 0;
            }break;
            case WM_WINDOWPOSCHANGING:if(s->windowed&&!s->sizing_viewer&&!IsIconic(window)&&!IsZoomed(window)){
                auto* position=reinterpret_cast<WINDOWPOS*>(l);
                if(!(position->flags&SWP_NOSIZE)&&position->cx>=s->px(240)&&position->cy>GetSystemMetrics(SM_CYMINIMIZED)){
                    RECT frame{};AdjustWindowRectExForDpi(&frame,DWORD(GetWindowLongW(window,GWL_STYLE)),FALSE,DWORD(GetWindowLongW(window,GWL_EXSTYLE)),GetDpiForWindow(window));
                    position->cy=LONG(std::lround(std::max(1L,position->cx-(frame.right-frame.left))/s->window_ratio))+frame.bottom-frame.top;
                }
            }break;
            // DXGI sends these synchronously while holding graphics locks.
            // Never enter DComp/BeginDraw from that nested message: the DXGI
            // worker cannot release its lock until this handler returns.
            case WM_SIZE:
                if(w==SIZE_MAXIMIZED)s->viewer_maximized=true;
                else if(w==SIZE_RESTORED&&s->viewer_maximized){s->viewer_maximized=false;s->viewer_restore_pending=true;}
                s->request_viewer_layout();break;
            case WM_MOVE:s->request_viewer_layout();break;
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
        for(const wchar_t* name:{L"HiveDesk.Owner",L"HiveDesk.Viewer",L"HiveDesk.Switcher",L"HiveDesk.Settings",L"HiveDesk.Health",L"HiveDesk.Waiting",L"HiveDesk.Drag"}) {
            WNDCLASSEXW c{};c.cbSize=sizeof(c);c.lpfnWndProc=window_proc;c.hInstance=instance;c.hCursor=LoadCursorW(nullptr,IDC_ARROW);c.hbrBackground=background_brush;c.lpszClassName=name;c.hIcon=app_icon;c.hIconSm=app_small_icon;
            if(!RegisterClassExW(&c)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){error="Could not register native UI classes.";return false;}
        }
        owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"HiveDesk.Owner",L"HiveDesk",WS_OVERLAPPED,0,0,0,0,nullptr,nullptr,instance,this);
        // Consume a launcher's STARTF_USESHOWWINDOW override on the invisible,
        // zero-size tool window, so the first real settings/viewer show is honored.
        if(owner){ShowWindow(owner,SW_SHOWNOACTIVATE);ShowWindow(owner,SW_HIDE);}
        viewer=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,L"HiveDesk.Viewer",L"HiveDesk",WS_POPUP,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),nullptr,nullptr,instance,this);
        if(viewer)viewer_surface.store(std::make_shared<const ViewerSurface>(ViewerSurface{viewer,{}}));
        panel=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,L"HiveDesk.Switcher",L"HiveDesk computers",WS_POPUP,0,0,px(200),px(123),nullptr,nullptr,instance,this);
        health_badge=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,L"HiveDesk.Health",L"Stream status",WS_POPUP,0,0,px(136),px(30),viewer,nullptr,instance,this);
        waiting=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,L"HiveDesk.Waiting",L"HiveDesk connection",WS_POPUP,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),viewer,nullptr,instance,this);

        if(!owner||!viewer||!panel||!waiting){error="Could not create native desktop windows.";return false;}
        try{composition.create(panel);waiting_composition.create(waiting);}catch(const std::exception& e){error=e.what();return false;}system_color();
        // GetAsyncKeyState cannot distinguish remote-injected modifiers. Start
        // physical shortcut state empty and populate it from non-injected hooks.
        publish_input();
        if(!input_capture.start(instance,owner,CaptureActionMessage,[this](const std::string& peer,uint64_t session,const InputEvent& event,uint64_t captured_us){
            if(callbacks.captured_input)callbacks.captured_input(peer,session,event,captured_us);
            else if(callbacks.input)callbacks.input(event);
        },error))return false;
        raw_mouse_available=input_capture.raw_mouse_available();
        relative_mode=config.desktop.relative_mouse&&raw_mouse_available;publish_input();
        if(!clipboard_desktop.start([this](Clipboard content){
            enqueue([this,content]{if(clipboard_desktop.cached()!=content)return;
                if(callbacks.rich_clipboard_changed)callbacks.rich_clipboard_changed(content);
                else if(callbacks.clipboard_changed)callbacks.clipboard_changed(content->offer.text);
            });
        },error))return false;
        hotkey_registered=RegisterHotKey(owner,hotkey_id,config.desktop.hotkey_modifiers|MOD_NOREPEAT,config.desktop.hotkey_key)!=FALSE;
        if(!hotkey_registered)log_hotkey_conflict();
        if(!RegisterHotKey(owner,83,MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_NOREPEAT,'H'))log("Remote Hive shortcut Ctrl+Alt+Shift+H is already in use.");
        taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");tray();SetTimer(owner,1,16,nullptr);read_clipboard(false);
        worker=std::thread([this]{for(;;){std::function<void()> f;{std::unique_lock lock(work_mutex);work_ready.wait(lock,[this]{return stop_worker||!work.empty();});if(stop_worker)break;f=std::move(work.front());work.pop_front();}
            try{f();}catch(const std::exception& e){std::wstring message=L"Operation failed: "+wide(e.what());dispatch([this,message]{status=message;InvalidateRect(panel,nullptr,FALSE);});}
            catch(...){dispatch([this]{status=L"Operation failed";InvalidateRect(panel,nullptr,FALSE);});}
        }});return true;
    }
    ~Impl() {
        log("Shell shutdown: input capture");
        taskbar_peek.reset();
        shutting_down=true;++generation;publish_input();input_capture.stop();ReleaseCapture();ClipCursor(nullptr);
        log("Shell shutdown: clipboard");
        clipboard_desktop.stop();
        log("Shell shutdown: background operations");
        {std::lock_guard lock(work_mutex);stop_worker=true;work.clear();}work_ready.notify_one();if(worker.joinable())worker.join();
        log("Shell shutdown: native windows");
        if(owner){UnregisterHotKey(owner,hotkey_id);UnregisterHotKey(owner,83);KillTimer(owner,1);KillTimer(owner,3);tray(true);}
        if(settings_process){if(auto window=external_settings_window())PostMessageW(window,WM_CLOSE,0,0);CloseHandle(settings_process);settings_process=nullptr;settings=nullptr;}
        if(settings)DestroyWindow(settings);if(panel)DestroyWindow(panel);if(health_badge)DestroyWindow(health_badge);if(waiting)DestroyWindow(waiting);if(viewer)DestroyWindow(viewer);
        for(const auto& surface:retired_viewers)DestroyWindow(surface->window);
        for(const auto& entry:warm_surfaces)if(entry.second)DestroyWindow(entry.second->window);warm_surfaces.clear();
        retired_viewers.clear();viewer_surface.store(nullptr);
        if(owner){MSG m{};while(PeekMessageW(&m,owner,UiTask,UiTask,PM_REMOVE))delete reinterpret_cast<std::function<void()>*>(m.lParam);DestroyWindow(owner);}
        for(auto& item:thumbnails)DeleteObject(item.second);thumbnails.clear();
        for(auto& item:full_snapshots)DeleteObject(item.second);full_snapshots.clear();
        for(auto& item:prior_thumbnails)DeleteObject(item.second);prior_thumbnails.clear();
        reset_remote_cursor();
        if(debug_font)DeleteObject(debug_font);
        if(font)DeleteObject(font);if(small_font)DeleteObject(small_font);if(title_font)DeleteObject(title_font);if(background_brush)DeleteObject(background_brush);if(edit_brush)DeleteObject(edit_brush);
        log("Shell shutdown: composition resources");
    }
};
Shell::Shell():impl_(new Impl){}
Shell::~Shell(){close();}
void Shell::close(){if(impl_){delete impl_;impl_=nullptr;log("Shell shutdown: complete");}}
#ifdef HIVEDESK_SHELL_TEST
#include "../tests/shell_taskbar_regression.inc"
#include "../tests/shell_corner_regression.inc"
#include "../tests/shell_cursor_regression.inc"
#include "../tests/shell_nested_regression.inc"
#endif
bool Shell::create(HINSTANCE instance,const ShellConfig& config,ShellCallbacks callbacks,std::string& error){return impl_->create(instance,config,std::move(callbacks),error);}
int Shell::run(){MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){if(impl_->settings&&!impl_->settings_process&&IsDialogMessageW(impl_->settings,&m))continue;TranslateMessage(&m);DispatchMessageW(&m);}return static_cast<int>(m.wParam);}
HWND Shell::viewer_window()const{const auto surface=viewer_surface();return surface?surface->window:nullptr;}
std::shared_ptr<const ViewerSurface> Shell::viewer_surface()const{return impl_->viewer_surface.load();}
std::future<std::shared_ptr<const ViewerSurface>> Shell::renew_viewer_surface(std::shared_ptr<const ViewerSurface> expected){
    auto reply=std::make_shared<std::promise<std::shared_ptr<const ViewerSurface>>>();auto result=reply->get_future();
    const auto generation=impl_->generation.load();
    impl_->dispatch([s=impl_,expected=std::move(expected),reply,generation]{
        try{
            if(!expected||generation!=s->generation||s->viewer_surface.load()!=expected||!s->selection_armed||s->pending_id!=expected->peer){reply->set_value(nullptr);return;}
            RECT bounds{};GetWindowRect(s->viewer,&bounds);const bool maximized=IsZoomed(s->viewer)!=FALSE;
            if(!s->replace_viewer(expected->peer,{bounds.left,bounds.top},false)){reply->set_value(nullptr);return;}
            if(s->windowed){SetWindowPos(s->viewer,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);if(maximized)ShowWindow(s->viewer,SW_MAXIMIZE);}
            s->begin_waiting(expected->peer,true);
            reply->set_value(s->selection_armed?s->viewer_surface.load():nullptr);
        }catch(...){reply->set_exception(std::current_exception());}
    });return result;
}
void Shell::discard_warm_surface(std::shared_ptr<const ViewerSurface> surface){
    impl_->dispatch([s=impl_,surface=std::move(surface)]{
        const auto found=s->warm_surfaces.find(surface->peer);
        if(found==s->warm_surfaces.end()||found->second!=surface)return;
        if(s->peek_surface==surface)s->end_peek(false);
        s->retired_viewers.push_back(surface);s->warm_surfaces.erase(found);
        s->warm_visible.clear();s->publish_visible();
    });
}
void Shell::update_peers(const std::vector<Peer>& peers){impl_->dispatch([s=impl_,peers]{
    const auto contains=[&](const std::string& id){return std::any_of(peers.begin(),peers.end(),[&](const Peer& p){return p.id==id;});};
    const bool removed=std::any_of(s->peers.begin(),s->peers.end(),[&](const Peer& p){return !contains(p.id);});
    if(removed){
        s->cancel_drag();s->hovered=-1;
        if(!s->peek_id.empty()&&!contains(s->peek_id))s->end_peek(false);
        std::erase_if(s->fading_peeks,[&](const auto& p){if(contains(p.surface->peer))return false;ShowWindow(p.surface->window,SW_HIDE);return true;});
    }
    if(s->peers.size()!=peers.size())s->composition.finish_cards();
    s->peers=peers;s->sort_peers();s->scroll=std::min(s->scroll,static_cast<int>(peers.size()));
    s->publish_visible();s->publish_input();InvalidateRect(s->panel,nullptr,FALSE);
});}

void Shell::show_remote(const std::string& peer,const std::wstring& label,uint64_t session,HWND surface){uint64_t g=impl_->generation.load();impl_->dispatch([s=impl_,peer,label,session,g,surface]{if(g==s->generation.load()&&(!surface||surface==s->viewer))s->show_remote_ui(peer,label,session);});}
void Shell::show_reconnecting(const std::string& peer){const auto g=impl_->generation.load();impl_->dispatch([s=impl_,peer,g]{
    if(g!=s->generation.load()||!s->selection_armed||s->pending_id!=peer||s->reconnecting)return;
    // Keep the selection armed so a first frame can restore immersion, while
    // local()/Escape still invalidates every delayed UI completion.
    s->begin_waiting(peer,true);
});}
void Shell::set_remote_size(uint32_t width,uint32_t height){uint64_t g=impl_->generation.load();impl_->dispatch([s=impl_,width,height,g]{if(g==s->generation.load()){s->remote_width=width;s->remote_height=height;s->size_to_source();s->publish_input();}});}
void Shell::cancel_peek(std::shared_ptr<const ViewerSurface> surface){impl_->dispatch([s=impl_,surface=std::move(surface)]{
    const auto warm=s->warm_surfaces.find(surface->peer);
    const bool owns=s->peek_surface==surface||(!s->peek_surface&&warm!=s->warm_surfaces.end()&&warm->second==surface);
    if(s->peek_id==surface->peer&&owns)s->end_peek(false);
});}
void Shell::show_peek(std::shared_ptr<const ViewerSurface> surface,std::shared_ptr<MediaViewer> media,uint64_t epoch,bool ready){impl_->dispatch([s=impl_,surface=std::move(surface),media=std::move(media),epoch,ready]{s->show_peek_ui(surface,media,epoch,ready);});}
void Shell::show_local(){impl_->dispatch([s=impl_]{s->local(false);});}
void Shell::quit(){impl_->dispatch([s=impl_]{PostMessageW(s->owner,WM_CLOSE,0,0);});}
void Shell::set_status(const std::wstring& status){impl_->dispatch([s=impl_,status]{s->status=status;InvalidateRect(s->panel,nullptr,FALSE);});}
void Shell::show_error(const std::wstring& message){impl_->dispatch([s=impl_,message]{s->local(true);s->status=message;MessageBoxW(s->owner,message.c_str(),L"HiveDesk - Could not open desktop",MB_OK|MB_ICONERROR);});}
void Shell::apply_clipboard(const std::wstring& value){impl_->apply_sync(value);}
std::wstring Shell::cached_clipboard()const{return impl_->clipboard_desktop.cached()->offer.text;}
void Shell::apply_clipboard(Clipboard content){impl_->clipboard_desktop.apply(std::move(content));}
Clipboard Shell::cached_rich_clipboard()const{return impl_->clipboard_desktop.cached();}
bool Shell::clipboard_busy()const{return impl_->clipboard_desktop.busy();}
void Shell::show_settings(){impl_->dispatch([s=impl_]{s->settings_ui();});}
bool Shell::prepare_video_restart(){return impl_->settings_restart.prepare();}
void Shell::cancel_video_restart(){impl_->settings_restart.cancel();}
void Shell::update_thumbnail(const std::string& peer,Thumbnail thumbnail) {
    if(!valid_snapshot_dimensions(thumbnail.width,thumbnail.height)||thumbnail.bgra.size()!=size_t(thumbnail.width)*thumbnail.height*4)return;
    impl_->dispatch([s=impl_,peer,thumbnail=std::move(thumbnail)] {
        if(std::none_of(s->peers.begin(),s->peers.end(),[&](const Peer& p){return p.id==peer;}))return;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=SnapshotWidth;info.bmiHeader.biHeight=-LONG(SnapshotHeight);
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits=nullptr;
        HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);if(!bitmap)return;
        if(thumbnail.width==SnapshotWidth&&thumbnail.height==SnapshotHeight){
            std::memcpy(bits,thumbnail.bgra.data(),thumbnail.bgra.size());
            const auto old_full=s->full_snapshots.find(peer);
            if(old_full!=s->full_snapshots.end()){DeleteObject(old_full->second);s->full_snapshots.erase(old_full);}
        }
        else {
            info.bmiHeader.biWidth=thumbnail.width;info.bmiHeader.biHeight=-LONG(thumbnail.height);void* full_bits=nullptr;
            HBITMAP full=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&full_bits,nullptr,0);
            HDC source=CreateCompatibleDC(nullptr),target=CreateCompatibleDC(nullptr);
            if(!full||!source||!target){if(full)DeleteObject(full);if(source)DeleteDC(source);if(target)DeleteDC(target);DeleteObject(bitmap);return;}
            std::memcpy(full_bits,thumbnail.bgra.data(),thumbnail.bgra.size());
            const auto old_source=SelectObject(source,full),old_target=SelectObject(target,bitmap);
            std::memset(bits,0,size_t(SnapshotWidth)*SnapshotHeight*4);
            const double scale=std::min(double(SnapshotWidth)/thumbnail.width,double(SnapshotHeight)/thumbnail.height);
            const int w=std::max(1,int(thumbnail.width*scale)),h=std::max(1,int(thumbnail.height*scale));
            SetStretchBltMode(target,HALFTONE);SetBrushOrgEx(target,0,0,nullptr);
            const bool drawn=StretchBlt(target,(SnapshotWidth-w)/2,(SnapshotHeight-h)/2,w,h,source,0,0,thumbnail.width,thumbnail.height,SRCCOPY)!=FALSE;
            GdiFlush();SelectObject(source,old_source);SelectObject(target,old_target);DeleteDC(source);DeleteDC(target);
            if(!drawn){DeleteObject(full);DeleteObject(bitmap);return;}
            auto& previous=s->full_snapshots[peer];if(previous)DeleteObject(previous);previous=full;
            // Full images are only for the waiting screen; sidebar crossfades
            // retain small bitmaps. Bound the full-resolution cache to 128 MiB.
            size_t bytes=0;for(const auto& [id,image]:s->full_snapshots){BITMAP d{};GetObjectW(image,sizeof(d),&d);bytes+=size_t(d.bmWidth)*d.bmHeight*4;}
            for(auto it=s->full_snapshots.begin();bytes>128*1024*1024&&it!=s->full_snapshots.end();){
                if(it->first==peer){++it;continue;}
                BITMAP d{};GetObjectW(it->second,sizeof(d),&d);bytes-=size_t(d.bmWidth)*d.bmHeight*4;
                DeleteObject(it->second);it=s->full_snapshots.erase(it);
            }
        }
        auto prior=s->prior_thumbnails.find(peer);if(prior!=s->prior_thumbnails.end()){DeleteObject(prior->second);s->prior_thumbnails.erase(prior);}
        auto& cached=s->thumbnails[peer];if(cached)s->prior_thumbnails[peer]=cached;cached=bitmap;
        BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        s->thumbnail_fades[peer]=GetTickCount64()-(animations?0:180);InvalidateRect(s->panel,nullptr,FALSE);
        if(s->waiting_visible&&!s->waiting_fading&&s->pending_id==peer){
            try{s->paint_waiting(false);}catch(const std::exception&){s->local(true);}
        }
    });
}
void Shell::update_snapshot(const std::string& peer,Snapshot snapshot) {
    impl_->enqueue([this,peer,snapshot=std::move(snapshot)]{update_thumbnail(peer,decode_snapshot(snapshot));});
}
void Shell::observe_cursor(const std::string& controller,uint64_t session) {
    impl_->dispatch([s=impl_,controller,session] {
        s->sample_physical_pointer();s->cursor_host_session=controller.empty()?0:session;
        s->input_capture.cursor_host(s->cursor_host_session);s->physical_host_session=s->physical_host_input_us=0;
        s->cursor_controller=controller;s->cursor_sent=false;s->sent_cursor=nullptr;s->sent_pointer_revision=0;
        ++s->pointer_revision;GetCursorPos(&s->physical_pointer);s->sample_cursor();
    });
}
void Shell::cursor_input_applied(uint64_t session,uint64_t capture_us){impl_->input_capture.cursor_input_applied(session,capture_us);}
void Shell::return_one(const std::string& peer,ReturnRequest request,std::function<bool()> authorized){
    const auto generation=impl_->generation.load();
    impl_->dispatch([s=impl_,peer,request,generation,authorized=std::move(authorized)]{s->return_begin({peer,request},[s,generation,authorized]{return s->generation.load()==generation&&(!authorized||authorized());});});
}
void Shell::return_previous(){impl_->dispatch([s=impl_]{s->return_begin();});}
RelayPlan Shell::route_remote(uint64_t host_session,const InputEvent& event,bool desktop_available,bool isolated_test){return impl_->input_capture.route_remote(host_session,event,desktop_available,isolated_test);}
void Shell::returned(const std::string& peer,ReturnReply reply){
    impl_->dispatch([s=impl_,peer,reply]{s->return_complete(s->nested_return.reply(peer,reply,s->return_view(),s->return_parent_valid()));});
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
    const auto generation=impl_->generation.load();
    impl_->dispatch([s=impl_,peer,cursor=std::move(cursor),generation] {
        if(generation!=s->generation.load()||(peer!=s->pending_id&&peer!=s->active_id))return;
        if(cursor.session&&s->remote&&cursor.session!=s->input_session)return;
        if(cursor.session==s->remote_pointer.session&&cursor.position_revision<s->remote_pointer.position_revision)return;
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
