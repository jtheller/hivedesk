#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "hivedesk/media.h"
#include "hivedesk/transport.h"
#include "hivedesk/adaptive.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
struct Winsock {
    Winsock() { WSADATA data{}; if (WSAStartup(MAKEWORD(2,2),&data)) throw std::runtime_error("Winsock initialization failed"); }
    ~Winsock() { WSACleanup(); }
};
uint16_t unused_port() {
    SOCKET socket = ::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (socket == INVALID_SOCKET) throw std::runtime_error("Cannot allocate loopback listener port");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))) { closesocket(socket); throw std::runtime_error("Cannot bind loopback port"); }
    int size = sizeof(address);
    if (getsockname(socket,reinterpret_cast<sockaddr*>(&address),&size)) { closesocket(socket); throw std::runtime_error("Cannot read loopback port"); }
    uint16_t port = ntohs(address.sin_port); closesocket(socket); return port;
}
void pump_once() {
    MSG message{};
    while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
}
bool pump_until(const std::function<bool()>& ready, Clock::time_point deadline) {
    while (!ready() && Clock::now() < deadline) pump_once();
    return ready();
}
struct Finally { std::function<void()> action; ~Finally() { action(); } };

int run(bool automatic,bool feedback_delay=false,bool background=false,bool baseline=false) {
    Winsock winsock;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{}; window_class.hInstance = instance; window_class.lpfnWndProc = DefWindowProcW; window_class.lpszClassName = L"HiveDesk.HiddenLoopback";
    if (!RegisterClassW(&window_class)) throw std::runtime_error("Cannot register hidden render window");
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,window_class.lpszClassName,L"HiveDesk single-machine loopback",WS_OVERLAPPEDWINDOW,0,0,960,540,nullptr,nullptr,instance,nullptr);
    if (!window) { UnregisterClassW(window_class.lpszClassName,instance); throw std::runtime_error("Cannot create hidden render window"); }
    HWND stimulus=automatic&&!background?CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,L"STATIC",L"HiveDesk adaptive test",WS_POPUP,32,32,240,120,nullptr,nullptr,instance,nullptr):nullptr;
    if(stimulus)ShowWindow(stimulus,SW_SHOWNOACTIVATE);
    const uint32_t source_refresh=hive::primary_refresh_millihz();
    const uint32_t target=std::min(source_refresh,59940u);
    constexpr uint64_t session=101;

    std::atomic<bool> closing{false}, started{false};
    std::atomic<uint64_t> encoded{0}, received{0}, bytes{0}, rejected{0};
    std::atomic<uint32_t> width{0}, height{0};
    std::atomic<uint32_t> negotiated{0},rate{0};std::atomic<uint32_t> diagnostic_reports{0};
    std::atomic<uint64_t> keys{0};
    std::mutex host_mutex, error_mutex;
    std::string failure;
    auto fail = [&](const std::string& text) { std::lock_guard lock(error_mutex); if (failure.empty()) failure = text; };
    auto get_failure = [&] { std::lock_guard lock(error_mutex); return failure; };
    hive::Transport host_transport, client_transport;
    hive::MediaHost host;
    hive::MediaViewer viewer;
    bool cleaned = false;
    auto cleanup = [&] {
        if (cleaned) return;
        cleaned = true; closing = true;
        // Keep the HWND owner pumping while D3D/DXGI resources are released on workers.
        std::atomic<bool> done{false};
        std::thread worker([&] {
            { std::lock_guard lock(host_mutex); host.stop(); }
            client_transport.stop(); host_transport.stop(); viewer.close(); done = true;
        });
        while (!done) pump_once();
        worker.join(); if(stimulus)DestroyWindow(stimulus);DestroyWindow(window); UnregisterClassW(window_class.lpszClassName,instance);
    };
    Finally cleanup_guard{cleanup};

    std::string error;
    if (!viewer.open(window,error)) throw std::runtime_error("Hidden viewer initialization failed: " + error);
    if (viewer.has_frame()) throw std::runtime_error("Viewer reported a frame before receiving video");
    const std::string group_key = hive::Transport::generate_group_key();
    if (group_key.empty()) throw std::runtime_error("Pairing-key generation failed");
    const uint16_t host_port = unused_port();
    hive::TransportCallbacks host_callbacks;
    host_callbacks.frame_ready=[&](const std::string&){if(!closing)host.notify_ready();};
    host_callbacks.start_stream = [&](const std::string& peer,const hive::StreamSettings& settings) {
        std::lock_guard lock(host_mutex);
        if (closing || peer != "loopback-client") return;
        if(automatic&&(settings.session!=session||settings.receiver_millihz!=59940)){fail("Automatic start negotiation was not preserved");return;}
        std::string capture_error;
        auto source_settings=settings;source_settings.diagnostic_constqp=baseline;
        bool ok = host.start(source_settings,[&,peer](hive::EncodedFrame frame) {
            if (closing) return;
            ++encoded;
            if (!host_transport.send_frame(peer,frame)) { ++rejected; host.request_keyframe(); }
        },capture_error,[&,peer]{return host_transport.frame_ready(peer);},[&,peer](const hive::StreamHealth& health){host_transport.send_health(peer,health);},[&,peer](const hive::SourceDiagnostics& d){host_transport.send_diagnostics(peer,d);});
        if (!ok) fail("Primary-screen capture failed: " + capture_error);
        else started = true;
    };
    host_callbacks.feedback=[&](const std::string&,const hive::StreamFeedback& feedback){host.feedback(feedback);};
    hive::TransportCallbacks client_callbacks;
    client_callbacks.health=[&](const std::string&,const hive::StreamHealth& health){
        if(health.session!=session||health.target_millihz!=target||health.rate_millihz>target){fail("Invalid health negotiation");return;}
        negotiated=health.target_millihz;rate=health.rate_millihz;
    };
    client_callbacks.diagnostics=[&](const std::string&,const hive::SourceDiagnostics& d){
        if(d.session!=session||!hive::valid_diagnostics(d)||!d.encode_details||!d.rate_details||d.automatic_cbr==baseline){fail("Invalid automatic rate diagnostics or baseline selection");return;}
        if(d.automatic_cbr&&!d.target_bitrate_bps){fail("Automatic CBR omitted its applied bitrate");return;}
        ++diagnostic_reports;
        std::cout<<"Rate: mode="<<(d.automatic_cbr?"auto-CBR":"CQP18 baseline")<<", target="<<d.target_bitrate_bps/1000000.0<<" Mbps, actual="<<d.actual_bitrate_bps/1000000.0<<" Mbps, QP="<<d.average_qp<<", source-discarded="<<d.source_discarded<<"\n";
    };
    client_callbacks.frame = [&](const std::string& peer,hive::EncodedFrame frame) {
        if (closing) return;
        if (peer != "loopback-host" || !frame.width || !frame.height || frame.data.empty()) { fail("Invalid authenticated video metadata"); return; }
        if(frame.session!=(automatic?session:0)){fail("Video session was not preserved");return;}
        if(frame.keyframe)++keys;
        ++received; bytes += frame.data.size(); width = frame.width; height = frame.height;
        viewer.submit(std::move(frame));
    };
    if (!host_transport.start({"loopback-host","Loopback host",group_key,host_port,false,true},std::move(host_callbacks),error)) throw std::runtime_error("Host transport failed: " + error);
    if (!client_transport.start({"loopback-client","Loopback client",group_key,0,false,true},std::move(client_callbacks),error)) throw std::runtime_error("Client transport failed: " + error);
    std::string peer;
    if (!client_transport.connect("127.0.0.1",host_port,peer,error) || peer != "loopback-host") throw std::runtime_error("Authenticated loopback connection failed: " + error);
    if (!client_transport.request_stream(peer,automatic?hive::StreamSettings{0,0,59940,session}:hive::StreamSettings{60,40})) throw std::runtime_error("Stream request was not queued");
    if (!pump_until([&] { return started || !get_failure().empty(); },Clock::now()+std::chrono::seconds(10))) throw std::runtime_error("Capture startup timed out");
    if (!get_failure().empty()) throw std::runtime_error(get_failure());

    std::string rendered_stats;
    const auto begin=Clock::now();const auto end = begin+std::chrono::seconds(automatic?9:2);
    auto last_feedback=begin,last_paint=begin;
    hive::StreamFeedback pending_feedback{};auto feedback_due=begin;
    uint32_t lowest=target,final_rate=0;uint64_t before_recovery=0;bool requested_recovery=false;
    while (Clock::now() < end && get_failure().empty()) {
        pump_once();
        const auto now=Clock::now();const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-begin).count();
        if(pending_feedback.session&&now>=feedback_due){if(!client_transport.send_feedback(peer,pending_feedback))fail("Delayed feedback was not queued");pending_feedback={};}
        if(stimulus&&now-last_paint>=std::chrono::milliseconds(16)){
            HDC dc=GetDC(stimulus);RECT rect{0,0,240,120};HBRUSH brush=CreateSolidBrush(RGB((elapsed/4)%255,40,150));FillRect(dc,&rect,brush);DeleteObject(brush);ReleaseDC(stimulus,dc);last_paint=now;
        }
        if(automatic&&now-last_feedback>=std::chrono::milliseconds(100)){
            if(background)host.request_keyframe(); // No visible stimulus; exercise even a static desktop.
            auto feedback=viewer.feedback();
            if(feedback.session){
                if(feedback_delay)feedback.service_us=1000; // Isolate feedback transport delay from decoder capacity.
                else if(elapsed>=2000&&elapsed<3000)feedback.service_us=60000;
                else if(elapsed>=3000)feedback.service_us=1000; // Explicit healthy-capacity injection for bounded recovery test.
                if(feedback_delay||elapsed>=2000)feedback.service_sample_age_us=0; // Inject a fresh measured sample, not a cached old P90.
                if(!feedback_delay&&elapsed>=2200&&!requested_recovery){before_recovery=keys.load();feedback.recover=true;requested_recovery=true;}
                if(feedback_delay){pending_feedback=feedback;feedback_due=now+std::chrono::milliseconds(elapsed>=1000?35:0);}
                else if(!client_transport.send_feedback(peer,feedback))fail("Feedback was not queued");
                // A stale session must not overwrite the active feedback or request a new IDR.
                auto stale=feedback;stale.session=session+1;stale.service_us=1000000;stale.recover=true;
                client_transport.send_feedback(peer,stale);
            }
            last_feedback=now;
        }
        if(elapsed>=2200&&elapsed<4000&&rate)lowest=std::min(lowest,rate.load());
        if(elapsed>=8500)final_rate=rate.load();
        const std::string stats = viewer.stats();
        if (stats.starts_with("Media error:")) { fail(stats); break; }
        if (viewer.has_frame()) rendered_stats = stats;
    }
    const auto measured=viewer.diagnostics();
    if(automatic&&(!diagnostic_reports||measured.session!=session||!measured.received_millihz||!measured.decoded_millihz||!measured.receive_kbps))fail("Missing live diagnostics");
    std::cout<<"Diagnostics: RX="<<measured.received_millihz/1000.0<<", decoded="<<measured.decoded_millihz/1000.0<<", presented="<<measured.presented_millihz/1000.0<<" fps; service p90="<<measured.service_p90_us/1000.0<<" ms; reports="<<diagnostic_reports<<"\n";
    std::cout<<"Transport:"<<host_transport.tcp_diagnostics("loopback-client")<<'\n';
    client_transport.stop_stream(peer);
    cleanup();
    if (viewer.stats().starts_with("Media error:")) fail(viewer.stats());
    if (!get_failure().empty()) throw std::runtime_error(get_failure());
    if (!encoded || !received || rendered_stats.empty()) throw std::runtime_error("No complete capture-to-render frame reached the hidden viewer");
    if(automatic){
        if(feedback_delay){
            if(negotiated!=target||final_rate<target*8/10)throw std::runtime_error("Feedback-only delay collapsed the cap: target="+std::to_string(target)+", final="+std::to_string(final_rate));
            std::cout<<"PASS 35 ms delayed feedback with flowing video: final cap="<<final_rate<<" mHz.\n";
        }else if(negotiated!=target||lowest>=target*8/10||final_rate<=lowest||keys<=before_recovery||!requested_recovery)throw std::runtime_error("Automatic overload/recovery/session feedback failed: target="+std::to_string(target)+", low="+std::to_string(lowest)+", final="+std::to_string(final_rate));
        if(hive::primary_refresh_millihz()!=source_refresh)throw std::runtime_error("Display refresh changed during test");
        if(!feedback_delay)std::cout<<"PASS automatic wire/session negotiation, injected decoder pressure, recovery IDR, stale-session rejection, upward recovery; target="<<target<<", low="<<lowest<<", final="<<final_rate<<" mHz; physical refresh unchanged.\n";
        if(background)std::cout<<"Background integration: periodic IDRs replace visible stimulus; these frame counts are not a throughput benchmark.\n";
    }
    std::cout << "PASS: single-machine loopback only (not a two-computer LAN latency measurement).\n"
              << "Primary-screen NVENC HEVC 4:4:4 -> authenticated negotiated video transport -> NVDEC -> hidden D3D11 renderer.\n"
              << "Encoded=" << encoded.load() << ", received=" << received.load() << ", bytes=" << bytes.load()
              << ", backpressure=" << rejected.load() << ", dimensions=" << width.load() << 'x' << height.load() << '\n'
              << rendered_stats << '\n';
    return 0;
}
}
int main(int argc,char** argv) {
    try { const std::string mode=argc>1?argv[1]:"";const bool background=mode.starts_with("--background-");return run(background||mode=="--automatic"||mode=="--feedback-delay",mode=="--feedback-delay"||mode=="--background-feedback-delay",background,mode=="--background-cqp-baseline"); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
