// Native shell integration smoke. All peers and viewer pixels are synthetic.
// No sockets, SendInput, mouse_event, or keybd_event. --gestures temporarily moves
// the pointer to exercise the real corner route, then restores its original position.
// --preview leaves the demo running with its local Switcher visible for inspection.
#include "hivedesk/shell.h"
#include "hivedesk/desktop_aux.h"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <set>
#include <vector>

namespace {
constexpr UINT TrayMessage = WM_APP + 42;
struct WindowQuery { const wchar_t* name; HWND found = nullptr; };
HWND find_own_window(const wchar_t* name) {
    WindowQuery query{name};
    EnumWindows([](HWND window, LPARAM argument) -> BOOL {
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (process != GetCurrentProcessId()) return TRUE;
        wchar_t name[128]{}; GetClassNameW(window, name, 128);
        auto& query = *reinterpret_cast<WindowQuery*>(argument);
        if (wcscmp(name, query.name) == 0) { query.found = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.found;
}
std::wstring field(HWND window, int id) {
    wchar_t text[256]{}; GetDlgItemTextW(window, id, text, 256); return text;
}
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }

struct Smoke {
    hive::Shell shell;
    bool preview = false, gestures = false, failed = false, capture_was_foreground = false, thumbnail_verified = false;
    int stage = 0, dpi = 96;
    HWND driver = nullptr, owner = nullptr, panel = nullptr;
    std::atomic<int> selected{0}, returned{0}, saved{0}, input_events{0};
    int snapshot_requests=0;
    std::atomic<bool> correct_save{false};
    std::vector<hive::Peer> peers{
        {"demo-studio", "Studio Workstation", "192.0.2.10", 47980, true},
        {"demo-render", "Render PC", "192.0.2.20", 47980, true},
        {"demo-laptop", "Travel Laptop", "192.0.2.30", 47980, false}
    };
    int px(int value) const { return MulDiv(value, dpi, 96); }
    void draw_demo(const std::string& id) {
        HWND viewer = shell.viewer_window(); HDC dc = GetDC(viewer); RECT r{}; GetClientRect(viewer, &r);
        const COLORREF color = id == "demo-render" ? RGB(37, 105, 78) : RGB(37, 83, 139);
        HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &r, brush); DeleteObject(brush);
        HFONT font = CreateFontW(-px(32), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ previous = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(240, 245, 250));
        const std::wstring label = id == "demo-render" ? L"Render PC / demo desktop" : L"Studio Workstation / demo desktop";
        RECT title{px(70), px(55), r.right - px(70), px(115)};
        DrawTextW(dc, label.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, previous); DeleteObject(font); ReleaseDC(viewer, dc); GdiFlush();
    }
    void open_panel() {
        // This is an app-local tray notification, not a system input injection.
        SendMessageW(owner, TrayMessage, 0, MAKELPARAM(NIN_SELECT, 1));
        SendMessageW(owner, WM_TIMER, 3, 0);
        UpdateWindow(panel);
    }
    void click_card(int index) {
        const int tile_height=(px(200)-2*px(12))*9/16;
        SendMessageW(panel, WM_LBUTTONDOWN, 0, MAKELPARAM(px(100), px(12)+index*(tile_height+px(16))+tile_height/2));
        SendMessageW(panel, WM_LBUTTONUP, 0, MAKELPARAM(px(100), px(12)+index*(tile_height+px(16))+tile_height/2));
    }
    void stop() { if (owner) PostMessageW(owner, WM_CLOSE, 0, 0); else PostQuitMessage(1); }
    struct Backdrop {
        HWND window{};
        Backdrop(HWND panel,int width) {
            window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP|SS_WHITERECT,
                0,0,width,GetSystemMetrics(SM_CYSCREEN),nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(window!=nullptr,"could not create uniform animation test backdrop");
            SetWindowPos(window,panel,0,0,width,GetSystemMetrics(SM_CYSCREEN),SWP_NOACTIVATE|SWP_SHOWWINDOW);UpdateWindow(window);
        }
        ~Backdrop(){DestroyWindow(window);}
    };
    void pump(DWORD duration) {
        const auto end=GetTickCount64()+duration;
        while(GetTickCount64()<end){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(1);}
    }
    std::vector<int> sample_opacity(int y,DWORD duration,bool dispatch=false) {
        HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=px(200);info.bmiHeader.biHeight=-1;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits=nullptr;
        HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);HGDIOBJ old=SelectObject(dc,bitmap);
        std::vector<int> values;bool fixed=true;const auto deadline=GetTickCount64()+duration;
        while(GetTickCount64()<deadline){
            if(dispatch)pump(1);
            DwmFlush();BitBlt(dc,0,0,px(200),1,screen,0,y,SRCCOPY);GdiFlush();
            auto* pixels=static_cast<uint32_t*>(bits);
            const int left=(pixels[px(5)]>>16)&255,right=(pixels[px(195)]>>16)&255;
            // Both edges must blend simultaneously; a moving reveal fails this.
            if(std::abs(left-right)>3)fixed=false;
            values.push_back(left);
        }
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
        require(fixed,"panel edges changed at different times: translation/reveal instead of opacity");
        return values;
    }
    void verify_compositor_motion() {
        BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        if(!animations){std::puts("SKIP animation samples: Windows client-area animations disabled.");return;}
        Backdrop backdrop(panel,px(200));
        // No UI dispatch during the samples: DWM must evaluate the opacity curve.
        auto values=sample_opacity(px(5),300);std::set<int> shades(values.begin(),values.end());
        require(shades.size()>=3&&values.back()<values.front(),"compositor did not fade in while UI dispatch was blocked");
        RECT rect{};GetWindowRect(panel,&rect);require(rect.left==0&&rect.right==px(200),"animation moved/resized the panel");
        std::printf("PASS compositor fade: %zu distinct opacity shades, both edges synchronized, UI dispatch blocked.\n",shades.size());
    }
    void verify_gesture_transitions() {
        BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        if(!animations){std::puts("SKIP fade gestures: Windows client-area animations disabled.");return;}
        struct RestorePointer {POINT point{};RestorePointer(){GetCursorPos(&point);}~RestorePointer(){SetCursorPos(point.x,point.y);}} restore;
        KillTimer(driver,1);
        Backdrop backdrop(panel,px(200));
        if(IsWindowVisible(panel))SendMessageW(owner,WM_HOTKEY,81,0);
        SetCursorPos(px(500),px(300));pump(650);require(!IsWindowVisible(panel),"sidebar did not finish animated dismissal");
        require(SetCursorPos(1,1)!=FALSE,"could not place pointer in corner");SendMessageW(owner,WM_TIMER,1,0);
        require(IsWindowVisible(panel),"corner preview did not open on the first sample (hover dwell)");pump(220);
        RECT compact{};GetWindowRect(panel,&compact);require(compact.right-compact.left==px(200),"sidebar exceeds compact 200 DIP width");
        require(compact.bottom<px(150),"corner preview unexpectedly opened full height");pump(280);
        SetCursorPos(1,px(50));SendMessageW(owner,WM_TIMER,1,0);
        // Sample below all cards. A height wipe cannot produce intermediate shades here.
        auto expansion=sample_opacity(GetSystemMetrics(SM_CYSCREEN)-px(10),300);
        std::set<int> expanded(expansion.begin(),expansion.end());
        require(expanded.size()>=3&&expansion.back()<expansion.front(),"additional list area did not fade in");
        pump(30);SetCursorPos(px(300),px(50));SendMessageW(owner,WM_TIMER,1,0);
        auto dismissal=sample_opacity(px(5),750,true);std::set<int> dismissed(dismissal.begin(),dismissal.end());
        require(dismissed.size()>=3&&dismissal.back()>dismissal.front(),"sidebar did not fade out");
        pump(40);require(!IsWindowVisible(panel),"sidebar remained visible after dismissal finished");
        // Reverse a partially completed fade-out using the real pointer route.
        SetCursorPos(1,1);pump(450);const int opaque=sample_opacity(px(5),1).back();SetCursorPos(px(300),px(50));
        std::vector<int> before;const auto reversal_deadline=GetTickCount64()+1500;
        do {pump(10);before=sample_opacity(px(5),1);}while(GetTickCount64()<reversal_deadline&&before.back()<opaque+20);
        require(IsWindowVisible(panel)&&before.back()<245,"could not observe a partial fade-out for reversal");
        SetCursorPos(1,px(5));SendMessageW(owner,WM_TIMER,1,0);
        auto reversed=sample_opacity(px(5),280,true);
        require(std::abs(reversed.front()-before.back())<60,"re-entry reset opacity instead of resuming");
        require(reversed.back()<reversed.front()&&IsWindowVisible(panel),"fade-out reversal failed");
        std::printf("PASS real corner route: fixed 200 DIP bounds; list fade=%zu shades, dismissal=%zu shades; reversal passed; pointer restored.\n",expanded.size(),dismissed.size());
    }
    void tick() {
        try {
            switch (stage++) {
            case 0:
                require(!IsWindowVisible(shell.viewer_window()), "viewer must start hidden");
                break;
            case 1: {
                // Settings now have their own real WinUI 3 self-test, including
                // Network Key, absence of manual performance controls, and both themes.
                // Exercise cross-thread UI marshalling without touching the real clipboard.
                std::thread update([this] { shell.update_peers(peers); shell.set_status(L"Demo devices / no network connection"); });
                update.join();
                break;
            }
            case 2:
                open_panel();
                {const int count=snapshot_requests;open_panel();require(snapshot_requests==count,"already-open panel requested another snapshot");}
                require(IsWindowVisible(panel), "Switcher did not open from tray activation");
                require(GetPropW(panel,L"HiveDesk.Composited")!=nullptr,"Switcher is not using DirectComposition");
                verify_compositor_motion();
                KillTimer(driver,1);pump(240);SetTimer(driver,1,180,nullptr);
                break;
            case 3:
                {
                    // The second peer has never been viewed. Its asynchronous
                    // snapshot must already be visible in the sidebar.
                    InvalidateRect(panel,nullptr,FALSE);UpdateWindow(panel);DwmFlush();
                    POINT sample{px(67),px(12)+2*((px(200)-2*px(12))*9/16+px(16))+px(45)};ClientToScreen(panel,&sample);
                    HDC dc=GetDC(nullptr);const auto pixel=GetPixel(dc,sample.x,sample.y);ReleaseDC(nullptr,dc);
                    if(!(std::abs(int(GetRValue(pixel))-180)<=5&&std::abs(int(GetGValue(pixel))-70)<=5&&std::abs(int(GetBValue(pixel))-40)<=5))
                        std::fprintf(stderr,"Snapshot sample RGB=%u,%u,%u visible=%d\n",GetRValue(pixel),GetGValue(pixel),GetBValue(pixel),IsWindowVisible(panel));
                    require(std::abs(int(GetRValue(pixel))-180)<=5&&std::abs(int(GetGValue(pixel))-70)<=5&&std::abs(int(GetBValue(pixel))-40)<=5,"unvisited peer snapshot not rendered");
                    require(snapshot_requests==1,"first open did not request exactly one snapshot batch");
                }
                click_card(1);
                require(selected.load() == 1, "computer card did not invoke select_peer");
                require(IsWindowVisible(shell.viewer_window()), "selected demo viewer did not open");
                capture_was_foreground = GetForegroundWindow() == shell.viewer_window();
                {
                    SetWindowPos(shell.viewer_window(),HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);DwmFlush();
                    hive::CursorState cursor;require(hive::read_cursor_shape(LoadCursorW(nullptr,IDC_IBEAM),cursor),"read test cursor");
                    shell.update_cursor("demo-studio",cursor);SendMessageW(shell.viewer_window(),WM_SETCURSOR,reinterpret_cast<WPARAM>(shell.viewer_window()),HTCLIENT);
                    hive::CursorState observed;require(hive::read_cursor_shape(GetCursor(),observed)&&observed.mask==cursor.mask&&observed.color==cursor.color,"viewer did not use remote native cursor");
                    const int input_before=input_events.load();cursor.position_revision=1;cursor.x=32768;cursor.y=32768;
                    shell.update_cursor("demo-studio",cursor);KillTimer(driver,1);pump(40);SetTimer(driver,1,180,nullptr);
                    POINT position{};GetCursorPos(&position);
                    require(std::abs(position.x-GetSystemMetrics(SM_CXSCREEN)/2)<=3&&std::abs(position.y-GetSystemMetrics(SM_CYSCREEN)/2)<=3,"host pointer position was not mapped to the viewer");
                    require(input_events.load()==input_before,"received host pointer position echoed as remote input");
                    cursor={};cursor.position_revision=1;cursor.x=32768;cursor.y=32768;cursor.visible=false;shell.update_cursor("demo-studio",cursor);require(GetCursor()==nullptr,"remote hidden cursor remained visible");
                    cursor.visible=true;shell.update_cursor("demo-studio",cursor);require(GetCursor()!=nullptr,"remote cursor did not reappear");
                    cursor.visible=false;shell.update_cursor("unselected-peer",cursor);require(GetCursor()!=nullptr,"unselected peer changed cursor");
                    SetWindowPos(shell.viewer_window(),HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
                    std::puts("PASS remote native cursor: shape, hide/show, host position mapping, no input echo, unrelated-peer rejection.");
                }
                break;
            case 4: {
                KillTimer(driver,1);SetTimer(owner,1,16,nullptr);
                HWND badge=find_own_window(L"HiveDesk.Health");require(badge!=nullptr,"health badge was not created");
                const bool foreground=GetForegroundWindow()==shell.viewer_window();
                shell.update_health("demo-studio",{1,144000,50000,hive::PressureDecode});pump(500);
                require(!IsWindowVisible(badge),"health badge flashed for a brief slowdown");
                pump(750);
                if(foreground&&GetForegroundWindow()==shell.viewer_window()) {
                    require(IsWindowVisible(badge),"sustained slowdown did not show badge");
                    require(GetWindowLongPtrW(badge,GWL_EXSTYLE)&WS_EX_NOACTIVATE,"badge can activate");
                    require(SendMessageW(badge,WM_NCHITTEST,0,0)==HTTRANSPARENT,"badge intercepts pointer input");
                    RECT bounds{};GetWindowRect(badge,&bounds);require(bounds.right-bounds.left==px(136)&&bounds.bottom-bounds.top==px(30),"badge is not compact");
                    std::puts("PASS native slowdown badge: sustained overload, compact size, no focus/input interception.");
                } else std::puts("SKIP badge foreground visibility: Windows denied test foreground.");
                shell.update_health("demo-studio",{1,144000,144000,0});pump(3200);
                require(!IsWindowVisible(badge),"badge did not clear after healthy recovery");
                KillTimer(owner,1);SetTimer(driver,1,180,nullptr);
                capture_was_foreground=GetForegroundWindow()==shell.viewer_window();
                if(capture_was_foreground)draw_demo("demo-studio");
                SendMessageW(shell.viewer_window(), WM_CLOSE, 0, 0);
                require(!IsWindowVisible(shell.viewer_window()), "return-local did not hide viewer");
                require(returned.load() > 0, "return-local callback was not invoked");
                require(GetCapture() == nullptr, "mouse capture was not released");
                const int before=snapshot_requests;open_panel();
                if(snapshot_requests!=before+1)std::fprintf(stderr,"Snapshot reopen count before=%d after=%d\n",before,snapshot_requests);
                require(snapshot_requests==before+1,"reopened sidebar did not refresh snapshots");
                require(GetCursor()==LoadCursorW(nullptr,IDC_ARROW),"local cursor was not restored");
                break;
            }
            case 5: {
                InvalidateRect(panel,nullptr,FALSE);UpdateWindow(panel);
                if (capture_was_foreground) {
                    DwmFlush();POINT sample{px(67),px(12)+(px(200)-2*px(12))*9/16+px(16)+px(45)};ClientToScreen(panel,&sample);
                    HDC dc = GetDC(nullptr); COLORREF pixel = GetPixel(dc,sample.x,sample.y); ReleaseDC(nullptr, dc);
                    const bool blue = std::abs(int(GetRValue(pixel)) - 37) <= 4 && std::abs(int(GetGValue(pixel)) - 83) <= 4 && std::abs(int(GetBValue(pixel)) - 139) <= 4;
                    require(blue, "cached Last view thumbnail does not contain the synthetic viewer pixels");
                    thumbnail_verified = true;
                }
                click_card(2);
                require(selected.load() == 2, "second peer selection did not dispatch");
                break;
            }
            case 6:
                SendMessageW(shell.viewer_window(), WM_CLOSE, 0, 0);
                open_panel();
                if(gestures){SetTimer(owner,1,16,nullptr);verify_gesture_transitions();}
                if (!preview) {
                    KillTimer(driver,1);SetTimer(owner,1,16,nullptr);
                    SendMessageW(owner,WM_HOTKEY,81,0);pump(240);
                    if(IsWindowVisible(panel)){SendMessageW(owner,WM_HOTKEY,81,0);pump(240);}
                    SetCursorPos(px(600),px(300));SendMessageW(owner,WM_HOTKEY,81,0);pump(650);
                    require(IsWindowVisible(panel),"summoned panel dismissed before pointer arrived");
                    SendMessageW(owner,WM_HOTKEY,81,0);pump(240);require(!IsWindowVisible(panel),"hotkey did not dismiss Hive");
                    SendMessageW(owner,TrayMessage,0,MAKELPARAM(WM_LBUTTONDBLCLK,1));pump(1500);
                    require(find_own_window(L"HiveDesk.Settings")||FindWindowW(nullptr,L"HiveDesk settings"),"tray double-click did not open Settings");
                    std::puts("PASS Show Hive shortcut route: persistent panel and toggle dismissal; tray double-click opens Settings.");
                    std::printf("PASS native switcher: fresh unvisited-peer snapshot, once-per-open refresh, cross-thread updates, peer selection, viewer return, capture release; cached-view thumbnail=%s. No network or remote input; local pointer restored.\n",
                        thumbnail_verified ? "verified" : "skipped (viewer could not become foreground)");
                    stop();
                } else {
                    std::printf("Native shell demo ready. Last view thumbnails are synthetic cached frames. Close via tray Quit or the HiveDesk.Owner window.\n");
                    std::fflush(stdout);
                    SetTimer(driver, 1, 180, nullptr);
                }
                break;
            default:
                if (preview && !IsWindowVisible(shell.viewer_window()) && !find_own_window(L"HiveDesk.Settings")) open_panel();
                break;
            }
        } catch (const std::exception& error) {
            failed = true; std::fprintf(stderr, "FAIL native shell: %s\n", error.what()); shell.show_local(); stop();
        }
    }
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l) {
        Smoke* self = reinterpret_cast<Smoke*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Smoke*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_TIMER && w == 1 && self) { self->tick(); return 0; }
        return DefWindowProcW(window, message, w, l);
    }
    int run() {
        struct PointerRestore {POINT point{};PointerRestore(){GetCursorPos(&point);}~PointerRestore(){SetCursorPos(point.x,point.y);}} pointer_restore;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        hive::ShellCallbacks callbacks;
        callbacks.select_peer = [this](const std::string& id) {
            ++selected;
            auto it = std::find_if(peers.begin(), peers.end(), [&](const hive::Peer& peer) { return peer.id == id; });
            shell.set_remote_size(1600, 900);
            shell.show_remote(id, it == peers.end() ? L"Demo computer" : std::wstring(it->name.begin(), it->name.end()));
            draw_demo(id);
        };
        callbacks.return_local = [this] { ++returned; };
        callbacks.panel_opened=[this] {
            ++snapshot_requests;
            hive::Thumbnail image;image.bgra.resize(size_t(hive::SnapshotWidth)*hive::SnapshotHeight*4);
            for(size_t i=0;i<image.bgra.size();i+=4){image.bgra[i]=40;image.bgra[i+1]=70;image.bgra[i+2]=180;image.bgra[i+3]=255;}
            std::thread reply([this,image=std::move(image)]() mutable {shell.update_thumbnail("demo-render",std::move(image));});reply.join();
        };
        callbacks.input = [this](const hive::InputEvent&) { ++input_events; /* Never forward input or send data. */ };
        callbacks.clipboard_changed = [](const std::wstring&) { /* Keep the user's clipboard local. */ };
        callbacks.save_settings = [this](const std::wstring& name, const std::string& key, const hive::StreamSettings& stream) {
            correct_save.store(name == L"This Computer" && key == std::string(64, '0') && stream.fps == 0 && stream.bitrate_mbps == 80); ++saved;
        };
        callbacks.connect_address = [this](const std::string&) { shell.set_status(L"Demo mode does not open network connections"); };
        std::string error;
        const bool created = shell.create(instance, {L"This Computer", std::string(64, '0'), {0, 80}}, std::move(callbacks), error);
        require(created, error.c_str());
        dpi = static_cast<int>(GetDpiForWindow(shell.viewer_window()));
        owner = find_own_window(L"HiveDesk.Owner"); panel = find_own_window(L"HiveDesk.Switcher");
        require(owner && panel, "shell native windows not found in this process");
        // Pixel/dispatch assertions must not race the pointer-leave timer. The
        // separate real-gesture phase re-enables it and exercises actual timing.
        KillTimer(owner,1);
        SetCursorPos(px(100),px(20));
        WNDCLASSW cls{}; cls.lpfnWndProc = proc; cls.hInstance = instance; cls.lpszClassName = L"HiveDesk.SmokeDriver";
        require(RegisterClassW(&cls) != 0, "could not register smoke driver");
        driver = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        require(driver != nullptr, "could not create smoke driver");
        SetTimer(driver, 1, 180, nullptr);
        int result = shell.run();
        KillTimer(driver, 1); DestroyWindow(driver); driver = nullptr;
        ReleaseCapture(); ClipCursor(nullptr);
        return failed ? 1 : result;
    }
};
}
int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        Smoke smoke;
        smoke.preview = argc > 1 && wcscmp(argv[1], L"--preview") == 0;
        smoke.gestures = argc > 1 && wcscmp(argv[1], L"--gestures") == 0;
        return smoke.run();
    } catch (const std::exception& error) {
        ReleaseCapture(); ClipCursor(nullptr);
        std::fprintf(stderr, "FAIL native shell setup: %s\n", error.what()); return 1;
    }
}
