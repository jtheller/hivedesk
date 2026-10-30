#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include "hivedesk/transport.h"
#include "hivedesk/config.h"
#include "hivedesk/experience_trace.h"
#include "hivedesk/network_key.h"
#include "rate_wire.h"
#include "frame_timer.h"
#include "video_send_window.h"
#include "video_send_budget.h"
#include "video_write_progress.h"
#include "input_send_window.h"
#include "datagram_video.h"
#include "network_watch.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace hive { namespace {
using Bytes = std::vector<uint8_t>;
using Key = std::array<uint8_t, 32>;
using Clock = std::chrono::steady_clock;
uint64_t transport_us(Clock::time_point point=Clock::now()){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(point.time_since_epoch()).count());}
constexpr uint32_t kMaxFrame = 16 * 1024 * 1024;
constexpr uint32_t kMaxClipboard = 1024 * 1024;
constexpr uint32_t kMaxPacket = kMaxFrame + 64;
constexpr size_t kMaxConnections = 128;
// Unsent freshness is tuning; the seconds-long write watchdog is session failure
// policy. TCP cannot skip an already submitted packet halfway through.
constexpr auto kVideoQueueAge = std::chrono::milliseconds(50);
enum : uint8_t { Start = 1, Stop = 2, Frame = 3, Input = 4, ClipboardText = 5, SnapshotRequest = 6, SnapshotReply = 7, Cursor = 8,
    AutoStart=9, Feedback=10, Health=11, AutoFrame=12, Rename=13, Diagnostics=14, HostEnded=15,
    InputEnable=16, InputReady=17, InputData=18, InputComplete=19, InputRevoke=20, AutoFrameTimed=21, FrameCursor=22, BuildVersion=23, NativeSnapshotRequest=24, NativeSnapshotReply=25, WarmStart=26, WarmCancel=27, RichClipboard=28, ClipboardData=29, LivePeek=30, ProbeStart=31, ProbeReady=32, ProbeDone=33, ProbePayload=34, DatagramOffer=35, DatagramResume=36, ScopedCursor=37, ReturnStep=38, ReturnDone=39, RoutedInput=40, ViewRouteState=41, HiveSummon=42, HiveSummoned=43, MediaProbeRequest=44, MediaProbeReply=45 };
constexpr std::array<uint8_t,8> feature_marker{'H','V','C','A','P','S',0,1};

void u16(Bytes& b, uint16_t n) { b.push_back(uint8_t(n >> 8)); b.push_back(uint8_t(n)); }
void u32(Bytes& b, uint32_t n) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(n >> (i * 8))); }
void u64(Bytes& b, uint64_t n) { for (int i = 7; i >= 0; --i) b.push_back(uint8_t(n >> (i * 8))); }
void str(Bytes& b, const std::string& s) { u16(b, uint16_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
struct Reader {
    const Bytes& b; size_t at = 0; bool good = true;
    uint8_t byte() { if (at >= b.size()) { good = false; return 0; } return b[at++]; }
    uint16_t word() { uint16_t a = byte(); return uint16_t((a << 8) | byte()); }
    uint32_t dword() { uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | byte(); return n; }
    uint64_t qword() { uint64_t n = 0; for (int i = 0; i < 8; ++i) n = (n << 8) | byte(); return n; }
    std::string string(size_t max) {
        auto n = word(); if (!good || n > max || at + n > b.size()) { good = false; return {}; }
        std::string s(reinterpret_cast<const char*>(b.data() + at), n); at += n; return s;
    }
    bool end() const { return good && at == b.size(); }
};
bool random_bytes(uint8_t* p, size_t n) { return BCryptGenRandom(nullptr, p, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0; }
bool decode_key(const std::string& s, Key& key) {
    if (s.size() != 64) return false;
    auto hex = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    for (size_t i = 0; i < key.size(); ++i) { int a = hex(s[2*i]), b = hex(s[2*i+1]); if (a < 0 || b < 0) return false; key[i] = uint8_t(a*16+b); }
    return true;
}
bool derive_network_key(const std::string& text, Key& key) {
    if (!valid_network_key(text)) return false;
    // Preserve existing 32-byte keys exactly for older installations.
    if (decode_key(text,key)) return true;
    // A stable application salt lets every peer derive the same network key.
    // Derivation happens once at network startup, never on the frame/input path.
    constexpr char salt[] = "HiveDesk.NetworkKey.v1";
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG)<0) return false;
    const bool ok=BCryptDeriveKeyPBKDF2(algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),ULONG(text.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(salt)),sizeof(salt)-1,100000,key.data(),ULONG(key.size()),0)>=0;
    BCryptCloseAlgorithmProvider(algorithm,0);return ok;
}
bool hmac(const Key& key, const Bytes& input, Key& result) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, const_cast<PUCHAR>(key.data()), ULONG(key.size()), 0) >= 0;
    if (ok) ok = BCryptHashData(hash, const_cast<PUCHAR>(input.data()), ULONG(input.size()), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, result.data(), ULONG(result.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
bool equal_key(const Key& a, const Key& b) { uint8_t diff = 0; for (size_t i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i]; return diff == 0; }
struct Agreement {
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_KEY_HANDLE key{};
    ~Agreement(){if(key)BCryptDestroyKey(key);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    bool generate(Bytes& public_key){
        ULONG size=0;
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_ECDH_P256_ALGORITHM,nullptr,0)<0||
            BCryptGenerateKeyPair(algorithm,&key,256,0)<0||BCryptFinalizeKeyPair(key,0)<0)return false;
        public_key.resize(sizeof(BCRYPT_ECCKEY_BLOB)+64);
        return BCryptExportKey(key,nullptr,BCRYPT_ECCPUBLIC_BLOB,public_key.data(),ULONG(public_key.size()),&size,0)>=0&&size==public_key.size();
    }
    bool derive(const Bytes& public_key,Key& shared){
        if(public_key.size()!=sizeof(BCRYPT_ECCKEY_BLOB)+64)return false;
        BCRYPT_ECCKEY_BLOB header{};memcpy(&header,public_key.data(),sizeof(header));
        if(header.dwMagic!=BCRYPT_ECDH_PUBLIC_P256_MAGIC||header.cbKey!=32)return false;
        BCRYPT_KEY_HANDLE peer{};BCRYPT_SECRET_HANDLE secret{};ULONG size=0;
        BCryptBuffer buffer{sizeof(BCRYPT_SHA256_ALGORITHM),KDF_HASH_ALGORITHM,const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM)};
        BCryptBufferDesc parameters{BCRYPTBUFFER_VERSION,1,&buffer};
        bool ok=BCryptImportKeyPair(algorithm,nullptr,BCRYPT_ECCPUBLIC_BLOB,&peer,const_cast<PUCHAR>(public_key.data()),ULONG(public_key.size()),0)>=0&&
            BCryptSecretAgreement(key,peer,&secret,0)>=0&&BCryptDeriveKey(secret,BCRYPT_KDF_HASH,&parameters,shared.data(),ULONG(shared.size()),&size,0)>=0&&size==shared.size();
        if(secret)BCryptDestroySecret(secret);if(peer)BCryptDestroyKey(peer);return ok;
    }
};
struct LanAddress {uint32_t address,mask;auto operator<=>(const LanAddress&)const=default;};
std::vector<LanAddress> lan_addresses(){
    ULONG size=16384;std::vector<uint8_t> storage(size);
    ULONG result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()),&size);
    if(result==ERROR_BUFFER_OVERFLOW){storage.resize(size);result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()),&size);}
    std::vector<LanAddress> addresses;if(result!=NO_ERROR)return addresses;
    for(auto* adapter=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());adapter;adapter=adapter->Next){
        if(adapter->OperStatus!=IfOperStatusUp||adapter->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;
        for(auto* unicast=adapter->FirstUnicastAddress;unicast;unicast=unicast->Next){
            if(!unicast->Address.lpSockaddr||unicast->Address.lpSockaddr->sa_family!=AF_INET||unicast->OnLinkPrefixLength==0||unicast->OnLinkPrefixLength>32)continue;
            const auto ip=ntohl(reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr.s_addr);
            addresses.push_back({ip,0xffffffffu<<(32-unicast->OnLinkPrefixLength)});
        }
    }std::sort(addresses.begin(),addresses.end());return addresses;
}
bool on_link(const std::string& address){
    IN_ADDR parsed{};if(inet_pton(AF_INET,address.c_str(),&parsed)!=1)return false;
    const auto ip=ntohl(parsed.s_addr);if((ip>>24)==127)return true;
    for(const auto& local:lan_addresses())if((local.address&local.mask)==(ip&local.mask))return true;
    return false;
}
bool text_ok(const std::string& s, size_t limit) { return !s.empty() && s.size() <= limit && s.find('\0') == std::string::npos; }
bool utf16_ok(const std::wstring& s) {
    if (s.size() > kMaxClipboard / 2) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        auto c = uint16_t(s[i]); if (!c) return false;
        if (c >= 0xd800 && c <= 0xdbff) { if (++i == s.size()) return false; c = uint16_t(s[i]); if (c < 0xdc00 || c > 0xdfff) return false; }
        else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
bool valid_input(const InputEvent& e) {
    switch (e.kind) {
    case InputKind::Key: return e.a >= 0 && e.a <= 255 && (e.b == 0 || e.b == 1) && (e.c == 0 || e.c == 1);
    case InputKind::Absolute: return e.a >= 0 && e.a <= 65535 && e.b >= 0 && e.b <= 65535;
    case InputKind::Relative: return true;
    case InputKind::Button: return e.a >= 1 && e.a <= 5 && (e.b == 0 || e.b == 1);
    case InputKind::Wheel: return e.b == 0 || e.b == 1;
    case InputKind::ReleaseAll: return true;
    default: return false;
    }
}
Bytes input_bytes(const InputPacket& packet) {
    Bytes b{packet.path_size?RoutedInput:InputData};u64(b,packet.session);u64(b,packet.sequence);u64(b,packet.capture_time_us);
    u32(b,uint32_t(packet.event.kind));u32(b,uint32_t(packet.event.a));u32(b,uint32_t(packet.event.b));u32(b,uint32_t(packet.event.c));if(packet.path_size){b.push_back(packet.path_size);for(unsigned i=0;i<packet.path_size;++i)u64(b,packet.path[i]);}return b;
}
bool send_all(SOCKET s, const uint8_t* p, size_t n) {
    // A blocking send can wait for the whole supplied buffer. Bound each write
    // so SO_SNDTIMEO detects stalled progress, not the total duration of a
    // multi-megabyte snapshot on a high-latency link. Wire framing is unchanged.
    while (n) { int sent = ::send(s, reinterpret_cast<const char*>(p), int((std::min)(n, size_t(16*1024))), 0); if (sent <= 0) return false; p += sent; n -= sent; }
    return true;
}
bool recv_all(SOCKET s, uint8_t* p, size_t n,std::atomic<uint64_t>* progress=nullptr,uint64_t* first_byte=nullptr) {
    // Bound per-call accounting work. recv(flags=0) returns available bytes;
    // a requested buffer size is not a promise to wait for that many bytes.
    while (n) { int received = ::recv(s, reinterpret_cast<char*>(p), int((std::min)(n, progress?size_t(16*1024):size_t(INT_MAX))), 0); if (received <= 0) return false; if(first_byte&&!*first_byte)*first_byte=transport_us(); p += received; n -= received; if(progress)progress->fetch_add(received,std::memory_order_relaxed); }
    return true;
}
bool send_blob(SOCKET s, const Bytes& b) { Bytes head; u32(head, uint32_t(b.size())); return send_all(s, head.data(), head.size()) && send_all(s, b.data(), b.size()); }
bool recv_blob(SOCKET s, Bytes& b, uint32_t max) { Bytes head(4); if (!recv_all(s, head.data(), 4)) return false; Reader r{head}; auto n = r.dword(); if (!n || n > max) return false; b.resize(n); return recv_all(s, b.data(), n); }
void socket_options(SOCKET s) { BOOL yes = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes)); DWORD timeout = 3000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)); setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)); }

struct Cipher {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    ~Cipher() { if (key) BCryptDestroyKey(key); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    bool init(const Key& material) {
        return BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) >= 0 &&
            BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, const_cast<PUCHAR>(material.data()), ULONG(material.size()), 0) >= 0;
    }
    bool crypt(bool encrypt, const Bytes& aad, const Bytes& input, Bytes& output, std::array<uint8_t,16>& tag) {
        // Each direction has a different derived key. Counter is authenticated and cannot repeat.
        std::array<uint8_t,12> nonce{}; std::copy(aad.end()-8, aad.end(), nonce.begin()+4);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth; BCRYPT_INIT_AUTH_MODE_INFO(auth);
        auth.pbNonce = nonce.data(); auth.cbNonce = ULONG(nonce.size()); auth.pbAuthData = const_cast<PUCHAR>(aad.data()); auth.cbAuthData = ULONG(aad.size()); auth.pbTag = tag.data(); auth.cbTag = ULONG(tag.size());
        output.resize(input.size()); ULONG done = 0;
        NTSTATUS status = encrypt ? BCryptEncrypt(key, const_cast<PUCHAR>(input.data()), ULONG(input.size()), &auth, nullptr, 0, output.data(), ULONG(output.size()), &done, 0)
                                  : BCryptDecrypt(key, const_cast<PUCHAR>(input.data()), ULONG(input.size()), &auth, nullptr, 0, output.data(), ULONG(output.size()), &done, 0);
        return status >= 0 && done == input.size();
    }
};

Bytes make_hello(const TransportConfig& cfg, uint8_t channel) {
    Bytes b{'H','I','V','E',uint8_t(cfg.automatic_lan?2:1),channel}; str(b, cfg.device_id); str(b, cfg.device_name); u16(b, cfg.port);
    size_t at = b.size(); b.resize(at+32); if (!random_bytes(b.data()+at, 32)) return {};
    // Version-one peers treat this as an opaque challenge. A tagged capability
    // prefix keeps their exact wire layout, with 160 fresh random challenge bits.
    // The complete challenge (including capabilities) is authenticated by HMAC.
    std::copy(feature_marker.begin(),feature_marker.end(),b.begin()+at);
    constexpr uint32_t features=FeatureDesktopAux|FeatureAutomaticStream|FeatureStreamDiagnostics|FeatureEncodeTiming|FeatureHostKick|FeatureAutoRate|FeatureDedicatedInput|FeatureFrameTiming|FeatureCursorComposition|FeatureBuildVersion|FeatureNativeSnapshot|FeatureWarmStream|FeatureRichClipboard|FeatureLivePeek|FeatureByteFeedback|FeaturePathProbe|FeatureDatagramVideo|FeatureDatagramRecovery|FeatureScopedCursor|FeatureNestedReturn|FeatureInputRelay|FeatureViewRoute|FeatureRemoteHive|FeatureMediaProbe|FeatureHostProbe;
    const auto advertised=cfg.datagram_video?features:features&~(FeatureDatagramVideo|FeatureDatagramRecovery);
    for(unsigned i=0;i<4;++i)b[at+8+i]=uint8_t(advertised>>(24-i*8));return b;
}
bool parse_hello(const Bytes& b, Peer& peer, uint8_t& channel,bool automatic=false) {
    Reader r{b}; if (r.byte() != 'H' || r.byte() != 'I' || r.byte() != 'V' || r.byte() != 'E' || r.byte() != (automatic?2:1)) return false;
    channel = r.byte(); peer.id = r.string(128); peer.name = r.string(256); peer.port = r.word();
    if(!r.good || channel>3 || !peer.port || !text_ok(peer.id,128) || !text_ok(peer.name,256) || b.size()-r.at!=32)return false;
    if(std::equal(feature_marker.begin(),feature_marker.end(),b.begin()+r.at)){
        r.at+=feature_marker.size();peer.features=r.dword();
    }
    if(channel==2&&!(peer.features&FeatureDedicatedInput))return false;
    if(channel==3&&!(peer.features&FeatureRichClipboard))return false;
    return true;
}
bool signed_part(const Key& key, const Bytes& transcript, uint8_t label, Key& signature) { Bytes b = transcript; b.push_back(label); return hmac(key,b,signature); }
}

struct Transport::Impl {
    struct Conn {
        struct ProbeReceiver {
            uint64_t id=0;Key key{};bool ready=false,done=false;uint16_t port=0;
            PathProbeResult result;uint64_t first_us=0,last_us=0;
            std::vector<bool> seen=std::vector<bool>(25000,false);
            ~ProbeReceiver(){SecureZeroMemory(key.data(),key.size());}
        };
        std::mutex probe_mutex;std::shared_ptr<ProbeReceiver> probe_receiver;
        std::thread probe_worker;std::atomic<bool> probe_finished=true;
        bool outbound=false;Impl* owner; std::atomic<SOCKET> socket{INVALID_SOCKET}; Peer peer; std::atomic<uint8_t> channel{0};
        std::atomic<bool> ready{false}, closed{false}, reader_done{false}, writer_done{true}; Cipher tx, rx; uint64_t tx_seq = 0, rx_seq = 0;
        Key confirmation{},pair_key{};uint64_t token=0,bound_control_token=0;
        std::thread reader, writer; std::mutex queue_mutex, socket_mutex; std::condition_variable queue_cv;
        std::deque<Bytes> control; size_t control_bytes = 0; bool control_inflight = false;
        struct VideoPacket { Bytes payload; Clock::time_point queued_at; };
        std::optional<VideoPacket> video; bool wait_keyframe = false;
        WSAEVENT send_complete = WSA_INVALID_EVENT; bool video_socket_configured = false;
        HANDLE video_wake = CreateEventW(nullptr,FALSE,FALSE,nullptr);
        std::atomic<unsigned> video_inflight{0};
        std::atomic<uint64_t> video_owned_bytes{0};
        std::shared_ptr<std::atomic<uint64_t>> received_bytes=std::make_shared<std::atomic<uint64_t>>(0);
        std::unique_ptr<DatagramVideo> datagram;
        std::mutex media_route_mutex; // Serialize TCP enqueue with a recovery barrier.
        std::optional<Bytes> video_resume; // queue_mutex; ordered, never freshness-expired.
        bool datagram_negotiated=false;
        uint64_t feedback_session=0,feedback_connection=0,feedback_origin_bytes=0,feedback_origin_us=0,feedback_sequence=0;
        uint64_t feedback_expected_session=0; // Last successfully queued Start; stale reports cannot replace its baseline.
        bool video_credit=true; // queue_mutex; published after WSASend ownership.
        uint64_t packet_started_us=0,packet_completed_us=0; // reader thread only
        VideoSendWindow video_send;
        InputSendWindow input_send;
        uint64_t input_receive_session=0,input_receive_sequence=0,route_queued_revision=0;
        bool input_enabled=false,input_ending=false;
        explicit Conn(Impl* p, SOCKET s) : owner(p), socket(s) {}
        std::string local_ipv4(){
            std::lock_guard lock(socket_mutex);sockaddr_in local{};int size=sizeof(local);char address[INET_ADDRSTRLEN]{};
            const auto s=socket.load();if(s==INVALID_SOCKET||getsockname(s,reinterpret_cast<sockaddr*>(&local),&size)||
                local.sin_family!=AF_INET||!inet_ntop(AF_INET,&local.sin_addr,address,sizeof(address)))return {};
            return address;
        }
        ~Conn() { close(); if (reader.joinable()) reader.join(); if (writer.joinable()) writer.join(); if(probe_worker.joinable())probe_worker.join(); if(send_complete!=WSA_INVALID_EVENT)WSACloseEvent(send_complete); if(video_wake)CloseHandle(video_wake); SecureZeroMemory(pair_key.data(),pair_key.size()); }
        void close(bool abortive = false) {
            // Publish while holding the same mutex used by queue_cv.wait(). An
            // atomic flag alone cannot prevent a notify-before-wait lost wake.
            {std::lock_guard lock(queue_mutex);closed = true; ready = false;if(datagram)datagram->stop();}
            // Do not let a socket handle be recycled while a new WSASend or its
            // result query is using it. Pending I/O is canceled by closesocket.
            {std::lock_guard lock(socket_mutex);SOCKET s = socket.exchange(INVALID_SOCKET);
                if (s != INVALID_SOCKET) {
                    // Video has no graceful-drain requirement: once its pair
                    // closes, flushing buffered old desktop frames is useless.
                    if(abortive||channel==1){linger discard{1,0};setsockopt(s,SOL_SOCKET,SO_LINGER,reinterpret_cast<const char*>(&discard),sizeof(discard));}
                    else shutdown(s, SD_BOTH);
                    closesocket(s);
                }
            }
            queue_cv.notify_all();
            if(video_wake)SetEvent(video_wake);
        }
        void notify_ready() {
            if(owner->callbacks.frame_ready){try{owner->callbacks.frame_ready(peer.id);}catch(...){owner->status("Video readiness callback failed.");}}
        }
        void request_fresh_frame() {
            if(owner->callbacks.request_keyframe){try{owner->callbacks.request_keyframe(peer.id);}catch(...){owner->status("Video keyframe callback failed.");}}
        }
        void revoke_input(uint64_t session) {
            {std::lock_guard lock(queue_mutex);if(input_send.session()!=session)return;
                input_enabled=false;input_ending=true;input_send.finish();}
            queue_cv.notify_all();
        }
        bool enqueue(Bytes b, bool keyframe = false) {
            if (!ready || closed) return false;
            std::unique_lock lock(queue_mutex);
            if(channel==0&&!b.empty()&&b[0]==ViewRouteState){
                Reader state{b};state.byte();const auto revision=state.qword();
                if(revision<=route_queued_revision)return true;
                route_queued_revision=revision;
            }
            if (channel == 1) {
                if (wait_keyframe && !keyframe) return false;
                if (video) {
                    if (!keyframe) {
                        // Discard the whole unsent tail, never a P-frame while
                        // allowing its dependents to continue. The false return
                        // asks the producer for an IDR; freeing the queue wakes it.
                        video.reset(); wait_keyframe = true;lock.unlock();notify_ready();return false;
                    }
                    // A fresh IDR can replace a queued frame without referencing it.
                }
                if (keyframe) wait_keyframe = false;
                video = VideoPacket{std::move(b),Clock::now()};
            } else {
                // HUD reports are latest-state data. Coalesce unsent reports
                // so faster diagnostics cannot build a stale control backlog.
                if(channel==0&&!b.empty()&&(b[0]==Diagnostics||b[0]==ViewRouteState)){
                    for(auto it=control.begin();it!=control.end();++it)if(!it->empty()&&(*it)[0]==b[0]){
                        control_bytes-=it->size();control.erase(it);break;
                    }
                }
                if (control.size() >= (channel==2?128u:2048u) || control_bytes + b.size() > (channel==2?16384u:4*1024*1024u)) return false;
                if(channel==0&&b.size()>=9&&(b[0]==AutoStart||b[0]==WarmStart)){
                    Reader start{b};start.byte();feedback_expected_session=start.qword();
                }
                control_bytes += b.size(); control.push_back(std::move(b));
            }
            queue_cv.notify_one(); if(channel==1&&video_wake)SetEvent(video_wake); return true;
        }
        // Caller holds queue_mutex. A missing result means the unsent chain was
        // invalidated; the caller must ask for an IDR after releasing the lock.
        std::optional<Bytes> take_video(Clock::time_point now,Clock::time_point* queued_at=nullptr) {
            auto queued=std::move(*video);video.reset();
            if(queued_at)*queued_at=queued.queued_at;
            if(now-queued.queued_at>=kVideoQueueAge){wait_keyframe=true;video_send.expired();return {};}
            return std::move(queued.payload);
        }
        struct VideoWrite {
            Bytes head,cipher;
            std::array<uint8_t,16> tag{};
            WSAOVERLAPPED operation{};
            SOCKET socket=INVALID_SOCKET;
            VideoSendWindow::Attempt attempt;
            VideoWrite(){operation.hEvent=WSACreateEvent();}
            ~VideoWrite(){if(operation.hEvent!=WSA_INVALID_EVENT)WSACloseEvent(operation.hEvent);}
        };
        bool submit_video(VideoWrite& write,const Bytes& payload,Clock::time_point queued_at) {
            write.attempt.queued_at=transport_us(queued_at);
            write.attempt.dequeued_at=transport_us();
            if(write.operation.hEvent==WSA_INVALID_EVENT||tx_seq==UINT64_MAX)return false;
            u32(write.head,uint32_t(payload.size()));u64(write.head,++tx_seq);
            const auto begin=transport_us();
            if(!tx.crypt(true,write.head,payload,write.cipher,write.tag))return false;
            write.attempt.encrypt_us=uint32_t(std::min<uint64_t>(UINT32_MAX,transport_us()-begin));
            write.attempt.wire_bytes=write.head.size()+write.cipher.size()+write.tag.size();
            std::array<WSABUF,3> buffers{{{ULONG(write.head.size()),reinterpret_cast<char*>(write.head.data())},
                {ULONG(write.cipher.size()),reinterpret_cast<char*>(write.cipher.data())},
                {ULONG(write.tag.size()),reinterpret_cast<char*>(write.tag.data())}}};
            std::lock_guard lock(socket_mutex);
            write.socket=socket.load();if(closed||write.socket==INVALID_SOCKET)return false;
            if(!video_socket_configured){
                const int zero=0;
                if(setsockopt(write.socket,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&zero),sizeof(zero)))return false;
                video_socket_configured=true;
            }
            write.attempt.submitted_at=transport_us();
            const int result=WSASend(write.socket,buffers.data(),DWORD(buffers.size()),nullptr,0,&write.operation,nullptr);
            write.attempt.submitted=result==0||WSAGetLastError()==WSA_IO_PENDING;
            return write.attempt.submitted;
        }
        void write_video_loop(std::optional<VideoTcpSample> test_sample={}) {
            // One unsent slot, then bounded overlapped writes in authenticated
            // order. On a high-RTT path the byte budget follows Windows' ISB and
            // TCP windows; low-RTT paths retain their original two writes.
            std::deque<std::unique_ptr<VideoWrite>> pending;
            struct Drain {
                Conn& conn;decltype(pending)& writes;
                ~Drain(){
                    conn.close(true);
                    for(auto& write:writes){
                        // closesocket initiates cancellation; storage remains
                        // owned until Windows actually completes each operation.
                        if(write->attempt.submitted)WaitForSingleObject(write->operation.hEvent,INFINITE);
                        write->attempt.finished_at=transport_us();conn.video_send.record(write->attempt);
                    }
                    conn.video_inflight=0;conn.video_owned_bytes=0;
                }
            } drain{*this,pending};
            VideoWriteProgress progress;VideoSendBudget budget;uint64_t next_tcp_sample=0,owned_bytes=0,next_slow_log=0;
            if(!video_wake)return;
            while(!closed&&owner->running){
                const auto sampled_at=transport_us();
                if(sampled_at>=next_tcp_sample){
                    VideoTcpSample sample;TCP_INFO_v0 info{};DWORD version=0,bytes=0;ULONG ideal=0;
                    bool tcp_observed=false;
                    if(test_sample)sample=*test_sample; // Private self-test injection; never negotiated or configured externally.
                    else {std::lock_guard lock(socket_mutex);const auto s=socket.load();
                        if(s!=INVALID_SOCKET&&!WSAIoctl(s,SIO_TCP_INFO,&version,sizeof(version),&info,sizeof(info),&bytes,nullptr,nullptr)&&
                            !idealsendbacklogquery(s,&ideal)){
                            sample={true,info.RttUs,info.MinRttUs,ideal,info.Cwnd,info.SndWnd,info.BytesInFlight};
                            tcp_observed=true;
                        }
                    }
                    budget.observe(sample);video_send.network(sample,sampled_at,uint32_t(budget.limit()),budget.bytes());
                    // Also cover read-only warm/Peek streams, which do not own
                    // the selected host telemetry endpoint. Log source-side TCP
                    // evidence for genuinely slow writes, at most once / 5 s.
                    // Receiver-side retransmit counters cannot diagnose this.
                    if(tcp_observed&&!pending.empty()&&sampled_at>=next_slow_log&&
                        sampled_at>=pending.front()->attempt.submitted_at+2000000){
                        log("Slow video TCP: peer="+peer.id+" connection="+std::to_string(token)+
                            " write_ms="+std::to_string((sampled_at-pending.front()->attempt.submitted_at)/1000)+
                            " owned_bytes="+std::to_string(owned_bytes)+" rtt_us="+std::to_string(info.RttUs)+
                            " min_rtt_us="+std::to_string(info.MinRttUs)+" cwnd="+std::to_string(info.Cwnd)+
                            " sndwnd="+std::to_string(info.SndWnd)+" flight="+std::to_string(info.BytesInFlight)+
                            " ideal="+std::to_string(ideal)+" bytes_out="+std::to_string(info.BytesOut)+
                            " bytes_retrans="+std::to_string(info.BytesRetrans)+" rto="+std::to_string(info.TimeoutEpisodes));
                        next_slow_log=sampled_at+5000000;
                    }
                    next_tcp_sample=sampled_at+250000;
                }
                // Completion events may arrive out of order. Retire in posting
                // order so an earlier failure cannot be hidden by a later send.
                while(!pending.empty()&&WaitForSingleObject(pending.front()->operation.hEvent,0)==WAIT_OBJECT_0){
                    auto& write=*pending.front();DWORD sent=0,flags=0;bool completed=false;
                    {
                        std::lock_guard lock(socket_mutex);
                        completed=!closed&&socket.load()==write.socket&&
                            WSAGetOverlappedResult(write.socket,&write.operation,&sent,FALSE,&flags)&&
                            sent==write.attempt.wire_bytes;
                    }
                    write.attempt.finished_at=transport_us();write.attempt.completed=completed;
                    owned_bytes-=write.attempt.wire_bytes;video_owned_bytes=owned_bytes;
                    video_send.record(write.attempt);pending.pop_front();video_inflight=unsigned(pending.size());
                    if(!completed)return; // Partial/error completion invalidates the whole byte stream.
                    progress.reset(transport_us());
                }
                if(!pending.empty()){
                    const auto now=transport_us();
                    if(progress.due(now)){
                        TCP_INFO_v0 info{};DWORD version=0,bytes=0;bool available=false;
                        {const std::lock_guard lock(socket_mutex);const auto s=socket.load();
                            available=s!=INVALID_SOCKET&&!WSAIoctl(s,SIO_TCP_INFO,&version,sizeof(version),&info,sizeof(info),&bytes,nullptr,nullptr);}
                        if(available)progress.observe(now,info.BytesOut,info.BytesRetrans);else progress.unavailable();
                    }
                    if(progress.expired(now)){
                        owner->status("Video send made no new TCP payload progress for 2 s; reconnecting for a fresh desktop.");return;
                    }
                }
                bool gained_credit=false;
                {
                    std::lock_guard lock(queue_mutex);
                    const bool credit=budget.admits(pending.size(),owned_bytes,1);
                    gained_credit=credit&&!video_credit;video_credit=credit;
                }
                if(gained_credit)notify_ready();
                if(pending.size()<budget.limit()){
                    std::optional<Bytes> payload;Clock::time_point queued_at{};bool taken=false;
                    {
                        std::lock_guard lock(queue_mutex);
                        if(video_resume&&budget.admits(pending.size(),owned_bytes,video_resume->size()+28)){
                            taken=true;payload=std::move(video_resume);video_resume.reset();queued_at=Clock::now();video_credit=false;
                        }else if(video&&budget.admits(pending.size(),owned_bytes,uint64_t(video->payload.size())+28)){taken=true;payload=take_video(Clock::now(),&queued_at);video_credit=false;}
                    }
                    if(taken){
                        if(!payload){
                            {std::lock_guard lock(queue_mutex);video_credit=budget.admits(pending.size(),owned_bytes,1);}
                            request_fresh_frame();notify_ready();continue;
                        }
                        // Allocate the owner before posting: no container growth
                        // or moving OVERLAPPED storage after WSASend accepted it.
                        if(pending.empty())progress.reset(transport_us());
                        pending.push_back(std::make_unique<VideoWrite>());
                        if(!submit_video(*pending.back(),*payload,queued_at))return;
                        owned_bytes+=pending.back()->attempt.wire_bytes;video_owned_bytes=owned_bytes;
                        video_inflight=unsigned(pending.size());
                        {std::lock_guard lock(queue_mutex);video_credit=budget.admits(pending.size(),owned_bytes,1);}
                        notify_ready();
                        continue;
                    }
                }
                HANDLE events[]{video_wake,pending.empty()?nullptr:pending.front()->operation.hEvent};
                DWORD timeout=INFINITE;
                if(!pending.empty()){
                    const auto now=transport_us(),wake=std::min(progress.wake_at(),next_tcp_sample);
                    timeout=wake>now?DWORD((wake-now+999)/1000):0;
                }
                const DWORD waited=WaitForMultipleObjects(pending.empty()?1:2,events,FALSE,timeout);
                if(waited==WAIT_FAILED)return;
            }
        }
        bool write_input(Bytes& head,Bytes& cipher,std::array<uint8_t,16>& tag,Clock::time_point deadline,uint64_t input_session=0,uint64_t input_sequence=0) {
            if(send_complete==WSA_INVALID_EVENT){send_complete=WSACreateEvent();if(send_complete==WSA_INVALID_EVENT)return false;}
            WSAOVERLAPPED operation{};operation.hEvent=send_complete;
            std::array<WSABUF,3> buffers{{{ULONG(head.size()),reinterpret_cast<char*>(head.data())},
                {ULONG(cipher.size()),reinterpret_cast<char*>(cipher.data())},{ULONG(tag.size()),reinterpret_cast<char*>(tag.data())}}};
            const DWORD total=DWORD(head.size()+cipher.size()+tag.size());
            SOCKET s=INVALID_SOCKET;int submitted=SOCKET_ERROR,error=0;
            {
                std::lock_guard lock(socket_mutex);s=socket.load();if(closed||s==INVALID_SOCKET)return false;
                if(Clock::now()>=deadline)return false;
                WSAResetEvent(send_complete);
                // One operation owns the complete encrypted packet (header,
                // ciphertext and tag); no three-send delayed-ACK interaction.
                if(input_sequence){std::lock_guard queue_lock(queue_mutex);const auto now=input_now_us();
                    if(input_send.expired(now))return false;input_send.submitted(input_session,input_sequence,now);experience_trace(ExperienceEvent::InputSent,input_session,input_sequence,{now});}
                submitted=WSASend(s,buffers.data(),DWORD(buffers.size()),nullptr,0,&operation,nullptr);
                if(submitted==SOCKET_ERROR)error=WSAGetLastError();
            }
            if(submitted==SOCKET_ERROR&&error!=WSA_IO_PENDING)return false;
            const auto remaining=deadline-Clock::now();
            const DWORD timeout=remaining>Clock::duration::zero()?DWORD(std::chrono::ceil<std::chrono::milliseconds>(remaining).count()):0;
            if(WaitForSingleObject(send_complete,timeout)!=WAIT_OBJECT_0){
                close(true);
                // closesocket only initiates cancellation. The event must finish
                // before stack OVERLAPPED / ciphertext / WSABUF storage is freed.
                WaitForSingleObject(send_complete,INFINITE);
                owner->status("Input send stalled for 2 s; releasing remote control.");
                return false;
            }
            DWORD sent=0,flags=0;
            {std::lock_guard lock(socket_mutex);
                if(closed||socket.load()!=s)return false;
                if(!WSAGetOverlappedResult(s,&operation,&sent,FALSE,&flags))return false;
            }
            // A partial completion must never be followed by a new packet on
            // this byte stream; write_loop aborts the pair on any failure.
            return sent==total&&Clock::now()<=deadline;
        }
        bool write_packet(const Bytes& b) {
            const auto deadline=Clock::now()+SessionStallTimeout;
            if (tx_seq == UINT64_MAX) return false;
            Bytes head; u32(head,uint32_t(b.size())); u64(head,++tx_seq);
            Bytes cipher; std::array<uint8_t,16> tag{};
            if(!tx.crypt(true,head,b,cipher,tag))return false;
            if(channel==2){
                uint64_t input_session=0,input_sequence=0;
                if(channel==2&&!b.empty()&&b[0]==InputData){Reader packet{b};packet.byte();input_session=packet.qword();input_sequence=packet.qword();}
                return write_input(head,cipher,tag,deadline,input_session,input_sequence);
            }
            SOCKET s = socket.load(); return send_all(s,head.data(),head.size()) && send_all(s,cipher.data(),cipher.size()) && send_all(s,tag.data(),tag.size());
        }
        bool read_packet(Bytes& b) {
            SOCKET s = socket.load();auto* progress=channel==1?received_bytes.get():nullptr;
            packet_started_us=packet_completed_us=0;
            Bytes head(12); if (!recv_all(s,head.data(),head.size(),progress,channel==1?&packet_started_us:nullptr)) return false;
            Reader r{head}; uint32_t n = r.dword(); uint64_t seq = r.qword();
            if (!n || n > (channel == 1 ? kMaxPacket : channel==2?128u:channel==3?ClipboardChunk+64:MaxNativeSnapshotBytes + 64) || rx_seq == UINT64_MAX || seq != rx_seq+1) return false;
            Bytes encrypted(n); std::array<uint8_t,16> tag{};
            if (!recv_all(s,encrypted.data(),encrypted.size(),progress) || !recv_all(s,tag.data(),tag.size(),progress) || !rx.crypt(false,head,encrypted,b,tag)) return false;
            if(channel==1)packet_completed_us=transport_us();
            rx_seq = seq; return true;
        }
        void write_loop() {
            if(channel==1){
                if(datagram_negotiated){
                    Bytes offer{DatagramOffer};u16(offer,datagram?datagram->port():0);
                    if(!write_packet(offer)){close();owner->ended(*this);return;}
                    if(datagram)datagram->start();
                }
                write_video_loop();if(datagram)datagram->join();owner->ended(*this);return;
            }
            if(channel==2)SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
            while (!closed && owner->running) {
                Bytes b;bool input_stalled=false; {
                    std::unique_lock lock(queue_mutex);
                    const auto writable=[&] {return closed||!owner->running||!control.empty()||(channel==2&&input_enabled&&input_send.writable());};
                    if(channel==2) {
                        const auto deadline=input_send.next_deadline();
                        if(deadline)queue_cv.wait_until(lock,Clock::time_point(std::chrono::microseconds(deadline)),writable);
                        else queue_cv.wait(lock,writable);
                        input_stalled=input_send.expired(input_now_us());
                    } else queue_cv.wait(lock,writable);
                    if (closed || !owner->running) break;
                    if(input_stalled){lock.unlock();owner->status("Input completion stalled for 2 s; releasing remote control.");close(true);break;}
                    if(!writable())continue;
                    if(channel==2&&control.empty()) {b=input_bytes(input_send.take(input_now_us()));control_inflight=true;}
                    else { b = std::move(control.front()); control.pop_front(); control_bytes -= b.size(); control_inflight = true; }
                }
                bool sent = write_packet(b);
                { std::lock_guard lock(queue_mutex); control_inflight = false; queue_cv.notify_all(); }
                if (!sent) {if(channel!=0)close(true);break;}
            }
            close();
            // A blocked receive callback must not delay pairing teardown after
            // a writer deadline. ended() is idempotent and invokes callbacks
            // only after releasing the connection map lock.
            owner->ended(*this);
        }
        void read_loop() {
            if(channel==2)SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
            Bytes b;
            while (!closed && owner->running && read_packet(b)) {
                try {
                    if(datagram&&!b.empty()&&(b[0]==Frame||b[0]==AutoFrame||b[0]==AutoFrameTimed||b[0]==FrameCursor))datagram->tcp_received();
                    if (!owner->dispatch(*this,b)) break;
                }
                catch (...) { owner->status("Remote callback failed; connection closed."); break; }
            }
            close(); owner->ended(*this);
        }
    };
    struct Pair { std::shared_ptr<Conn> control, video, input, clipboard; };
    TransportConfig cfg; TransportCallbacks callbacks; Key group{};
    std::atomic<bool> running{false}; bool winsock = false;std::atomic<uint64_t> connection_sequence{0};
    std::atomic<SOCKET> listener{INVALID_SOCKET}, discovery{INVALID_SOCKET};
    std::thread accept_thread, discovery_thread, discovery_dial_thread;
    std::mutex listener_mutex;
    std::condition_variable discovery_cv;
    std::deque<Peer> discovery_pending;
    std::atomic<bool> refresh_network{false};
    mutable std::mutex mutex; std::mutex thread_mutex, connect_mutex,clipboard_connect_mutex,config_mutex; std::vector<std::shared_ptr<Conn>> connections;
    std::map<std::string,Pair> connected;
    std::map<std::string,std::chrono::steady_clock::time_point> next_attempt;
    std::mutex sleep_mutex; std::condition_variable sleep_cv;

    mutable std::mutex route_mutex;ViewRoutes routes;
    Bytes route_bytes()const {
        std::lock_guard lock(route_mutex);const auto& route=routes.local;
        Bytes b{ViewRouteState};u64(b,route.revision);u64(b,route.session);b.push_back(route.full?1:0);b.push_back(uint8_t(route.path.size()));
        for(const auto& node:route.path)str(b,node);return b;
    }
    void publish_route(){
        const auto packet=route_bytes();
        {std::lock_guard lock(mutex);for(const auto& [id,pair]:connected)
            if(pair.control&&(pair.control->peer.features&FeatureViewRoute)&&!pair.control->enqueue(packet))pair.control->close(true);}
        if(callbacks.view_route_changed)callbacks.view_route_changed();
    }
    bool incoming_view_allowed(const std::string& peer)const {std::lock_guard lock(route_mutex);return routes.incoming_allowed(peer);}
    void status(const std::string& s) { if (callbacks.status) { try { callbacks.status(s); } catch (...) {} } }
    void ended(Conn& c) {
        if (!running) return;
        if(c.channel==3){std::lock_guard lock(mutex);auto it=connected.find(c.peer.id);if(it!=connected.end()&&it->second.clipboard.get()==&c)it->second.clipboard.reset();return;}
        bool notify = false;Peer ended_peer; {
            std::lock_guard lock(mutex); auto it = connected.find(c.peer.id);
            if(it!=connected.end()&&(it->second.control.get()==&c||it->second.video.get()==&c||it->second.input.get()==&c)) {
                if(it->second.control)it->second.control->close();
                if(it->second.clipboard)it->second.clipboard->close();
                if(it->second.video)it->second.video->close();
                if(it->second.input)it->second.input->close();
                ended_peer=c.peer;connected.erase(it);notify=true;
                {std::lock_guard route_lock(route_mutex);routes.peers.erase(c.peer.id);if(routes.parent==c.peer.id){routes.parent.clear();routes.session=0;}routes.rebuild();}
            }
        }
        if (notify) { publish_route();ended_peer.authenticated = false; if (callbacks.peer) { try { callbacks.peer(ended_peer); } catch (...) {} } status("Connection closed: " + ended_peer.name); }
    }
    bool handshake(Conn& c, bool client, uint8_t requested) {
        c.outbound=client;
        if(cfg.automatic_lan&&!on_link(c.peer.address))return false;
        SOCKET s = c.socket.load(); socket_options(s);
        // TCP probes detect a vanished cable/peer without mistaking an idle
        // desktop for failure. No new wire messages; older peers are compatible.
        tcp_keepalive keepalive{1,5000,1000};DWORD returned=0;
        if(WSAIoctl(s,SIO_KEEPALIVE_VALS,&keepalive,sizeof(keepalive),nullptr,0,&returned,nullptr,nullptr)==SOCKET_ERROR)return false;
        TransportConfig hello_config;{std::lock_guard lock(config_mutex);hello_config=cfg;}
        Bytes first, second; Key proof{}, received{}, transmit{}, receive{}; Peer peer; uint8_t channel = requested;
        if (client) {
            first = make_hello(hello_config,channel); if (first.empty() || !send_blob(s,first) || !recv_blob(s,second,512) || !parse_hello(second,peer,channel,cfg.automatic_lan) || channel != requested || peer.id == cfg.device_id) return false;
        } else {
            if (!recv_blob(s,first,512) || !parse_hello(first,peer,channel,cfg.automatic_lan) || peer.id == cfg.device_id) return false;
            second = make_hello(hello_config,channel); if (second.empty() || !send_blob(s,second)) return false;
        }
        Bytes transcript; u32(transcript,uint32_t(first.size())); transcript.insert(transcript.end(),first.begin(),first.end()); u32(transcript,uint32_t(second.size())); transcript.insert(transcript.end(),second.begin(),second.end());
        Key material=group;struct Wipe{Key& key;~Wipe(){SecureZeroMemory(key.data(),key.size());}}wipe{material};
        if(cfg.automatic_lan){
            Agreement agreement;Bytes local_public,remote_public;
            if(!agreement.generate(local_public)||!send_blob(s,local_public)||!recv_blob(s,remote_public,128)||!agreement.derive(remote_public,material))return false;
            const auto& a=client?local_public:remote_public;const auto& b=client?remote_public:local_public;
            transcript.insert(transcript.end(),a.begin(),a.end());transcript.insert(transcript.end(),b.begin(),b.end());
        }
        if((cfg.automatic_lan&&channel==1)||channel>=2){
            Key binding{};{std::lock_guard lock(mutex);auto existing=connected.find(peer.id);
                if(existing==connected.end()||!existing->second.control||!existing->second.control->ready||existing->second.control->closed||
                    (channel==2&&!(existing->second.control->peer.features&FeatureDedicatedInput))||
                    (channel==3&&!(existing->second.control->peer.features&FeatureRichClipboard)))return false;
                binding=existing->second.control->pair_key;c.bound_control_token=existing->second.control->token;}
            Key combined{};const bool ok=hmac(binding,Bytes(material.begin(),material.end()),combined);SecureZeroMemory(binding.data(),binding.size());if(!ok)return false;material=combined;SecureZeroMemory(combined.data(),combined.size());
        }
        if (!signed_part(material,transcript,1,proof)) return false;
        if (client) { if (!recv_all(s,received.data(),received.size()) || !equal_key(proof,received)) return false; }
        else if (!send_all(s,proof.data(),proof.size())) return false;
        if (!signed_part(material,transcript,2,proof)) return false;
        if (client) { if (!send_all(s,proof.data(),proof.size())) return false; }
        else if (!recv_all(s,received.data(),received.size()) || !equal_key(proof,received)) return false;
        if (!signed_part(material,transcript,3,proof)) return false;
        if (client) { if (!recv_all(s,received.data(),received.size()) || !equal_key(proof,received)) return false; }
        else c.confirmation = proof;
        if (!signed_part(material,transcript,6,c.pair_key)||!signed_part(material,transcript,client ? 4 : 5,transmit) || !signed_part(material,transcript,client ? 5 : 4,receive) || !c.tx.init(transmit) || !c.rx.init(receive)) return false;
        SecureZeroMemory(transmit.data(),transmit.size()); SecureZeroMemory(receive.data(),receive.size());
        peer.address = c.peer.address; peer.authenticated = true; c.peer = std::move(peer); c.channel = channel;
        c.datagram_negotiated=channel==1&&cfg.datagram_video&&(c.peer.features&FeatureDatagramVideo);
        if(c.datagram_negotiated){
            try{
                DatagramVideo::Hooks hooks;
                hooks.picture=[this,&c](Bytes bytes,uint64_t began,uint64_t completed){
                    if(!c.closed&&!dispatch(c,bytes,began,completed,true))c.close(true);
                };
                hooks.receive_ready=[this,&c](const Bytes& bytes){
                    if(c.closed||!callbacks.frame_credit)return true;
                    Reader frame{bytes};const auto type=frame.byte();
                    const auto session=(type==AutoFrame||type==AutoFrameTimed||type==FrameCursor)?frame.qword():0;
                    return !frame.good||callbacks.frame_credit(c.peer.id,session);
                };
                hooks.credit=[&c]{if(!c.closed)c.notify_ready();};
                hooks.keyframe=[&c]{if(!c.closed)c.request_fresh_frame();};
                hooks.status=[this,&c](const std::string& text){status(c.peer.name+": "+text);};
                const bool recovery=(c.peer.features&FeatureDatagramRecovery)!=0;
                if(recovery)hooks.resume=[&c](uint64_t epoch){
                    std::lock_guard route(c.media_route_mutex);std::lock_guard queue(c.queue_mutex);
                    if(c.closed||!c.ready||c.video_resume)return false;
                    // Submitted TCP frames stay ordered before the barrier.
                    // Drop only its unsent tail; the UDP continuation needs IDR.
                    c.video.reset();c.wait_keyframe=true;
                    Bytes barrier{DatagramResume};u64(barrier,epoch);c.video_resume=std::move(barrier);
                    if(c.video_wake)SetEvent(c.video_wake);return true;
                };
                auto datagram=std::make_unique<DatagramVideo>(c.peer.address,c.pair_key,client,c.received_bytes,std::move(hooks),c.local_ipv4(),recovery);
                {std::lock_guard lock(c.queue_mutex);if(c.closed)return false;c.datagram=std::move(datagram);}
            }catch(const std::exception& e){status(std::string("Video will use TCP: ")+e.what());}
        }
        // Idle control connections have no timeout. shutdown() wakes blocked receives at stop().
        DWORD timeout = 0; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout)); return running;
    }
    bool activate(const std::shared_ptr<Conn>& c) {
        {
            std::lock_guard lock(mutex); if (!running) return false;
            auto& pair = connected[c->peer.id]; auto& slot = c->channel == 0 ? pair.control : c->channel==1?pair.video:c->channel==2?pair.input:pair.clipboard;
            if(slot&&!slot->closed){
                // Simultaneous clipboard-lane repairs converge on the lower
                // device ID's outbound connection without disturbing desktop lanes.
                if(c->channel!=3||c->outbound!=(cfg.device_id<c->peer.id)||slot->outbound==c->outbound)return false;
                slot->close();
            }
            if (c->channel != 0 && (!pair.control || pair.control->closed)) return false;
            if(c->bound_control_token&&c->bound_control_token!=pair.control->token)return false;
            if(c->channel==0){if(pair.clipboard){pair.clipboard->close();pair.clipboard.reset();}if(pair.video){pair.video->close();pair.video.reset();}if(pair.input){pair.input->close();pair.input.reset();}}
            slot = c; c->ready = true;
            if(!c->channel){std::lock_guard route_lock(route_mutex);routes.peers.erase(c->peer.id);
                // A new control connection never inherits an old viewing lease,
                // even if its registration races the old reader's ended().
                if(routes.parent==c->peer.id){routes.parent.clear();routes.session=0;}routes.rebuild();}
        }
        if(!c->channel)publish_route();
        if (!c->channel && callbacks.peer) { try { callbacks.peer(c->peer); } catch (...) {} }
        // Keep the authenticated hello layout and its random challenge intact.
        // Older peers never receive the optional encrypted version announcement.
        if(!c->channel&&(c->peer.features&FeatureBuildVersion)){
            Bytes version{BuildVersion};str(version,HIVEDESK_VERSION);if(!c->enqueue(std::move(version)))return false;
        }
        return true;
    }
    std::shared_ptr<Conn> add(SOCKET socket, const std::string& address) {
        auto c = std::make_shared<Conn>(this,socket); c->peer.address = address;c->token=++connection_sequence;
        std::lock_guard thread_lock(thread_mutex);
        std::lock_guard lock(mutex);
        // Reap completed workers so refused handshakes cannot accumulate thread handles or heap memory.
        std::erase_if(connections,[](const auto& item) {
            if (!item->closed || !item->reader_done || !item->writer_done) return false;
            if (item->reader.joinable()) item->reader.join(); if (item->writer.joinable()) item->writer.join(); return true;
        });
        if (!running || connections.size() >= kMaxConnections) { c->close(); return {}; }
        connections.push_back(c); return c;
    }
    bool open_listener() {
        std::lock_guard lock(listener_mutex);
        if(!running)return false;
        SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(s==INVALID_SOCKET)return false;
        BOOL exclusive=TRUE;setsockopt(s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=INADDR_ANY;address.sin_port=htons(cfg.port);
        if(bind(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(s,SOMAXCONN)) {closesocket(s);return false;}
        int size=sizeof(address);getsockname(s,reinterpret_cast<sockaddr*>(&address),&size);
        {std::lock_guard config_lock(config_mutex);cfg.port=ntohs(address.sin_port);}
        listener=s;refresh_network=true;return true;
    }
    void accept_loop() {
        while (running) {
            if(listener==INVALID_SOCKET&&!open_listener()) {
                std::unique_lock lock(sleep_mutex);sleep_cv.wait_for(lock,std::chrono::milliseconds(500),[&]{return !running;});continue;
            }
            sockaddr_in address{}; int size = sizeof(address); SOCKET s = accept(listener.load(),reinterpret_cast<sockaddr*>(&address),&size);
            if (s == INVALID_SOCKET) {
                {std::lock_guard lock(listener_mutex);auto failed=listener.exchange(INVALID_SOCKET);if(failed!=INVALID_SOCKET)closesocket(failed);}
                if(running){status("TCP listener interrupted; retrying.");std::unique_lock lock(sleep_mutex);sleep_cv.wait_for(lock,std::chrono::milliseconds(500),[&]{return !running;});}
                continue;
            }
            char ip[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET,&address.sin_addr,ip,sizeof(ip)); auto c = add(s,ip); if (!c) continue;
            std::lock_guard thread_lock(thread_mutex);
            if (!running) { c->close(); break; }
            c->reader = std::thread([this,c] {
                auto work = [&] {
                    if (!handshake(*c,false,0) || !activate(c) || !send_all(c->socket.load(),c->confirmation.data(),c->confirmation.size())) { c->close(); return; }
                    { std::lock_guard lock(thread_mutex); if (!running) { c->close(); return; } c->writer_done = false; c->writer = std::thread([c] { c->write_loop(); c->writer_done = true; }); }
                    c->read_loop();
                };
                work(); c->reader_done = true;
            });
        }
    }
    std::shared_ptr<Conn> dial(const std::string& address, uint16_t port, uint8_t channel, std::string& error) {
        sockaddr_in remote{}; remote.sin_family = AF_INET; remote.sin_port = htons(port);
        if (inet_pton(AF_INET,address.c_str(),&remote.sin_addr) != 1) { error = "Enter an IPv4 LAN address."; return {}; }
        if(cfg.automatic_lan&&!on_link(address)){error="Automatic LAN requires an on-link IPv4 subnet; a routed VPN address is not accepted.";return {};}
        SOCKET s = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); if (s == INVALID_SOCKET) { error = "Cannot create connection socket."; return {}; }
        auto c = add(s,address); if (!c) { error = "Transport stopped or too many connections."; return {}; }
        u_long nonblocking = 1; ioctlsocket(s,FIONBIO,&nonblocking);
        int rc = ::connect(s,reinterpret_cast<sockaddr*>(&remote),sizeof(remote));
        if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set write, except; FD_ZERO(&write); FD_ZERO(&except); FD_SET(s,&write); FD_SET(s,&except); timeval timeout{3,0};
            rc = select(0,nullptr,&write,&except,&timeout); int failure = 1; int length = sizeof(failure);
            if (rc > 0 && FD_ISSET(s,&write) && getsockopt(s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&failure),&length) == 0 && failure == 0) rc = 0; else rc = SOCKET_ERROR;
        }
        nonblocking = 0; ioctlsocket(s,FIONBIO,&nonblocking);
        if (rc != 0 || !handshake(*c,true,channel)) {
            const char* lane=channel==0?"control":channel==1?"video":channel==2?"input":"clipboard";
            error=std::string(lane)+" channel "+address+":"+std::to_string(port)+
                (rc!=0?" TCP connection failed or timed out; check reachability and Private/Domain LocalSubnet firewall scope.":
                 cfg.automatic_lan?" automatic LAN handshake failed; check both endpoints' subnet, version and logs.":" Network Key handshake failed.");
            c->close(); c->reader_done = true; return {};
        }
        if (!activate(c)) { error = "Peer channel is already connected or unavailable."; c->close(); c->reader_done = true; return {}; }
        {
            std::lock_guard lock(thread_mutex); if (!running) { c->close(); c->reader_done = true; error = "Transport stopped."; return {}; }
            c->writer_done = false; c->writer = std::thread([c] { c->write_loop(); c->writer_done = true; }); c->reader = std::thread([c] { c->read_loop(); c->reader_done = true; });
        }
        return c;
    }
    std::shared_ptr<Conn> find(const std::string& id, bool video = false) {
        std::lock_guard lock(mutex); auto it = connected.find(id); if (it == connected.end()) return {};
        auto c = video ? it->second.video : it->second.control; return c && c->ready && !c->closed ? c : nullptr;
    }
    std::shared_ptr<Conn> find_input(const std::string& id) {
        std::lock_guard lock(mutex);auto it=connected.find(id);if(it==connected.end())return {};
        auto c=it->second.input;return c&&c->ready&&!c->closed?c:nullptr;
    }
    static bool complete(const Pair& pair) {
        return pair.control&&pair.video&&pair.control->ready&&!pair.control->closed&&pair.video->ready&&!pair.video->closed&&
            (!(pair.control->peer.features&FeatureDedicatedInput)||(pair.input&&pair.input->ready&&!pair.input->closed));
    }
    bool connect(const std::string& address, uint16_t port, std::string& id, std::string& error) {
        // Discovery and the explicit IP action can target the same machine.
        // Reuse its authenticated pair instead of attempting a duplicate handshake.
        const std::lock_guard connecting(connect_mutex);
        if (!running) { error = "Transport is not running."; return false; }
        auto reuse=[&]{
            const std::lock_guard lock(mutex);
            for(const auto& [peer,pair]:connected) {
                if(complete(pair)&&
                    pair.control->peer.address==address&&pair.control->peer.port==port){id=peer;error.clear();return true;}
            }
            return false;
        };
        if(reuse())return true;
        auto control = dial(address,port,0,error); if (!control) return reuse();
        auto video = dial(address,port,1,error);
        if (!video || video->peer.id != control->peer.id) { control->close(); if (video) video->close(); return false; }
        if(control->peer.features&FeatureDedicatedInput){
            auto input=dial(address,port,2,error);
            if(!input||input->peer.id!=control->peer.id){control->close();video->close();if(input)input->close();return false;}
        }
        if(control->peer.features&FeatureRichClipboard){std::string clipboard_error;auto lane=dial(address,port,3,clipboard_error);if(!lane||lane->peer.id!=control->peer.id){if(lane)lane->close();status("Clipboard channel unavailable; desktop remains usable.");}}
        id = control->peer.id; status("Connected: " + control->peer.name); return true;
    }
#include "path_probe.inl"
    bool dispatch(Conn& c, const Bytes& b,uint64_t udp_started=0,uint64_t udp_completed=0,bool udp=false) {
        Reader r{b}; uint8_t type = r.byte();
        if(type==DatagramResume){
            const auto epoch=r.qword();
            return c.channel==1&&!udp&&(c.peer.features&FeatureDatagramRecovery)&&c.datagram&&r.end()&&c.datagram->resume_received(epoch);
        }
        if(type==DatagramOffer){
            const auto port=r.word();if(c.channel!=1||!c.datagram_negotiated||!r.end())return false;
            return !c.datagram||c.datagram->offer(port);
        }
        if((c.channel==3)!=(type==ClipboardData))return false;
        if(type==ClipboardData){if(b.size()<2||b.size()>ClipboardChunk+64)return false;if(callbacks.clipboard_packet)callbacks.clipboard_packet(c.peer.id,Bytes(b.begin()+1,b.end()));return true;}
        if(type==RichClipboard){ClipboardOffer offer;if(c.channel!=0||!(c.peer.features&FeatureRichClipboard)||!decode_clipboard_offer(Bytes(b.begin()+1,b.end()),offer))return false;if(callbacks.clipboard_offer)callbacks.clipboard_offer(c.peer.id,offer);else if(callbacks.clipboard)callbacks.clipboard(c.peer.id,offer.text);return true;}
        if ((c.channel == 1) != (type == Frame||type==AutoFrame||type==AutoFrameTimed||type==FrameCursor||type==ProbePayload)) return false;
        const bool input_message=type==InputReady||type==InputData||type==RoutedInput||type==InputComplete||type==InputRevoke;
        if((c.channel==2)!=input_message)return false;
        if(type==ProbeStart||type==ProbeReady||type==ProbeDone||type==ProbePayload)return dispatch_probe(c,type,r);
        switch (type) {
        case ViewRouteState: {
            ViewRoute route;route.revision=r.qword();route.session=r.qword();const auto full=r.byte(),count=r.byte();route.full=full!=0;
            if(!(c.peer.features&FeatureViewRoute)||!route.revision||full>1||!count||count>ViewRoutes::limit)return false;
            for(unsigned i=0;i<count;++i){auto node=r.string(128);if(!text_ok(node,128))return false;
                const bool repeat=std::find(route.path.begin(),route.path.end(),node)!=route.path.end();
                if(repeat&&i+1!=count)return false;route.path.push_back(std::move(node));}
            if(!r.end()||route.path.front()!=c.peer.id||(count>1)!=bool(route.session))return false;
            bool changed=false;
            {std::lock_guard lock(mutex);const auto it=connected.find(c.peer.id);
                if(it==connected.end()||it->second.control.get()!=&c)return true;
                std::lock_guard route_lock(route_mutex);auto& old=routes.peers[c.peer.id];
                if(route.revision<=old.revision)return true;old=std::move(route);changed=routes.rebuild();}
            if(changed)publish_route();return true;
        }
        case InputEnable: {
            const auto session=r.qword();if(!(c.peer.features&FeatureDedicatedInput)||!session||!r.end())return false;
            auto input=find_input(c.peer.id);if(!input)return false;
            // This callback follows all earlier clipboard and start work on the
            // control reader. The input reader stays closed until it completes.
            {std::lock_guard lock(input->queue_mutex);input->input_receive_session=0;input->input_receive_sequence=0;}
            const bool accepted=callbacks.input_begin&&callbacks.input_begin(c.peer.id,session);
            if(accepted){std::lock_guard lock(input->queue_mutex);if(input->closed)return false;input->input_receive_session=session;}
            Bytes ready{InputReady};u64(ready,session);ready.push_back(accepted?1:0);return input->enqueue(std::move(ready));
        }
        case InputReady: {
            const auto session=r.qword();const auto accepted=r.byte();if(!session||accepted>1||!r.end())return false;
            {std::lock_guard lock(c.queue_mutex);if(session==c.input_send.session()&&!c.input_ending){
                c.input_enabled=accepted!=0;
                // A declined warm/startup request is complete, not forever
                // pending. Promotion may retry authority for this same lease.
                if(!accepted){c.input_ending=true;c.input_send.finish();}
            }}
            c.queue_cv.notify_all();return true;
        }
        case InputData: case RoutedInput: {
            InputPacket packet;packet.session=r.qword();packet.sequence=r.qword();packet.capture_time_us=r.qword();
            packet.event.kind=InputKind(r.dword());packet.event.a=int32_t(r.dword());packet.event.b=int32_t(r.dword());packet.event.c=int32_t(r.dword());
            if(type==RoutedInput){
                packet.path_size=r.byte();if(!(c.peer.features&FeatureInputRelay)||!packet.path_size||packet.path_size>packet.path.size())return false;
                for(unsigned i=0;i<packet.path_size;++i){packet.path[i]=r.qword();if(!packet.path[i])return false;for(unsigned j=0;j<i;++j)if(packet.path[i]==packet.path[j])return false;}
            }
            if(!packet.session||!packet.sequence||!packet.capture_time_us||!valid_input(packet.event)||!r.end())return false;
            {std::lock_guard lock(c.queue_mutex);if(packet.session!=c.input_receive_session||packet.sequence<=c.input_receive_sequence)return true;c.input_receive_sequence=packet.sequence;}
            packet.received_time_us=input_now_us();
            if(callbacks.input_packet)callbacks.input_packet(c.peer.id,packet);return true;
        }
        case InputComplete: {
            InputAck ack;ack.session=r.qword();ack.sequence=r.qword();ack.capture_time_us=r.qword();ack.host_queue_us=r.dword();ack.inject_us=r.dword();const auto injected=r.byte();ack.injected=injected!=0;
            if(!ack.session||!ack.sequence||!ack.capture_time_us||ack.host_queue_us>10000000||ack.inject_us>10000000||injected>1||!r.end())return false;
            {std::lock_guard lock(c.queue_mutex);if(ack.session!=c.input_send.session()||c.input_ending)return true;
                if(!c.input_send.acknowledge(ack,input_now_us()))return false;
                if(c.input_ending&&c.input_send.empty())c.input_enabled=false;}
            experience_trace(ExperienceEvent::InputAck,ack.session,ack.sequence,{ack.capture_time_us,ack.host_queue_us,ack.inject_us,ack.injected?1ull:0ull});
            c.queue_cv.notify_all();if(callbacks.input_ack)callbacks.input_ack(c.peer.id,ack);
            if(!ack.injected){status("Desktop input was not accepted; reconnecting remote control.");return false;}return true;
        }
        case InputRevoke: {
            const auto session=r.qword();if(!session||!r.end())return false;
            c.revoke_input(session);if(callbacks.host_ended)callbacks.host_ended(c.peer.id,session);return true;
        }
        case Rename:{
            auto name=r.string(256);if(!cfg.automatic_lan||!text_ok(name,256)||!r.end())return false;
            Peer updated;{std::lock_guard lock(mutex);c.peer.name=std::move(name);updated=c.peer;}
            if(callbacks.peer)callbacks.peer(updated);return true;
        }
        case BuildVersion:{
            auto version=r.string(32);
            if(!(c.peer.features&FeatureBuildVersion)||!r.end()||version.empty()||
                !std::all_of(version.begin(),version.end(),[](unsigned char ch){return (ch>='0'&&ch<='9')||(ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||ch=='.'||ch=='-'||ch=='+';}))return false;
            Peer updated;{std::lock_guard lock(mutex);c.peer.version=std::move(version);updated=c.peer;}
            if(callbacks.peer)callbacks.peer(updated);return true;
        }
        case WarmCancel: {
            const auto session=r.qword();
            if(!(c.peer.features&FeatureWarmStream)||!session||!r.end())return false;
            if(callbacks.cancel_warm)callbacks.cancel_warm(c.peer.id,session);return true;
        }
        case LivePeek: {
            const auto session=r.qword();const auto enabled=r.byte();
            if(!(c.peer.features&FeatureLivePeek)||!session||enabled>1||!r.end())return false;
            if(enabled&&!incoming_view_allowed(c.peer.id)){Bytes ended{HostEnded};u64(ended,session);return c.enqueue(std::move(ended));}
            if(callbacks.preview)callbacks.preview(c.peer.id,session,enabled!=0);return true;
        }
        case WarmStart:
        case AutoStart: {
            StreamSettings settings;settings.session=r.qword();settings.receiver_millihz=r.dword();settings.warm=type==WarmStart;
            if(settings.warm&&!(c.peer.features&FeatureWarmStream))return false;
            if(!(c.peer.features&FeatureAutomaticStream)||!settings.session||settings.receiver_millihz<1000||settings.receiver_millihz>2000000||!r.end())return false;
            if(!settings.warm&&!incoming_view_allowed(c.peer.id)){Bytes ended{HostEnded};u64(ended,settings.session);return c.enqueue(std::move(ended));}
            if(callbacks.start_stream)callbacks.start_stream(c.peer.id,settings);return true;
        }
        case Feedback: {
            StreamFeedback f;f.session=r.qword();f.capture_time_us=r.qword();f.arrival_age_us=r.dword();f.service_us=r.dword();f.queue_us=r.dword();const auto recover=r.byte();f.recover=recover!=0;
            if(!r.end()&&(!(c.peer.features&FeatureAutoRate)||!rate_wire::read_feedback(f,[&]{return r.dword();},[&]{return r.qword();})))return false;
            if(!(c.peer.features&FeatureAutomaticStream)||!valid_feedback(f)||recover>1||!r.end()||(f.transport_details&&!(c.peer.features&FeatureByteFeedback)))return false;
            if(callbacks.feedback)callbacks.feedback(c.peer.id,f);return true;
        }
        case Health: {
            StreamHealth h;h.session=r.qword();h.target_millihz=r.dword();h.rate_millihz=r.dword();h.pressure=r.dword();
            if(!(c.peer.features&FeatureAutomaticStream)||!h.session||h.target_millihz<1000||h.target_millihz>2000000||!h.rate_millihz||h.rate_millihz>h.target_millihz||h.pressure>7||!r.end())return false;
            if(callbacks.health)callbacks.health(c.peer.id,h);return true;
        }
        case Diagnostics: {
            SourceDiagnostics d;d.session=r.qword();d.encoded_millihz=r.dword();d.encode_us=r.dword();d.capture_wait_us=r.dword();d.send_wait_us_per_second=r.dword();d.feedback_age_us=r.dword();d.network_extra_us=r.dword();
            if(!r.end()){if(!(c.peer.features&FeatureEncodeTiming))return false;d.color_us=r.dword();d.nvenc_us=r.dword();d.encode_age_us=r.dword();d.encode_details=true;}
            if(!r.end()&&(!(c.peer.features&FeatureAutoRate)||!rate_wire::read_diagnostics(d,[&]{return r.dword();},[&]{return r.qword();})))return false;
            if(!(c.peer.features&FeatureStreamDiagnostics)||!valid_diagnostics(d)||!r.end())return false;
            if(callbacks.diagnostics)callbacks.diagnostics(c.peer.id,d);return true;
        }
        case MediaProbeRequest: {
            const auto request=r.qword();const auto mode=r.dword();
            if(!(c.peer.features&FeatureMediaProbe)||!request||mode<1||mode>4||(mode==4&&!(c.peer.features&FeatureHostProbe))||!r.end())return false;
            if(callbacks.media_probe_request)callbacks.media_probe_request(c.peer,request,mode);
            else {Bytes reply{MediaProbeReply};u64(reply,request);u32(reply,ERROR_NOT_SUPPORTED);const std::string message="Media diagnostic unavailable in this process";reply.insert(reply.end(),message.begin(),message.end());c.enqueue(std::move(reply));}
            return true;
        }
        case MediaProbeReply: {
            MediaProbeReport report;report.request=r.qword();report.result=r.dword();
            if(!(c.peer.features&FeatureMediaProbe)||!report.request||!r.good||b.size()-r.at>MaxMediaProbeReport)return false;
            report.text.assign(b.begin()+r.at,b.end());if(callbacks.media_probe_report)callbacks.media_probe_report(c.peer.id,std::move(report));return true;
        }
        case NativeSnapshotRequest:
        case SnapshotRequest: {
            const bool native=b[0]==NativeSnapshotRequest;
            if(native&&!(c.peer.features&FeatureNativeSnapshot))return false;
            const auto request=r.qword();if(!(c.peer.features&FeatureDesktopAux)||!request||!r.end())return false;
            if(callbacks.snapshot_request)callbacks.snapshot_request(c.peer.id,request,native);return true;
        }
        case NativeSnapshotReply:
        case SnapshotReply: {
            Snapshot snapshot;snapshot.request=r.qword();snapshot.native=b[0]==NativeSnapshotReply;
            if(snapshot.native&&!(c.peer.features&FeatureNativeSnapshot))return false;
            if(!(c.peer.features&FeatureDesktopAux)||!r.good||!snapshot.request||b.size()==r.at||b.size()-r.at>(snapshot.native?MaxNativeSnapshotBytes:MaxSnapshotBytes))return false;
            snapshot.jpeg.assign(b.begin()+r.at,b.end());if(callbacks.snapshot)callbacks.snapshot(c.peer.id,std::move(snapshot));return true;
        }
        case Cursor: case ScopedCursor: {
            CursorState cursor;
            if(b[0]==ScopedCursor){
                cursor.session=r.qword();cursor.input_capture_us=r.qword();const auto position=r.byte();cursor.position_valid=position!=0;
                if(!(c.peer.features&FeatureScopedCursor)||!cursor.session||position>1||!r.good)return false;
            }
            const auto visible=r.byte();cursor.visible=visible!=0;cursor.x=r.dword();cursor.y=r.dword();
            cursor.position_revision=r.qword();cursor.width=r.dword();cursor.height=r.dword();cursor.hotspot_x=r.dword();cursor.hotspot_y=r.dword();
            const auto colors=r.dword(),mask=r.dword();
            if(!(c.peer.features&FeatureDesktopAux)||!r.good||visible>1||colors>256*256*4||mask>256*256/4||size_t(colors)+mask!=b.size()-r.at)return false;
            cursor.color.assign(b.begin()+r.at,b.begin()+r.at+colors);r.at+=colors;cursor.mask.assign(b.begin()+r.at,b.end());
            if(!valid_cursor(cursor))return false;if(callbacks.cursor)callbacks.cursor(c.peer.id,std::move(cursor));return true;
        }
        case Start: { StreamSettings settings; settings.fps = r.dword(); settings.bitrate_mbps = r.dword(); if (!r.end() || !settings.bitrate_mbps || settings.bitrate_mbps > 10000) return false; if (callbacks.start_stream) callbacks.start_stream(c.peer.id,settings); return true; }
        case Stop: if (!r.end()) return false; if (callbacks.stop_stream) callbacks.stop_stream(c.peer.id); return true;
        case ReturnStep: case ReturnDone: case HiveSummon: case HiveSummoned: {
            const bool summon=type==HiveSummon||type==HiveSummoned;
            const auto session=r.qword(),id=r.qword();const auto value=r.byte();
            if(!(c.peer.features&(summon?FeatureRemoteHive:FeatureNestedReturn))||!session||!id||!r.end())return false;
            if(summon&&find(c.peer.id).get()!=&c)return true;
            if(type==ReturnStep||type==HiveSummon){
                if(value>16)return false;
                if(callbacks.return_request)callbacks.return_request(c.peer.id,{session,id,value,summon});
            }else {
                if(value>uint8_t(ReturnResult::Timeout))return false;
                if(callbacks.return_reply)callbacks.return_reply(c.peer.id,{session,id,ReturnResult(value),summon});
            }return true;
        }
        case HostEnded: {
            const auto session=r.qword();if(!(c.peer.features&FeatureHostKick)||!session||!r.end())return false;
            if(auto input=find_input(c.peer.id))input->revoke_input(session);
            if(callbacks.host_ended)callbacks.host_ended(c.peer.id,session);return true;
        }
        case Input: { InputEvent e; e.kind = InputKind(r.dword()); e.a = int32_t(r.dword()); e.b = int32_t(r.dword()); e.c = int32_t(r.dword()); if (!r.end() || !valid_input(e)) return false; if (callbacks.input) callbacks.input(c.peer.id,e); return true; }
        case ClipboardText: {
            uint32_t count = r.dword(); if (!r.good || count > kMaxClipboard/2 || b.size()-r.at != size_t(count)*2) return false;
            std::wstring value; value.reserve(count); for (uint32_t i = 0; i < count; ++i) value.push_back(wchar_t(r.word())); if (!utf16_ok(value)) return false;
            // Callback must finish the local clipboard write before returning. Input follows on this same thread.
            if (callbacks.clipboard) callbacks.clipboard(c.peer.id,value); return true;
        }
        case Frame: case AutoFrame: case AutoFrameTimed: case FrameCursor: {
            EncodedFrame frame;
            if(type==FrameCursor){
                if(!(c.peer.features&FeatureCursorComposition))return false;
                frame.session=r.qword();if(frame.session&&!(c.peer.features&FeatureAutomaticStream))return false;
            }else if(type!=Frame){frame.session=r.qword();if(!(c.peer.features&FeatureAutomaticStream)||!frame.session)return false;}
            frame.width = r.dword(); frame.height = r.dword(); frame.sequence = r.qword(); frame.capture_time_us = r.qword(); uint8_t key = r.byte(); frame.keyframe = key != 0;
            if(type==AutoFrameTimed||type==FrameCursor){
                frame.source_ready_us=r.qword();
                if((type==AutoFrameTimed&&!frame.source_ready_us)||
                    (frame.source_ready_us&&(!frame.session||!(c.peer.features&FeatureFrameTiming)))||!valid_frame_timing(frame))return false;
            }
            if(type==FrameCursor){
                frame.cursor_composition=CursorComposition(r.byte());
                if(!valid_cursor_composition(frame.cursor_composition))return false;
            }
            if (!r.good || !frame.width || !frame.height || frame.width > 16384 || frame.height > 16384 || key > 1 || b.size()-r.at > kMaxFrame || b.size() == r.at) return false;
            frame.receive_started_us=udp?udp_started:c.packet_started_us;frame.receive_completed_us=udp?udp_completed:c.packet_completed_us;
            frame.receive_progress=c.received_bytes;
            frame.data.assign(b.begin()+r.at,b.end()); if (callbacks.frame) callbacks.frame(c.peer.id,std::move(frame)); return true;
        }
        default: return false;
        }
    }
    Bytes announcement() {
        std::lock_guard lock(config_mutex);
        Bytes b{'H','I','V','D',uint8_t(cfg.automatic_lan?2:1)}; str(b,cfg.device_id); str(b,cfg.device_name); u16(b,cfg.port);
        if(!cfg.automatic_lan){Key proof{};if(!hmac(group,b,proof))return {};b.insert(b.end(),proof.begin(),proof.end());}return b;
    }
    void discovery_dial_loop() {
        while(running){
            Peer peer;
            {std::unique_lock lock(mutex);discovery_cv.wait(lock,[&]{return !running||!discovery_pending.empty();});
                if(!running)break;
                peer=discovery_pending.front();discovery_pending.pop_front();
                const auto current=connected.find(peer.id);
                if(current!=connected.end()&&complete(current->second))continue;
            }
            std::string id,error;
            if(!connect(peer.address,peer.port,id,error)&&running)status("Discovered peer connection failed: "+peer.name+"; "+error);
        }
    }
    void discovery_loop() {
        NetworkWatch watch;watch.start();
        auto next_broadcast=Clock::now(),burst_until=next_broadcast+std::chrono::seconds(10),next_poll=next_broadcast;
        auto next_bind=next_broadcast,last_rebind=next_broadcast-std::chrono::seconds(1);
        bool rebind=true,reported_bind=false;std::vector<LanAddress> addresses;
        std::vector<SOCKET> senders;
        auto close_senders=[&]{for(auto s:senders)if(s!=INVALID_SOCKET)closesocket(s);senders.clear();};
        while(running){
            auto now=Clock::now();const bool event=watch.changed(),refresh=refresh_network.exchange(false);const bool notified=event||refresh;
            if(notified||now>=next_poll){
                auto current=lan_addresses();next_poll=now+std::chrono::seconds(5);
                if(notified||current!=addresses){
                    const bool removed=!addresses.empty()&&current!=addresses;
                    addresses=std::move(current);rebind=true;burst_until=now+std::chrono::seconds(10);next_broadcast=now;
                    {std::lock_guard lock(mutex);next_attempt.clear();discovery_pending.clear();
                        // Route/metric changes alone must not break healthy streams.
                        // Retire only sockets whose bound local address disappeared.
                        if(removed)for(const auto& c:connections){
                            if(c->closed||!c->ready)continue;
                            IN_ADDR parsed{};inet_pton(AF_INET,c->local_ipv4().c_str(),&parsed);
                            const auto local=ntohl(parsed.s_addr);
                            if(local&&(local>>24)!=127&&std::none_of(addresses.begin(),addresses.end(),[&](const auto& a){return a.address==local;}))c->close();
                        }
                    }
                }
            }
            if(rebind&&now-last_rebind>=std::chrono::milliseconds(250)){
                auto old=discovery.exchange(INVALID_SOCKET);if(old!=INVALID_SOCKET)closesocket(old);
                close_senders();last_rebind=now;next_bind=now;rebind=false;
                // Bind each announcement source to its actual interface address.
                // A wildcard send can otherwise leave through the wrong NIC.
                for(const auto& local:addresses){
                    SOCKET out=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
                    if(out==INVALID_SOCKET){senders.push_back(INVALID_SOCKET);continue;}
                    BOOL yes=TRUE;setsockopt(out,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&yes),sizeof(yes));
                    sockaddr_in bind_to{};bind_to.sin_family=AF_INET;bind_to.sin_addr.s_addr=htonl(local.address);
                    if(bind(out,reinterpret_cast<sockaddr*>(&bind_to),sizeof(bind_to))){closesocket(out);out=INVALID_SOCKET;}
                    senders.push_back(out);
                }
            }
            SOCKET s=discovery.load();
            if(s==INVALID_SOCKET&&now>=next_bind){
                s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
                BOOL yes=TRUE;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&yes),sizeof(yes));setsockopt(s,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&yes),sizeof(yes));
                sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(cfg.discovery_port);
                if(s==INVALID_SOCKET||bind(s,reinterpret_cast<sockaddr*>(&local),sizeof(local))){
                    if(s!=INVALID_SOCKET)closesocket(s);s=INVALID_SOCKET;
                    if(!reported_bind){status("LAN discovery listener unavailable; retrying every 500 ms.");reported_bind=true;}
                }else{discovery=s;reported_bind=false;next_broadcast=now;}
                next_bind=now+std::chrono::milliseconds(500);
            }
            if(s==INVALID_SOCKET){std::unique_lock lock(sleep_mutex);sleep_cv.wait_for(lock,std::chrono::milliseconds(100),[&]{return !running;});continue;}
            if(!rebind&&now>=next_broadcast&&listener!=INVALID_SOCKET){
                const auto bytes=announcement();sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(cfg.discovery_port);
                if(!bytes.empty()){
                    // Keep limited broadcast for hosts with unusual prefix/route
                    // setup; directed broadcasts cover every usable IPv4 NIC.
                    to.sin_addr.s_addr=INADDR_BROADCAST;
                    sendto(s,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to));
                    for(size_t i=0;i<addresses.size()&&i<senders.size();++i)if(senders[i]!=INVALID_SOCKET){
                        to.sin_addr.s_addr=addresses[i].mask==0xffffffffu?INADDR_BROADCAST:htonl(addresses[i].address|~addresses[i].mask);
                        sendto(senders[i],reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to));
                    }
                }
                next_broadcast=now+(now<burst_until?std::chrono::milliseconds(250):std::chrono::milliseconds(2000));
            }
            fd_set read;FD_ZERO(&read);FD_SET(s,&read);timeval timeout{0,100000};
            const auto selected=select(0,&read,nullptr,nullptr,&timeout);
            if(selected==SOCKET_ERROR){rebind=true;std::unique_lock lock(sleep_mutex);sleep_cv.wait_for(lock,std::chrono::milliseconds(50),[&]{return !running;});continue;}if(!selected)continue;
            std::array<uint8_t,1024> buffer{}; sockaddr_in remote{}; int len = sizeof(remote); int n = recvfrom(s,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&remote),&len);
            if (n < (cfg.automatic_lan?13:40) || n > 512) continue;
            Bytes b(buffer.begin(),buffer.begin()+n-(cfg.automatic_lan?0:32));
            if(!cfg.automatic_lan){Key received{},proof{};std::copy(buffer.begin()+n-32,buffer.begin()+n,received.begin());if(!hmac(group,b,proof)||!equal_key(received,proof))continue;}
            Reader r{b}; if (r.byte() != 'H' || r.byte() != 'I' || r.byte() != 'V' || r.byte() != 'D' || r.byte() != (cfg.automatic_lan?2:1)) continue;
            Peer peer; peer.id = r.string(128); peer.name = r.string(256); peer.port = r.word();
            if (!r.end() || !peer.port || !text_ok(peer.id,128) || !text_ok(peer.name,256) || peer.id == cfg.device_id || cfg.device_id > peer.id) continue;
            char ip[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET,&remote.sin_addr,ip,sizeof(ip)); peer.address = ip;
            if(cfg.automatic_lan&&!on_link(peer.address))continue;
            {
                std::lock_guard lock(mutex);
                const auto key=peer.id+"@"+peer.address;
                const auto retry=next_attempt.find(key);if(retry!=next_attempt.end()&&now<retry->second)continue;
                const auto current=connected.find(peer.id);if(current!=connected.end()&&complete(current->second))continue;
                if(next_attempt.size()>512)next_attempt.clear();
                next_attempt[key]=now+std::chrono::seconds(1);
                auto queued=std::find_if(discovery_pending.begin(),discovery_pending.end(),[&](const Peer& p){return p.id==peer.id&&p.address==peer.address;});
                if(queued!=discovery_pending.end())*queued=peer;else if(discovery_pending.size()<64)discovery_pending.push_back(peer);
            }
            discovery_cv.notify_one();
        }
        auto old=discovery.exchange(INVALID_SOCKET);if(old!=INVALID_SOCKET)closesocket(old);close_senders();
    }

};

Transport::Transport() : impl_(std::make_unique<Impl>()) {}
Transport::~Transport() { stop(); }
bool Transport::start(const TransportConfig& cfg, TransportCallbacks callbacks, std::string& error) {
    auto& p = *impl_; if (p.running) { error = "Transport already started."; return false; }
    if (!text_ok(cfg.device_id,128) || !text_ok(cfg.device_name,256) || (!cfg.automatic_lan&&!derive_network_key(cfg.group_key,p.group))) { error = "Invalid device identity or legacy Network Key."; return false; }
    WSADATA ws{}; if (WSAStartup(MAKEWORD(2,2),&ws) != 0) { error = "Winsock initialization failed."; return false; }
    p.winsock = true; p.cfg = cfg; p.callbacks = std::move(callbacks);
    {std::lock_guard lock(p.route_mutex);p.routes={};p.routes.self=cfg.device_id;p.routes.rebuild();}
    p.running=true;
    if(!p.open_listener()){
        if(!cfg.discovery){error="Cannot listen on the configured TCP port.";stop();return false;}
        p.status("TCP listener unavailable; retrying every 500 ms.");
    }
    p.accept_thread=std::thread([&p]{p.accept_loop();});
    if(cfg.discovery){
        p.discovery_dial_thread=std::thread([&p]{p.discovery_dial_loop();});
        p.discovery_thread=std::thread([&p]{p.discovery_loop();});
    }
    return true;
}
void Transport::stop() {
    auto& p = *impl_;
    if (p.running) {
        // Give explicit ReleaseAll/Stop already queued by the caller a bounded opportunity to leave.
        // One deadline for the whole transport; a slow peer cannot multiply shutdown time.
        auto deadline = std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        std::vector<std::shared_ptr<Impl::Conn>> pending;
        { std::lock_guard lock(p.mutex); pending = p.connections; }
        for (auto& c : pending) {
            if (c->channel == 1 || !c->ready || c->closed) continue;
            std::unique_lock lock(c->queue_mutex);
            c->queue_cv.wait_until(lock,deadline,[&] { return c->closed || (c->control.empty() && !c->control_inflight&&(c->channel!=2||c->input_send.empty())); });
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }
    p.running = false; p.sleep_cv.notify_all();p.discovery_cv.notify_all();
    {std::lock_guard lock(p.listener_mutex);SOCKET s=p.listener.exchange(INVALID_SOCKET);if(s!=INVALID_SOCKET){shutdown(s,SD_BOTH);closesocket(s);}}
    { std::lock_guard lock(p.mutex); for (auto& c : p.connections) c->close(); }
    if (p.accept_thread.joinable()) p.accept_thread.join(); if (p.discovery_thread.joinable()) p.discovery_thread.join();
    if(p.discovery_dial_thread.joinable())p.discovery_dial_thread.join();
    // Workers may create their writer only while holding thread_mutex and while running is true.
    { std::lock_guard lock(p.thread_mutex); }
    std::vector<std::shared_ptr<Impl::Conn>> connections; { std::lock_guard lock(p.mutex); connections = p.connections; }
    for (auto& c : connections) { c->close(); if (c->reader.joinable()) c->reader.join(); if (c->writer.joinable()) c->writer.join(); }
    { std::lock_guard lock(p.mutex); p.connected.clear(); p.connections.clear(); p.next_attempt.clear();p.discovery_pending.clear(); }
    SecureZeroMemory(p.group.data(),p.group.size()); SecureZeroMemory(p.cfg.group_key.data(),p.cfg.group_key.size()); p.cfg.group_key.clear(); if (p.winsock) { WSACleanup(); p.winsock = false; }
}
bool Transport::connect(const std::string& address, uint16_t port, std::string& peer_id, std::string& error) { return impl_->connect(address,port,peer_id,error); }
void Transport::rename(const std::string& name){
    if(!text_ok(name,256))return;
    {std::lock_guard lock(impl_->config_mutex);impl_->cfg.device_name=name;}
    if(!impl_->cfg.automatic_lan)return;
    Bytes message{Rename};str(message,name);
    std::lock_guard lock(impl_->mutex);for(const auto& [id,pair]:impl_->connected)if(pair.control)pair.control->enqueue(message);
}
std::vector<Peer> Transport::peers() const {
    std::vector<Peer> result; std::lock_guard lock(impl_->mutex); for (const auto& [id,pair] : impl_->connected) if (pair.control && pair.control->ready && !pair.control->closed) result.push_back(pair.control->peer); return result;
}
std::vector<Peer> Transport::visible_peers()const {
    auto list=peers();std::lock_guard lock(impl_->route_mutex);
    std::erase_if(list,[&](const Peer& p){return !impl_->routes.allowed(p.id);});return list;
}
bool Transport::view_allowed(const std::string& peer)const {std::lock_guard lock(impl_->route_mutex);return impl_->routes.allowed(peer);}
ViewRoute Transport::view_route()const {std::lock_guard lock(impl_->route_mutex);return impl_->routes.local;}
void Transport::set_view_parent(const std::string& peer,uint64_t session){
    bool changed;{std::lock_guard lock(impl_->route_mutex);impl_->routes.parent=session?peer:std::string{};impl_->routes.session=session;changed=impl_->routes.rebuild();}
    if(changed)impl_->publish_route();
}
uint64_t Transport::connection_token(const std::string& peer) const {
    std::lock_guard lock(impl_->mutex);const auto it=impl_->connected.find(peer);
    if(it==impl_->connected.end())return 0;const auto& pair=it->second;
    if(!Impl::complete(pair))return 0;
    return pair.control->token;
}
bool Transport::request_stream(const std::string& peer, const StreamSettings& s) {
    if(!view_allowed(peer))return false;
    if(s.warm&&!s.session)return false; // Never downgrade prewarming to legacy control Start.
    if(s.session){auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureAutomaticStream)||s.receiver_millihz<1000||s.receiver_millihz>2000000)return false;
        if(s.warm&&!(c->peer.features&FeatureWarmStream))return false;
        Bytes b{uint8_t(s.warm?WarmStart:AutoStart)};u64(b,s.session);u32(b,s.receiver_millihz);return c->enqueue(std::move(b));}
    auto c = impl_->find(peer); if (!c || !s.bitrate_mbps || s.bitrate_mbps > 10000) return false; Bytes b{Start}; u32(b,s.fps); u32(b,s.bitrate_mbps); return c->enqueue(std::move(b));
}
bool Transport::preview(const std::string& peer,uint64_t session,bool enabled){
    if(enabled&&!view_allowed(peer))return false;
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureLivePeek)||!session)return false;
    Bytes b{LivePeek};u64(b,session);b.push_back(enabled?1:0);return c->enqueue(std::move(b));
}
bool Transport::cancel_warm(const std::string& peer,uint64_t session) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureWarmStream)||!session)return false;
    Bytes b{WarmCancel};u64(b,session);return c->enqueue(std::move(b));
}
void Transport::stop_stream(const std::string& peer) { if (auto c = impl_->find(peer)) c->enqueue(Bytes{Stop}); }
bool Transport::end_stream(const std::string& peer,uint64_t session) {
    if(!session)return false;
    bool input_notified=false;
    if(auto input=impl_->find_input(peer)){
        {std::lock_guard lock(input->queue_mutex);if(input->input_receive_session==session){input->input_receive_session=0;input->input_receive_sequence=0;}}
        // The control reader may be applying a large clipboard. Revoke credits
        // on the independent input lane immediately, preserving a reciprocal
        // session instead of mistaking intentional termination for lost ACKs.
        Bytes notice{InputRevoke};u64(notice,session);input_notified=input->enqueue(std::move(notice));if(!input_notified)input->close(true);
    }
    std::shared_ptr<Impl::Conn> control,video;
    {std::lock_guard lock(impl_->mutex);const auto found=impl_->connected.find(peer);
        if(found==impl_->connected.end())return input_notified;control=found->second.control;video=found->second.video;}
    // Only this sender's queued frame can be removed; receiving video on the
    // same full-duplex connection remains untouched. Never cancel an in-flight
    // TCP packet, which would require disconnecting the reciprocal session too.
    if(video){std::lock_guard lock(video->queue_mutex);
        if(video->video){Reader r{video->video->payload};
            const auto kind=r.byte();if((kind==AutoFrame||kind==AutoFrameTimed||kind==FrameCursor)&&r.qword()==session&&r.good){video->video.reset();video->wait_keyframe=true;}}}
    if(video&&video->datagram)video->datagram->cancel(session);
    if(!control||!(control->peer.features&FeatureHostKick))return input_notified;
    Bytes b{HostEnded};u64(b,session);const bool control_notified=control->enqueue(std::move(b));return input_notified||control_notified;
}
bool Transport::send_return_request(const std::string& peer,const ReturnRequest& request) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&(request.summon?FeatureRemoteHive:FeatureNestedReturn))||!request.session||!request.id||request.hops>16)return false;
    Bytes b{uint8_t(request.summon?HiveSummon:ReturnStep)};u64(b,request.session);u64(b,request.id);b.push_back(request.hops);return c->enqueue(std::move(b));
}
bool Transport::send_return_reply(const std::string& peer,const ReturnReply& reply) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&(reply.summon?FeatureRemoteHive:FeatureNestedReturn))||!reply.session||!reply.id||uint8_t(reply.result)>uint8_t(ReturnResult::Timeout))return false;
    Bytes b{uint8_t(reply.summon?HiveSummoned:ReturnDone)};u64(b,reply.session);u64(b,reply.id);b.push_back(uint8_t(reply.result));return c->enqueue(std::move(b));
}
bool Transport::send_frame(const std::string& peer, const EncodedFrame& frame) {
    auto c = impl_->find(peer,true); if (!c || frame.data.empty() || frame.data.size() > kMaxFrame || !frame.width || !frame.height || frame.width > 16384 || frame.height > 16384) return false;
    if((frame.session&&!(c->peer.features&FeatureAutomaticStream))||!valid_frame_timing(frame)||!valid_cursor_composition(frame.cursor_composition))return false;
    const bool timed=frame.session&&frame.source_ready_us&&(c->peer.features&FeatureFrameTiming);
    const bool cursor=(c->peer.features&FeatureCursorComposition)!=0;
    Bytes b{cursor?FrameCursor:timed?AutoFrameTimed:frame.session?AutoFrame:Frame}; b.reserve(frame.data.size()+43);if(frame.session||cursor)u64(b,frame.session);
    u32(b,frame.width); u32(b,frame.height); u64(b,frame.sequence); u64(b,frame.capture_time_us); b.push_back(frame.keyframe ? 1 : 0);
    if(cursor||timed)u64(b,timed?frame.source_ready_us:0);
    if(cursor)b.push_back(uint8_t(frame.cursor_composition));
    b.insert(b.end(),frame.data.begin(),frame.data.end());
    const std::lock_guard route_lock(c->media_route_mutex);
    if(c->datagram){
        const auto route=c->datagram->route();
        if(route==DatagramVideo::Route::Datagram)return c->datagram->send(std::move(b),frame.keyframe,frame.session);
        if(route==DatagramVideo::Route::Negotiating||!c->datagram->accept_tcp(frame.keyframe))return false;
    }
    return c->enqueue(std::move(b),frame.keyframe);
}
bool Transport::frame_ready(const std::string& peer) const {
    auto c=impl_->find(peer,true);if(!c)return false;
    if(c->datagram){const auto route=c->datagram->route();if(route!=DatagramVideo::Route::Tcp)return route==DatagramVideo::Route::Datagram&&c->datagram->ready();}
    const std::lock_guard lock(c->queue_mutex);return c->ready&&!c->closed&&c->video_credit&&!c->video.has_value();
}
VideoSendDiagnostics Transport::video_send_diagnostics(const std::string& peer)const{
    const auto c=impl_->find(peer,true);if(!c)return {};
    auto result=c->video_send.snapshot(transport_us());result.connection=c->token;result.inflight=c->video_inflight.load();result.owned_bytes=c->video_owned_bytes.load();
    if(c->datagram)c->datagram->diagnostics(result);return result;
}
std::string Transport::tcp_diagnostics(const std::string& peer)const{
    std::string text;
    if(auto c=impl_->find(peer,true);c&&c->datagram)text+=c->datagram->diagnostics();
    const std::array channels{impl_->find(peer),impl_->find(peer,true),impl_->find_input(peer)};
    for(size_t channel=0;channel<channels.size();++channel){
        const auto& c=channels[channel];if(!c)continue;
        const std::lock_guard lock(c->socket_mutex);const auto socket=c->socket.load();
        TCP_INFO_v0 info{};DWORD version=0,bytes=0;
        if(socket==INVALID_SOCKET||WSAIoctl(socket,SIO_TCP_INFO,&version,sizeof(version),&info,sizeof(info),&bytes,nullptr,nullptr))continue;
        text+=" ch"+std::to_string(channel)+" rx="+std::to_string(info.BytesIn)+" tx="+std::to_string(info.BytesOut)+
            " flight="+std::to_string(info.BytesInFlight)+" rtt="+std::to_string(info.RttUs)+
            " rwnd="+std::to_string(info.RcvWnd)+" swnd="+std::to_string(info.SndWnd)+
            " retrans="+std::to_string(info.BytesRetrans)+" rto="+std::to_string(info.TimeoutEpisodes);
    }
    return text;
}
uint64_t Transport::received_video_bytes(const std::string& peer)const{
    const auto c=impl_->find(peer,true);return c?c->received_bytes->load(std::memory_order_relaxed):0;
}

bool Transport::measure_path(const std::string& peer,bool udp,uint32_t bitrate,uint32_t duration,PathProbeResult& result,std::string& error){
    result={};auto c=impl_->find(peer);
    if(!c||!(c->peer.features&FeaturePathProbe)){error="Peer does not support generated path measurements.";return false;}
    if(bitrate<100000||bitrate>20000000||duration<1000||duration>10000){error="Probe bounds: 0.1-20 Mbps, 1-10 seconds.";return false;}
    auto receiver=std::make_shared<Impl::Conn::ProbeReceiver>();
    if(!random_bytes(reinterpret_cast<uint8_t*>(&receiver->id),sizeof(receiver->id))||!receiver->id||!random_bytes(receiver->key.data(),receiver->key.size())){error="Probe nonce generation failed.";return false;}
    struct SocketOwner{SOCKET s=INVALID_SOCKET;~SocketOwner(){if(s!=INVALID_SOCKET)closesocket(s);}} socket;
    uint16_t port=0;sockaddr_in remote{};remote.sin_family=AF_INET;
    if(inet_pton(AF_INET,c->peer.address.c_str(),&remote.sin_addr)!=1){error="Probe requires the authenticated IPv4 peer address.";return false;}
    if(udp){
        socket.s=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sockaddr_in local{};local.sin_family=AF_INET;
        if(inet_pton(AF_INET,c->local_ipv4().c_str(),&local.sin_addr)!=1){error="Cannot determine the authenticated local endpoint.";return false;}
        int length=sizeof(local);DWORD timeout=30;int buffer=1024*1024;
        if(socket.s==INVALID_SOCKET||bind(socket.s,reinterpret_cast<sockaddr*>(&local),sizeof(local))||getsockname(socket.s,reinterpret_cast<sockaddr*>(&local),&length)){error="Cannot bind UDP probe receiver.";return false;}
        setsockopt(socket.s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        setsockopt(socket.s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&buffer),sizeof(buffer));port=ntohs(local.sin_port);
    }
    {std::lock_guard lock(c->probe_mutex);if(c->probe_receiver){error="A path measurement is already running.";return false;}c->probe_receiver=receiver;}
    struct Clear{std::shared_ptr<Impl::Conn> c;~Clear(){std::lock_guard lock(c->probe_mutex);c->probe_receiver.reset();}} clear{c};
    Bytes request{ProbeStart};u64(request,receiver->id);request.push_back(udp?1:0);u16(request,port);u32(request,bitrate);u32(request,duration);request.insert(request.end(),receiver->key.begin(),receiver->key.end());
    if(!c->enqueue(std::move(request))){error="Cannot queue path measurement.";return false;}
    const auto deadline=Clock::now()+std::chrono::milliseconds(duration+10000);
    bool punched=false;uint64_t done_at=0;Bytes packet(1600);
    while(impl_->running&&!c->closed&&Clock::now()<deadline){
        bool ready=false,done=false;uint64_t sent=0,count=0;
        {std::lock_guard lock(c->probe_mutex);ready=receiver->ready;done=receiver->done;remote.sin_port=htons(receiver->port);sent=receiver->result.sent_packets;count=receiver->result.received_packets;}
        if(done&&!done_at)done_at=transport_us();
        if(done&&(count>=sent||transport_us()-done_at>=2000000))break;
        if(udp&&ready&&!punched){
            // Only the authenticated peer's address is used. Reciprocal UDP
            // traffic establishes ordinary stateful firewall/NAT permission.
            for(int i=0;i<3;++i)sendto(socket.s,reinterpret_cast<const char*>(receiver->key.data()),int(receiver->key.size()),0,reinterpret_cast<sockaddr*>(&remote),sizeof(remote));punched=true;
        }
        if(udp&&ready){
            sockaddr_in from{};int size=sizeof(from);const int n=recvfrom(socket.s,reinterpret_cast<char*>(packet.data()),int(packet.size()),0,reinterpret_cast<sockaddr*>(&from),&size);
            if(n!=1244||from.sin_addr.s_addr!=remote.sin_addr.s_addr||from.sin_port!=remote.sin_port)continue;
            Bytes body(packet.begin(),packet.begin()+n-32);Key tag{},expected{};std::copy_n(packet.begin()+n-32,32,tag.begin());
            if(!hmac(receiver->key,body,expected)||!equal_key(tag,expected))continue;
            Reader read{body};if(read.qword()!=receiver->id)continue;const auto sequence=read.dword();
            std::lock_guard lock(c->probe_mutex);Impl::record_probe(*receiver,sequence,1200);
        }else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    {std::lock_guard lock(c->probe_mutex);result=receiver->result;
        result.receive_span_us=receiver->last_us>=receiver->first_us?receiver->last_us-receiver->first_us:0;
        if(!receiver->ready||!receiver->done){error="Path measurement did not finish within its deadline.";return false;}}
    return true;
}
bool Transport::send_feedback(const std::string& peer,const StreamFeedback& input) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureAutomaticStream)||!valid_feedback(input))return false;
    auto f=input;f.transport_details=false;
    if((c->peer.features&FeatureByteFeedback)&&(c->peer.features&FeatureAutoRate)){
        if(auto video=impl_->find(peer,true)){
            const auto now=transport_us(),bytes=video->received_bytes->load(std::memory_order_relaxed);
            std::lock_guard lock(c->queue_mutex);
            if(!c->feedback_expected_session||c->feedback_expected_session==f.session){
            if(c->feedback_session!=f.session||c->feedback_connection!=video->token||bytes<c->feedback_origin_bytes){
                c->feedback_session=f.session;c->feedback_connection=video->token;
                c->feedback_origin_bytes=bytes;c->feedback_origin_us=now;c->feedback_sequence=0;
            }
            f.rate_details=f.transport_details=true;
            f.feedback_sequence=++c->feedback_sequence;
            f.transport_bytes=bytes-c->feedback_origin_bytes;f.transport_sample_us=now-c->feedback_origin_us;
            }
        }
    }
    Bytes b{Feedback};u64(b,f.session);u64(b,f.capture_time_us);u32(b,f.arrival_age_us);u32(b,f.service_us);u32(b,f.queue_us);b.push_back(f.recover?1:0);
    if(f.rate_details&&(c->peer.features&FeatureAutoRate))rate_wire::write_feedback(f,[&](uint32_t n){u32(b,n);},[&](uint64_t n){u64(b,n);});
    return c->enqueue(std::move(b));
}
bool Transport::send_health(const std::string& peer,const StreamHealth& h) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureAutomaticStream)||!h.session||h.target_millihz<1000||h.target_millihz>2000000||!h.rate_millihz||h.rate_millihz>h.target_millihz||h.pressure>7)return false;
    Bytes b{Health};u64(b,h.session);u32(b,h.target_millihz);u32(b,h.rate_millihz);u32(b,h.pressure);return c->enqueue(std::move(b));
}
bool Transport::send_diagnostics(const std::string& peer,const SourceDiagnostics& d) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureStreamDiagnostics)||!valid_diagnostics(d))return false;
    Bytes b{Diagnostics};u64(b,d.session);u32(b,d.encoded_millihz);u32(b,d.encode_us);u32(b,d.capture_wait_us);u32(b,d.send_wait_us_per_second);u32(b,d.feedback_age_us);u32(b,d.network_extra_us);
    const bool rate=d.rate_details&&(c->peer.features&FeatureAutoRate)&&(c->peer.features&FeatureEncodeTiming);
    if((d.encode_details||rate)&&(c->peer.features&FeatureEncodeTiming)){u32(b,d.color_us);u32(b,d.nvenc_us);u32(b,d.encode_age_us);}
    if(rate)rate_wire::write_diagnostics(d,[&](uint32_t n){u32(b,n);},[&](uint64_t n){u64(b,n);});
    return c->enqueue(std::move(b));
}
bool Transport::send_input(const std::string& peer, const InputEvent& e) { auto c = impl_->find(peer); if (!c || !valid_input(e)) return false; Bytes b{Input}; u32(b,uint32_t(e.kind)); u32(b,uint32_t(e.a)); u32(b,uint32_t(e.b)); u32(b,uint32_t(e.c)); return c->enqueue(std::move(b)); }
bool Transport::begin_input(const std::string& peer,uint64_t session) {
    if(!session)return false;auto control=impl_->find(peer);auto input=impl_->find_input(peer);
    if(!control||!input||!(control->peer.features&FeatureDedicatedInput))return false;
    {std::lock_guard lock(input->queue_mutex);if(input->closed)return false;
        if(input->input_send.session()==session&&!input->input_ending)return true;
        input->input_send.reset(session);input->input_enabled=false;input->input_ending=false;}
    Bytes b{InputEnable};u64(b,session);const bool queued=control->enqueue(std::move(b));
    if(!queued)input->close(true);return queued;
}
bool Transport::input_ready(const std::string& peer,uint64_t session) const {
    auto input=impl_->find_input(peer);if(!input||!session)return false;
    std::lock_guard lock(input->queue_mutex);return !input->closed&&input->input_enabled&&!input->input_ending&&input->input_send.session()==session;
}
bool Transport::send_input(const std::string& peer,const InputPacket& packet) {
    if(!valid_input(packet.event)||!packet.session||!packet.sequence||!packet.capture_time_us||packet.path_size>packet.path.size())return false;
    auto input=impl_->find_input(peer);if(!input)return false;bool queued=false;
    {std::lock_guard lock(input->queue_mutex);
        if(input->closed||!input->input_enabled||input->input_ending||input->input_send.session()!=packet.session||packet.sequence<=input->input_send.last_sequence())return false;
        auto wire=packet;
        // Old endpoints terminate input locally; do not send an unknown opcode.
        if(!(input->peer.features&FeatureInputRelay)){wire.path_size=0;wire.path.fill(0);}
        queued=input->input_send.enqueue(wire,input_now_us());}
    if(queued)input->queue_cv.notify_one();
    else {impl_->status("Input edge queue exceeded its bound; releasing remote control.");input->close(true);}
    return queued;
}
bool Transport::end_input(const std::string& peer,uint64_t session) {
    auto input=impl_->find_input(peer);if(!input||!session)return false;bool queued=true;
    {std::lock_guard lock(input->queue_mutex);if(input->closed||input->input_send.session()!=session)return false;
        if(input->input_ending)return true;input->input_ending=true;
        const auto sequence=input->input_send.last_sequence();const bool release=input->input_enabled;
        input->input_enabled=false;input->input_send.finish();
        // Ended sessions never replay their unsent movement or wait for ACKs
        // from a host that has already revoked them. The final release still
        // follows the currently writing packet on the same reliable socket.
        if(release){queued=sequence!=UINT64_MAX&&input->control.size()<128;
            if(queued){auto bytes=input_bytes({session,sequence+1,input_now_us(),{InputKind::ReleaseAll}});input->control_bytes+=bytes.size();input->control.push_back(std::move(bytes));}}}
    if(queued)input->queue_cv.notify_one();else input->close(true);return queued;
}
bool Transport::send_input_ack(const std::string& peer,const InputAck& ack) {
    if(!ack.session||!ack.sequence||!ack.capture_time_us||ack.host_queue_us>10000000||ack.inject_us>10000000)return false;
    auto input=impl_->find_input(peer);if(!input)return false;
    {std::lock_guard lock(input->queue_mutex);if(input->closed||ack.session!=input->input_receive_session||ack.sequence>input->input_receive_sequence)return false;}
    Bytes b{InputComplete};u64(b,ack.session);u64(b,ack.sequence);u64(b,ack.capture_time_us);u32(b,ack.host_queue_us);u32(b,ack.inject_us);b.push_back(ack.injected?1:0);
    const bool queued=input->enqueue(std::move(b));if(!queued)input->close(true);return queued;
}
InputDiagnostics Transport::input_diagnostics(const std::string& peer) const {
    const auto input=impl_->find_input(peer);
    if(!input){InputDiagnostics legacy;legacy.available=bool(impl_->find(peer));return legacy;}
    std::lock_guard lock(input->queue_mutex);auto result=input->input_send.snapshot(input_now_us());
    result.connection=input->token;result.ready=input->input_enabled&&!input->input_ending;return result;
}
bool Transport::send_clipboard_offer(const std::string& peer,const ClipboardOffer& offer) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureRichClipboard))return false;
    auto b=encode_clipboard_offer(offer);if(b.empty())return false;b.insert(b.begin(),RichClipboard);return c->enqueue(std::move(b));
}
bool Transport::send_clipboard_packet(const std::string& peer,const std::vector<uint8_t>& bytes) {
    if(bytes.empty()||bytes.size()>ClipboardChunk+63)return false;
    std::shared_ptr<Impl::Conn> c;{std::lock_guard lock(impl_->mutex);auto it=impl_->connected.find(peer);if(it!=impl_->connected.end())c=it->second.clipboard;}
    if(!c||!c->ready||c->closed){
        std::lock_guard repairing(impl_->clipboard_connect_mutex);
        {std::lock_guard lock(impl_->mutex);auto it=impl_->connected.find(peer);if(it!=impl_->connected.end())c=it->second.clipboard;}
        if(!c||!c->ready||c->closed){auto control=impl_->find(peer);if(!control||!(control->peer.features&FeatureRichClipboard))return false;
            std::string error;c=impl_->dial(control->peer.address,control->peer.port,3,error);
            if(!c){std::lock_guard lock(impl_->mutex);auto it=impl_->connected.find(peer);if(it!=impl_->connected.end())c=it->second.clipboard;}
        }
    }
    if(!c)return false;Bytes b{ClipboardData};b.insert(b.end(),bytes.begin(),bytes.end());return c->enqueue(std::move(b));
}
bool Transport::send_clipboard(const std::string& peer, const std::wstring& value) { auto c = impl_->find(peer); if (!c || !utf16_ok(value)) return false; Bytes b{ClipboardText}; b.reserve(5+value.size()*2); u32(b,uint32_t(value.size())); for (wchar_t ch : value) u16(b,uint16_t(ch)); return c->enqueue(std::move(b)); }
bool Transport::request_media_probe(const std::string& peer,uint64_t request,uint32_t mode) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureMediaProbe)||!request||mode<1||mode>4||(mode==4&&!(c->peer.features&FeatureHostProbe)))return false;
    Bytes b{MediaProbeRequest};u64(b,request);u32(b,mode);return c->enqueue(std::move(b));
}
bool Transport::send_media_probe_report(const std::string& peer,const MediaProbeReport& report) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureMediaProbe)||!report.request||report.text.size()>MaxMediaProbeReport)return false;
    Bytes b{MediaProbeReply};u64(b,report.request);u32(b,report.result);b.insert(b.end(),report.text.begin(),report.text.end());return c->enqueue(std::move(b));
}
bool Transport::request_snapshot(const std::string& peer,uint64_t request) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureDesktopAux)||!request)return false;
    Bytes b{uint8_t((c->peer.features&FeatureNativeSnapshot)?NativeSnapshotRequest:SnapshotRequest)};u64(b,request);return c->enqueue(std::move(b));
}
bool Transport::send_snapshot(const std::string& peer,const Snapshot& snapshot) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureDesktopAux)||!snapshot.request||snapshot.jpeg.empty()||snapshot.jpeg.size()>(snapshot.native?MaxNativeSnapshotBytes:MaxSnapshotBytes)||
        (snapshot.native&&!(c->peer.features&FeatureNativeSnapshot)))return false;
    Bytes b{uint8_t(snapshot.native?NativeSnapshotReply:SnapshotReply)};u64(b,snapshot.request);b.insert(b.end(),snapshot.jpeg.begin(),snapshot.jpeg.end());return c->enqueue(std::move(b));
}
bool Transport::send_cursor(const std::string& peer,const CursorState& cursor) {
    auto c=impl_->find(peer);if(!c||!(c->peer.features&FeatureDesktopAux)||!valid_cursor(cursor))return false;
    const bool scoped=(c->peer.features&FeatureScopedCursor)&&cursor.session;
    Bytes b{uint8_t(scoped?ScopedCursor:Cursor)};
    if(scoped){u64(b,cursor.session);u64(b,cursor.input_capture_us);b.push_back(cursor.position_valid?1:0);}
    // Old viewers have no causal/session gate. Preserve shape/visibility but
    // use revision zero so they cannot warp to delayed positions during OTA.
    b.push_back(cursor.visible?1:0);u32(b,cursor.x);u32(b,cursor.y);u64(b,scoped?cursor.position_revision:0);
    u32(b,cursor.width);u32(b,cursor.height);u32(b,cursor.hotspot_x);u32(b,cursor.hotspot_y);
    u32(b,uint32_t(cursor.color.size()));u32(b,uint32_t(cursor.mask.size()));
    b.insert(b.end(),cursor.color.begin(),cursor.color.end());b.insert(b.end(),cursor.mask.begin(),cursor.mask.end());return c->enqueue(std::move(b));
}
std::string Transport::generate_group_key() {
    std::array<uint8_t,8> random{};if(!random_bytes(random.data(),random.size()))return {};
    constexpr char alphabet[]="abcdefghjkmnpqrstuvwxyz23456789";
    std::string result;result.reserve(8);
    constexpr size_t count=sizeof(alphabet)-1,limit=256/count*count;
    // Rejection sampling avoids modulo bias.
    for(auto value:random){while(value>=limit){if(!random_bytes(&value,1))return {};}result.push_back(alphabet[value%count]);}
    SecureZeroMemory(random.data(),random.size());return result;
}

bool Transport::discovery_self_test(std::string& result){
    // Broadcast on an ephemeral discovery port so generated test machines
    // never appear in the installed user's real Hive.
    WSADATA ws{};if(WSAStartup(MAKEWORD(2,2),&ws)){result="FAIL: Winsock";return false;}
    SOCKET probe=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sockaddr_in address{};address.sin_family=AF_INET;int size=sizeof(address);
    const bool bound=probe!=INVALID_SOCKET&&bind(probe,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&getsockname(probe,reinterpret_cast<sockaddr*>(&address),&size)==0;
    if(probe!=INVALID_SOCKET)closesocket(probe);WSACleanup();if(!bound){result="FAIL: isolated discovery port";return false;}
    const auto discovery_port=ntohs(address.sin_port);
    Transport a,b;std::string error;const auto suffix=std::to_string(GetCurrentProcessId());
    const std::string aid="discovery-a-"+suffix,bid="discovery-b-"+suffix;
    if(!a.start({aid,"Discovery A","",0,true,true,true,discovery_port},{},error)||!b.start({bid,"Discovery B","",0,true,true,true,discovery_port},{},error)){result="FAIL: "+error;return false;}
    auto paired=[&]{
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
      while(std::chrono::steady_clock::now()<deadline){
        const auto ap=a.peers(),bp=b.peers();
        if(std::any_of(ap.begin(),ap.end(),[&](const auto& p){return p.id==bid&&p.authenticated;})&&std::any_of(bp.begin(),bp.end(),[&](const auto& p){return p.id==aid&&p.authenticated;})){
            // Require the complete negotiated channel set, not only control/video.
            if(a.connection_token(bid)&&b.connection_token(aid)&&a.frame_ready(bid)&&b.frame_ready(aid))return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      return false;
    };
    if(!paired()){result="FAIL: initial automatic discovery did not complete.";return false;}
    const auto first_token=a.connection_token(bid);if(!first_token){result="FAIL: paired connection has no token.";return false;}
    b.stop();
    if(!b.start({bid,"Discovery B restarted","",0,true,true,true,discovery_port},{},error)||!paired()){result="FAIL: listener restart was not rediscovered while initiator stayed running: "+error;return false;}
    if(a.connection_token(bid)==first_token){result="FAIL: restart reused the old connection token.";return false;}
    a.stop();
    if(!a.start({aid,"Discovery A restarted","",0,true,true,true,discovery_port},{},error)||!paired()){result="FAIL: initiator restart was not rediscovered while listener stayed running: "+error;return false;}
    const auto broken=b.impl_->find(aid,true);if(!broken){result="FAIL: missing video channel before teardown check.";return false;}broken->close();
    if(!paired()){result="FAIL: a closed video channel left an unusable control-only peer.";return false;}
    result="PASS: automatic broadcast discovery established all three encrypted channels, rediscovered listener and initiator restarts independently, and replaced a broken video/control/input set without connect() or an IP argument.";return true;
}
bool Transport::self_test(std::string& result) {
    {
        Transport endpoint;auto& owner=*endpoint.impl_;owner.routes.self="local";owner.routes.parent="parent";owner.routes.session=99;owner.routes.rebuild();
        auto c=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);c->ready=true;c->peer.id="parent";c->peer.features=FeatureViewRoute|FeatureHostKick|FeatureAutomaticStream|FeatureLivePeek;
        owner.connected[c->peer.id]={c,{}};
        const auto wire=[](uint64_t revision,uint64_t session,std::vector<std::string> nodes){Bytes b{ViewRouteState};u64(b,revision);u64(b,session);b.push_back(0);b.push_back(uint8_t(nodes.size()));for(const auto& n:nodes)str(b,n);return b;};
        const auto valid=wire(2,88,{"parent","ancestor"});
        if(!c->enqueue(wire(10,0,{"parent"}))||!c->enqueue(wire(9,1,{"parent","stale"}))||c->control.size()!=1||c->control.back()!=wire(10,0,{"parent"})){
            result="Concurrent route publishers replaced newer state with older state";return false;
        }

        if(!owner.dispatch(*c,valid)||endpoint.view_allowed("ancestor")){result="View ancestry wire/filter failed";return false;}
        if(!owner.dispatch(*c,wire(1,0,{"parent"}))||endpoint.view_allowed("ancestor")){result="Stale view revision restored ancestor";return false;}
        for(auto bad:{wire(3,0,{"parent","ancestor"}),wire(3,1,{"wrong","ancestor"}),wire(3,1,{"parent","parent","ancestor"}),wire(0,0,{"parent"})})
            if(owner.dispatch(*c,bad)){result="Malformed view ancestry accepted";return false;}
        auto bad=valid;bad.pop_back();if(owner.dispatch(*c,bad)){result="Truncated view route accepted";return false;}
        bad=valid;bad.push_back(0);if(owner.dispatch(*c,bad)){result="Trailing view route accepted";return false;}
        bad=valid;bad[18]=65;if(owner.dispatch(*c,bad)){result="Oversized view route accepted";return false;}
        c->peer.features&=~FeatureViewRoute;if(owner.dispatch(*c,valid)){result="Unnegotiated view route accepted";return false;}c->peer.features|=FeatureViewRoute;
        if(!owner.dispatch(*c,wire(3,1,{"parent","local"}))){result="Cycle route rejected before arbitration";return false;}
        unsigned starts=0;owner.callbacks.start_stream=[&](const std::string&,const StreamSettings&){++starts;};
        Bytes start{AutoStart};u64(start,321);u32(start,60000);
        if(!owner.dispatch(*c,start)||starts||c->control.back()[0]!=HostEnded){result="Inbound view cycle bypassed admission";return false;}
        // The identical revision on a replacement connection cannot inherit
        // the old socket's authority. Dispatch must use the registered control.
        auto replacement=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);owner.connected[c->peer.id].control=replacement;
        if(!owner.dispatch(*c,wire(100,0,{"parent"}))||owner.routes.peers["parent"].revision!=3){result="Retired view route source changed authority";return false;}
        replacement->closed=true;replacement->peer.id="parent";
        auto fresh=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);fresh->peer.id="parent";fresh->peer.features=FeatureViewRoute;
        owner.running=true;
        const bool renewed=owner.activate(fresh);owner.running=false;
        if(!renewed||endpoint.view_route().session||!endpoint.view_allowed("parent")||!owner.routes.peers.empty()){
            result="New control connection inherited old ancestry/lease";return false;
        }

    }
    {
        Transport sender;auto& owner=*sender.impl_;
        auto control=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);control->ready=true;
        control->peer.id="return-peer";control->peer.features=FeatureNestedReturn;
        owner.connected[control->peer.id]={control,{}};ReturnRequest request;ReturnReply reply;
        owner.callbacks.return_request=[&](const std::string&,const ReturnRequest& r){request=r;};
        owner.callbacks.return_reply=[&](const std::string&,const ReturnReply& r){reply=r;};
        if(!sender.send_return_request("return-peer",{91,7,15})||!owner.dispatch(*control,control->control.back())||request.session!=91||request.id!=7||request.hops!=15){result="Nested return request serialization";return false;}
        const auto packet=control->control.back();auto malformed=packet;malformed.pop_back();
        if(owner.dispatch(*control,malformed)){result="Truncated nested return accepted";return false;}
        malformed=packet;malformed.back()=17;
        if(owner.dispatch(*control,malformed)){result="Unbounded nested return accepted";return false;}
        malformed=packet;std::fill(malformed.begin()+1,malformed.begin()+9,0);
        if(owner.dispatch(*control,malformed)||sender.send_return_request("return-peer",{91,0,15})){result="Invalid nested return identity accepted";return false;}
        if(!sender.send_return_reply("return-peer",{91,7,ReturnResult::Returned})||!owner.dispatch(*control,control->control.back())||reply.session!=91||reply.id!=7||reply.result!=ReturnResult::Returned){result="Nested return reply serialization";return false;}
        malformed=control->control.back();malformed.back()=6;
        if(owner.dispatch(*control,malformed)){result="Invalid nested return result accepted";return false;}
        control->peer.features=0;
        if(owner.dispatch(*control,packet)||sender.send_return_request("return-peer",{91,7,15})){result="Unnegotiated nested return accepted";return false;}
        if(sender.send_return_request("return-peer",{91,8,15,true})){result="Summon sent to legacy peer";return false;}
        control->peer.features=FeatureRemoteHive;
        if(!sender.send_return_request("return-peer",{91,8,15,true})||!owner.dispatch(*control,control->control.back())||!request.summon||request.id!=8){result="Remote Hive request serialization";return false;}
        const auto summon_packet=control->control.back();
        malformed=summon_packet;malformed.back()=17;if(owner.dispatch(*control,malformed)){result="Remote Hive hop limit accepted";return false;}
        malformed=summon_packet;malformed.pop_back();if(owner.dispatch(*control,malformed)){result="Truncated remote Hive request accepted";return false;}
        if(!sender.send_return_reply("return-peer",{91,8,ReturnResult::Returned,true})||!owner.dispatch(*control,control->control.back())||!reply.summon||reply.id!=8){result="Remote Hive reply serialization";return false;}
        request={};owner.connected.clear();
        if(!owner.dispatch(*control,summon_packet)||request.id){result="Retired connection summoned Hive";return false;}
        control->peer.features=0;
        if(owner.dispatch(*control,summon_packet)){result="Unnegotiated remote Hive accepted";return false;}
    }
    {
        Transport sender;auto& owner=*sender.impl_;
        auto control=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);control->ready=true;
        control->peer.id="cursor-peer";control->peer.features=FeatureDesktopAux|FeatureScopedCursor;
        owner.connected[control->peer.id]={control,{}};CursorState received;
        owner.callbacks.cursor=[&](const std::string&,CursorState c){received=std::move(c);};
        CursorState c;c.session=91;c.position_revision=7;c.input_capture_us=900;c.position_valid=true;c.x=123;c.y=456;
        if(!sender.send_cursor("cursor-peer",c)||!owner.dispatch(*control,control->control.back())||
           received.session!=91||received.input_capture_us!=900||!received.position_valid||received.position_revision!=7){result="Scoped cursor serialization failed";return false;}
        const auto packet=control->control.back();auto malformed=packet;malformed[17]=2;
        if(owner.dispatch(*control,malformed)){result="Invalid cursor position flag accepted";return false;}
        malformed=packet;malformed.resize(17);
        if(owner.dispatch(*control,malformed)){result="Truncated scoped cursor accepted";return false;}
        malformed=packet;std::fill(malformed.begin()+1,malformed.begin()+9,0);
        if(owner.dispatch(*control,malformed)){result="Zero cursor session accepted";return false;}
        control->peer.features=FeatureDesktopAux;
        if(owner.dispatch(*control,packet)||!sender.send_cursor("cursor-peer",c)||control->control.back()[0]!=Cursor||
           !owner.dispatch(*control,control->control.back())||received.session||received.position_revision||received.position_valid||received.x!=123){result="Mixed-version cursor shape-only fallback failed";return false;}
    }
    {
        // Real socket failures on isolated ephemeral ports: never close the
        // installed agent's listeners or change a physical NIC for this test.
        WSADATA ws{};if(WSAStartup(MAKEWORD(2,2),&ws)){result="Recovery test Winsock startup";return false;}
        struct Sockets{SOCKET tcp=INVALID_SOCKET,udp=INVALID_SOCKET;~Sockets(){if(tcp!=INVALID_SOCKET)closesocket(tcp);if(udp!=INVALID_SOCKET)closesocket(udp);WSACleanup();}}held;
        auto reserve=[&](int type,SOCKET& socket_value,uint16_t& port){
            socket_value=socket(AF_INET,type,type==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP);
            BOOL yes=TRUE;setsockopt(socket_value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&yes),sizeof(yes));
            sockaddr_in local{};local.sin_family=AF_INET;int size=sizeof(local);
            if(socket_value==INVALID_SOCKET||bind(socket_value,reinterpret_cast<sockaddr*>(&local),sizeof(local))||getsockname(socket_value,reinterpret_cast<sockaddr*>(&local),&size))return false;
            port=ntohs(local.sin_port);return type!=SOCK_STREAM||listen(socket_value,SOMAXCONN)==0;
        };
        TransportConfig cfg{"recovery-listener","Listener recovery",generate_group_key(),0,true,false};
        if(!reserve(SOCK_STREAM,held.tcp,cfg.port)||!reserve(SOCK_DGRAM,held.udp,cfg.discovery_port)){result="Cannot reserve isolated recovery ports";return false;}
        Transport recovering;std::string error;
        if(!recovering.start(cfg,{},error)){result="Transient listener conflict still fails startup: "+error;return false;}
        Sleep(150);
        if(recovering.impl_->listener!=INVALID_SOCKET||recovering.impl_->discovery!=INVALID_SOCKET){result="Exclusive conflict was not exercised";return false;}
        closesocket(held.tcp);held.tcp=INVALID_SOCKET;closesocket(held.udp);held.udp=INVALID_SOCKET;
        auto deadline=Clock::now()+std::chrono::seconds(3);
        while(Clock::now()<deadline&&(recovering.impl_->listener==INVALID_SOCKET||recovering.impl_->discovery==INVALID_SOCKET))Sleep(20);
        if(recovering.impl_->listener==INVALID_SOCKET||recovering.impl_->discovery==INVALID_SOCKET){result="TCP/UDP listeners did not recover automatically";return false;}
        Transport peer;auto peer_cfg=cfg;peer_cfg.device_id="recovery-peer";peer_cfg.port=0;peer_cfg.discovery=false;
        if(!peer.start(peer_cfg,{},error)){result=error;return false;}
        std::string id;if(!peer.connect("127.0.0.1",cfg.port,id,error)){result="Recovered listener cannot pair: "+error;return false;}
        const auto token=peer.connection_token(cfg.device_id);
        for(int i=0;i<20;++i){recovering.impl_->refresh_network=true;Sleep(25);}
        if(!token||peer.connection_token(cfg.device_id)!=token){result="Topology notification burst broke a healthy pair";return false;}
        {std::lock_guard lock(recovering.impl_->listener_mutex);const auto failed=recovering.impl_->listener.exchange(INVALID_SOCKET);shutdown(failed,SD_BOTH);closesocket(failed);}
        deadline=Clock::now()+std::chrono::seconds(3);
        while(Clock::now()<deadline&&recovering.impl_->listener==INVALID_SOCKET)Sleep(20);
        if(recovering.impl_->listener==INVALID_SOCKET||peer.connection_token(cfg.device_id)!=token){result="Failed accept socket did not recover without breaking the live pair";return false;}
        Transport newcomer;peer_cfg.device_id="recovery-newcomer";
        if(!newcomer.start(peer_cfg,{},error)||!newcomer.connect("127.0.0.1",cfg.port,id,error)){result="Reopened listener did not accept a new peer: "+error;return false;}
        const auto stop_started=Clock::now();recovering.stop();
        if(Clock::now()-stop_started>std::chrono::seconds(3)){result="Topology/discovery cancellation exceeded stop deadline";return false;}
    }

    {
        VideoWriteProgress progress;progress.reset(0);
        if(progress.due(499999)||!progress.due(500000)||progress.expired(1999999)){
            result="Video progress initial sampling/deadline failed";return false;
        }
        progress.observe(500000,1000,0);
        progress.observe(1500000,2000,0);
        if(progress.expired(3000000)){result="New video payload did not refresh its stall deadline";return false;}
        progress.observe(3000000,3000,1000);
        if(!progress.expired(3500000)){result="Retransmission-only traffic extended the video deadline";return false;}
        progress.reset(4000000);progress.unavailable();
        if(progress.due(6000000)||!progress.expired(6000000)){
            result="Unavailable TCP statistics disabled the stall watchdog";return false;
        }
    }
    {
        // A healthy slow transfer can outlast SO_SNDTIMEO in total. Only a
        // lack of write progress should fail it, and a stopped reader must
        // still terminate the send. Exercise real TCP without desktop input.
        WSADATA ws{};
        if(WSAStartup(MAKEWORD(2,2),&ws)){result="Progress test Winsock initialization failed";return false;}
        struct WinsockScope {~WinsockScope(){WSACleanup();}} winsock_scope;
        struct SocketScope {SOCKET s=INVALID_SOCKET;~SocketScope(){if(s!=INVALID_SOCKET)closesocket(s);}} listener,writer,reader;
        listener.s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        int address_size=sizeof(address);
        int receive_buffer=16*1024;
        if(setsockopt(listener.s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&receive_buffer),sizeof(receive_buffer))||
           bind(listener.s,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener.s,1)||
           getsockname(listener.s,reinterpret_cast<sockaddr*>(&address),&address_size)){
            result="Progress test listener failed";return false;
        }
        writer.s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(::connect(writer.s,reinterpret_cast<sockaddr*>(&address),sizeof(address))){result="Progress test connect failed";return false;}
        reader.s=accept(listener.s,nullptr,nullptr);
        int zero=0;DWORD timeout=1000;
        if(reader.s==INVALID_SOCKET||
           setsockopt(writer.s,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&zero),sizeof(zero))||
           setsockopt(writer.s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))||
           setsockopt(reader.s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))){
            result="Progress test socket configuration failed";return false;
        }
        Bytes payload(4*1024*1024,73);bool received=true;
        std::thread draining([&]{
            std::array<uint8_t,16*1024> part{};size_t remaining=payload.size();
            while(remaining){
                Sleep(10);
                const int count=recv(reader.s,reinterpret_cast<char*>(part.data()),int(part.size()),0);
                if(count<=0){received=false;break;}
                if(size_t(count)>remaining||!std::all_of(part.begin(),part.begin()+count,[](uint8_t value){return value==73;})){received=false;break;}
                remaining-=count;
            }
        });
        const auto begun=Clock::now();
        const bool sent=send_all(writer.s,payload.data(),payload.size());
        const auto duration=Clock::now()-begun;
        if(!sent)shutdown(writer.s,SD_BOTH);
        draining.join();
        if(!sent||!received||duration<std::chrono::milliseconds(timeout)){
            result="Progressing TCP transfer failed or did not span the per-write timeout: sent="+std::to_string(sent)+
                " received="+std::to_string(received)+" elapsed_ms="+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());return false;
        }
        payload.resize(4*1024*1024,73);
        const auto stalled=Clock::now();
        if(send_all(writer.s,payload.data(),payload.size())||Clock::now()-stalled>std::chrono::seconds(4)){
            result="Stalled TCP control write did not stop within its timeout";return false;
        }
        // Repeat the slow drain through the production encrypted overlapped
        // video writer. A full packet takes more than two seconds, but its TCP
        // payload keeps advancing. The old whole-packet deadline fails here.
        closesocket(writer.s);writer.s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        closesocket(reader.s);reader.s=INVALID_SOCKET;
        if(::connect(writer.s,reinterpret_cast<sockaddr*>(&address),sizeof(address))){result="Slow video connect failed";return false;}
        reader.s=accept(listener.s,nullptr,nullptr);
        if(reader.s==INVALID_SOCKET||setsockopt(reader.s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))){
            result="Slow video receiver setup failed";return false;
        }
        Transport endpoint;auto& owner=*endpoint.impl_;
        auto channel=std::make_shared<Impl::Conn>(&owner,writer.s);writer.s=INVALID_SOCKET;
        Key key{};if(!channel->tx.init(key)){result="Slow video cipher setup failed";return false;}
        channel->channel=1;channel->ready=true;owner.running=true;
        std::atomic<unsigned> returned_credit{0};
        owner.callbacks.frame_ready=[&](const std::string&){std::lock_guard lock(channel->queue_mutex);if(channel->video_credit)++returned_credit;};
        std::thread writing([&]{channel->write_video_loop(VideoTcpSample{true,140000,138000,65536,13090,262912,13090});});
        bool queued=channel->enqueue(payload,true);
        Bytes wire(payload.size()+28);size_t offset=0;const auto video_began=Clock::now();bool denied_credit=false;
        while(queued&&offset<wire.size()){
            Sleep(10);const auto count=recv(reader.s,reinterpret_cast<char*>(wire.data()+offset),int((std::min)(wire.size()-offset,size_t(16*1024))),0);
            if(count<=0)break;offset+=count;
            if(offset<wire.size()/2){std::lock_guard lock(channel->queue_mutex);denied_credit=denied_credit||(!channel->video&& !channel->video_credit);}
        }
        const auto video_duration=Clock::now()-video_began;
        const auto finish=Clock::now()+std::chrono::seconds(1);
        while(offset==wire.size()&&(!channel->video_send.snapshot(transport_us()).completed_packets||!returned_credit)&&Clock::now()<finish)Sleep(1);
        const auto stats=channel->video_send.snapshot(transport_us());
        channel->close();writing.join();owner.running=false;
        owner.callbacks.frame_ready={};
        if(!queued||!denied_credit||!returned_credit||offset!=wire.size()||video_duration<SessionStallTimeout||stats.completed_packets!=1||stats.failed_packets){
            result="Slow progressing overlapped video was canceled or did not complete: bytes="+std::to_string(offset)+
                " elapsed_ms="+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(video_duration).count());return false;
        }
        Cipher decoder;Bytes head(wire.begin(),wire.begin()+12),encrypted(wire.begin()+12,wire.end()-16),plain;
        std::array<uint8_t,16> tag{};std::copy(wire.end()-16,wire.end(),tag.begin());
        if(!decoder.init(key)||!decoder.crypt(false,head,encrypted,plain,tag)||plain!=payload){
            result="Slow overlapped video lost authenticated bytes";return false;
        }
        // Force only the observation, not WSASend/completion/cipher behavior.
        // A small real receive buffer holds eight writes simultaneously. Check
        // authenticated order on drain, then cancellation with all owners live.
        for(bool cancel:{false,true}){
            writer.s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            closesocket(reader.s);reader.s=INVALID_SOCKET;
            if(::connect(writer.s,reinterpret_cast<sockaddr*>(&address),sizeof(address))){result="Expanded video connect failed";return false;}
            reader.s=accept(listener.s,nullptr,nullptr);
            if(reader.s==INVALID_SOCKET||setsockopt(reader.s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))){result="Expanded video receive setup failed";return false;}
            auto expanded=std::make_shared<Impl::Conn>(&owner,writer.s);writer.s=INVALID_SOCKET;
            if(!expanded->tx.init(key)){result="Expanded video cipher setup failed";return false;}
            expanded->channel=1;expanded->ready=true;owner.running=true;
            const VideoTcpSample observed{true,100000,100000,4*1024*1024,4*1024*1024,4*1024*1024,0};
            std::thread pumping([&]{expanded->write_video_loop(observed);});
            constexpr unsigned count=8;constexpr size_t packet_size=256*1024;
            bool held=true,authenticated=true;
            const auto admission_deadline=Clock::now()+std::chrono::milliseconds(1500);
            for(unsigned i=0;i<count&&held;++i){
                // Scheduling can legitimately expire an unsent picture before
                // the writer runs. Match a host's fresh-IDR retry; never resend
                // an accepted WSASend or weaken the production freshness bound.
                do{
                    const auto expired=expanded->video_send.snapshot(transport_us()).queue_expired;
                    held=expanded->enqueue(Bytes(packet_size,uint8_t(i+1)),true);
                    while(held&&expanded->video_inflight<i+1&&!expanded->closed&&Clock::now()<admission_deadline&&
                        expanded->video_send.snapshot(transport_us()).queue_expired==expired)Sleep(1);
                }while(held&&expanded->video_inflight<i+1&&!expanded->closed&&Clock::now()<admission_deadline);
                held=held&&expanded->video_inflight==i+1;
            }
            const auto owned=expanded->video_owned_bytes.load();
            const auto limit=expanded->video_send.snapshot(transport_us()).write_limit;
            if(held&&!cancel){
                Cipher decipher;authenticated=decipher.init(key);
                for(unsigned i=0;i<count&&authenticated;++i){
                    Bytes packet(packet_size+28);
                    if(!recv_all(reader.s,packet.data(),packet.size())){authenticated=false;break;}
                    Bytes header(packet.begin(),packet.begin()+12),ciphertext(packet.begin()+12,packet.end()-16),decoded;
                    std::array<uint8_t,16> authentication{};std::copy(packet.end()-16,packet.end(),authentication.begin());
                    authenticated=decipher.crypt(false,header,ciphertext,decoded,authentication)&&decoded==Bytes(packet_size,uint8_t(i+1));
                }
                const auto completion_deadline=Clock::now()+std::chrono::seconds(1);
                while(expanded->video_inflight&&Clock::now()<completion_deadline)Sleep(1);
            }
            const auto close_began=Clock::now();expanded->close();pumping.join();owner.running=false;
            const auto completed=expanded->video_send.snapshot(transport_us());
            if(!held||!authenticated||owned!=count*(packet_size+28)||limit!=VideoSendBudget::MaximumWrites||
               expanded->video_inflight||expanded->video_owned_bytes||Clock::now()-close_began>std::chrono::seconds(1)||
               (!cancel&&(completed.completed_packets!=count||completed.failed_packets))||
               (cancel&&completed.completed_packets+completed.failed_packets!=count)){
                result="Expanded overlapped video ownership/order/cancellation failed: cancel="+std::to_string(cancel)+
                    " held="+std::to_string(held)+" authenticated="+std::to_string(authenticated)+" owned="+std::to_string(owned);return false;
            }
        }
    }
    {
        Transport endpoint;auto& owner=*endpoint.impl_;Impl::Conn control(&owner,INVALID_SOCKET);control.channel=0;control.peer.features=FeatureMediaProbe;
        bool called=false;owner.callbacks.media_probe_request=[&](const Peer&,uint64_t id,uint32_t mode){called=id==9&&mode==2;};
        Bytes request{MediaProbeRequest};u64(request,9);u32(request,2);
        if(!owner.dispatch(control,request)||!called){result="Media probe dispatch failed";return false;}
        Bytes host_request{MediaProbeRequest};u64(host_request,9);u32(host_request,4);
        if(owner.dispatch(control,host_request)){result="Unnegotiated host probe accepted";return false;}
        control.peer.features|=FeatureHostProbe;called=false;
        owner.callbacks.media_probe_request=[&](const Peer&,uint64_t id,uint32_t mode){called=id==9&&mode==4;};
        if(!owner.dispatch(control,host_request)||!called){result="Negotiated host probe dispatch failed";return false;}
        control.peer.features=0;if(owner.dispatch(control,request)){result="Unnegotiated media probe accepted";return false;}
        control.peer.features=FeatureMediaProbe;request.back()=255;
        if(owner.dispatch(control,request)){result="Invalid media probe mode accepted";return false;}
        request.resize(5);if(owner.dispatch(control,request)){result="Truncated media probe accepted";return false;}
        Bytes reply{MediaProbeReply};u64(reply,9);u32(reply,0);reply.resize(MaxMediaProbeReport+14,'x');
        if(owner.dispatch(control,reply)){result="Oversized media diagnostic report accepted";return false;}
    }
    {
        Transport endpoint;auto& owner=*endpoint.impl_;Impl::Conn control(&owner,INVALID_SOCKET);
        control.channel=0;control.peer.features=FeatureDesktopAux;
        bool requested=false,received=false;bool native=false;
        owner.callbacks.snapshot_request=[&](const std::string&,uint64_t id,bool full){requested=id==7;native=full;};
        owner.callbacks.snapshot=[&](const std::string&,Snapshot image){received=image.request==7;native=image.native;};
        Bytes request{SnapshotRequest};u64(request,7);
        if(!owner.dispatch(control,request)||!requested||native){result="Legacy snapshot request changed";return false;}
        Bytes reply{SnapshotReply};u64(reply,7);reply.push_back(42);
        if(!owner.dispatch(control,reply)||!received||native){result="Legacy snapshot reply changed";return false;}
        request[0]=NativeSnapshotRequest;reply[0]=NativeSnapshotReply;
        if(owner.dispatch(control,request)||owner.dispatch(control,reply)){result="Unnegotiated native snapshot accepted";return false;}
        control.peer.features|=FeatureNativeSnapshot;
        if(!owner.dispatch(control,request)||!native||!owner.dispatch(control,reply)||!native){result="Native snapshot negotiation failed";return false;}
        reply.resize(MaxNativeSnapshotBytes+10,42);
        if(owner.dispatch(control,reply)){result="Oversized native snapshot accepted";return false;}
        reply[0]=SnapshotReply;reply.resize(MaxSnapshotBytes+10,42);
        if(owner.dispatch(control,reply)){result="Legacy snapshot byte bound changed";return false;}
    }
    {
        Transport endpoint;auto& owner=*endpoint.impl_;Impl::Conn control(&owner,INVALID_SOCKET);
        control.channel=0;control.peer.features=FeatureBuildVersion;
        Bytes packet{BuildVersion};str(packet,"0.1.0-alpha.36");
        if(!owner.dispatch(control,packet)||control.peer.version!="0.1.0-alpha.36"){result="Build version announcement lost";return false;}
        for(const auto& invalid:{std::string{},std::string(33,'1'),std::string("1.0\nFAKE"),std::string("1.0 xyz")}){
            Bytes bad{BuildVersion};str(bad,invalid);
            if(owner.dispatch(control,bad)){result="Invalid build version accepted";return false;}
        }
        auto truncated=packet;truncated.pop_back();auto trailing=packet;trailing.push_back(0);
        if(owner.dispatch(control,truncated)||owner.dispatch(control,trailing)){result="Malformed build version accepted";return false;}
        control.peer.features=0;if(owner.dispatch(control,packet)){result="Unnegotiated build version accepted";return false;}
        control.peer.features=FeatureBuildVersion;
        for(const uint8_t channel:{uint8_t(1),uint8_t(2)}){control.channel=channel;if(owner.dispatch(control,packet)){result="Build version accepted on media/input channel";return false;}}
    }
    {
        Transport endpoint;auto& owner=*endpoint.impl_;Impl::Conn control(&owner,INVALID_SOCKET);
        control.channel=0;control.peer.id="warm-peer";control.peer.features=FeatureAutomaticStream;
        Bytes request{WarmStart};u64(request,71);u32(request,100000);
        Bytes cancel{WarmCancel};u64(cancel,71);
        if(owner.dispatch(control,request)||owner.dispatch(control,cancel)){result="Unnegotiated prewarm accepted";return false;}
        control.peer.features|=4096; // Broken 0.4.0-0.4.2 capability is reserved.
        if(owner.dispatch(control,request)||owner.dispatch(control,cancel)){result="Legacy broken prewarm capability accepted";return false;}
        control.peer.features|=FeatureWarmStream;unsigned starts=0,cancels=0;
        owner.callbacks.start_stream=[&](const std::string&,const StreamSettings& settings){if(settings.warm&&settings.session==71&&settings.receiver_millihz==100000)++starts;};
        owner.callbacks.cancel_warm=[&](const std::string&,uint64_t session){if(session==71)++cancels;};
        if(!owner.dispatch(control,request)||!owner.dispatch(control,cancel)||starts!=1||cancels!=1){result="Prewarm request/cancel identity lost";return false;}
        for(auto bytes:{request,cancel}){
            auto truncated=bytes;truncated.pop_back();auto trailing=bytes;trailing.push_back(0);
            if(owner.dispatch(control,truncated)||owner.dispatch(control,trailing)){result="Malformed prewarm accepted";return false;}
            for(uint8_t channel:{uint8_t(1),uint8_t(2)}){control.channel=channel;if(owner.dispatch(control,bytes)){result="Prewarm accepted off control lane";return false;}}
            control.channel=0;
        }
        Bytes preview{LivePeek};u64(preview,71);preview.push_back(1);
        control.peer.features|=32768; // Retired 0.7.0 capability must not opt in.
        if(owner.dispatch(control,preview)){result="Broken legacy live Peek capability accepted";return false;}
        control.peer.features|=FeatureLivePeek;unsigned previews=0;
        owner.callbacks.preview=[&](const std::string&,uint64_t session,bool enabled){if(session==71)previews+=enabled?1:10;};
        if(!owner.dispatch(control,preview)){result="Live Peek resume rejected";return false;}
        preview.back()=0;
        if(!owner.dispatch(control,preview)||previews!=11){result="Live Peek pause lost session identity";return false;}
        preview.back()=2;
        if(owner.dispatch(control,preview)){result="Invalid live Peek state accepted";return false;}
    }
    {
        InputSendWindow queue;queue.reset(71);
        uint64_t sequence=0;
        const auto put=[&](InputEvent event,uint64_t at=1000){return queue.enqueue({71,++sequence,at,event},at);};
        if(!put({InputKind::Absolute,1,2})||!put({InputKind::Absolute,3,4})||!put({InputKind::Button,1,0})||
            !put({InputKind::Absolute,5,6})||!put({InputKind::Relative,10,-20})||!put({InputKind::Relative,-3,5},1100)||
            !put({InputKind::Wheel,120,0})||!put({InputKind::Key,30,0})||!put({InputKind::Key,30,1})||!put({InputKind::ReleaseAll})) {
            result="Input ordering test setup failed";return false;
        }
        const std::array<InputKind,8> kinds{InputKind::Absolute,InputKind::Button,InputKind::Absolute,InputKind::Relative,InputKind::Wheel,InputKind::Key,InputKind::Key,InputKind::ReleaseAll};
        std::array<InputPacket,8> sent{};
        for(size_t i=0;i<sent.size();++i){if(!queue.writable()){result="Input credit exhausted before its bound";return false;}sent[i]=queue.take(1200+i);if(sent[i].event.kind!=kinds[i]){result="Motion crossed an input edge";return false;}}
        if(sent[0].event.a!=3||sent[3].event.a!=7||sent[3].event.b!=-15||sent[3].capture_time_us!=1000||queue.writable()||
            !put({InputKind::Absolute,7,8},1300)||queue.writable()||queue.expired(501200)||
            queue.expired(1200+InputSendWindow::deadline_us-1)||!queue.expired(1200+InputSendWindow::deadline_us)) {
            result="Input motion merge, eight-packet credit, or completion deadline failed";return false;
        }
        const auto& last=sent.back();
        queue.submitted(71,last.sequence,1250);
        if(!queue.acknowledge({71,last.sequence,last.capture_time_us,7,11,true},1400)||!queue.writable()) {result="Completed injection ACK did not return credit";return false;}
        const auto credit_metrics=queue.snapshot(1450);
        if(credit_metrics.credit_stalls!=1||credit_metrics.credit_wait_total_us!=100||credit_metrics.credit_wait_max_us!=100){result="Input credit wait includes non-blocked time or was not measured";return false;}

        auto stats=queue.snapshot(1500);
        if(stats.coalesced!=2||stats.inflight||stats.queued!=1||stats.acknowledged!=8||stats.capture_to_send_p99_us!=250||stats.ack_roundtrip_p99_us!=150||stats.host_queue_max_us!=7||stats.inject_max_us!=11) {
            result="Input latency metric attribution failed";return false;
        }
        if(queue.snapshot(2001400).samples){result="Input samples failed to expire";return false;}
        const auto next=queue.take(1600);
        if(queue.acknowledge({71,next.sequence,next.capture_time_us+1,0,0,true},1700)||queue.acknowledge({71,next.sequence+1,next.capture_time_us,0,0,true},1700)) {
            result="Unsent or mismatched input ACK accepted";return false;
        }
        queue.finish();if(!queue.empty()||queue.next_deadline()){result="Ended input session retained old work or deadline";return false;}
        queue.reset(72);if(queue.enqueue({71,1,1800,{InputKind::Key,30,0}},1800)){result="Old input session entered a new queue";return false;}
        for(size_t i=0;i<InputSendWindow::capacity;++i)if(!queue.enqueue({72,i+1,2000,{InputKind::Key,30,int32_t(i%2)}},2000)){result="Bounded input edge admission failed";return false;}
        if(queue.enqueue({72,129,2000,{InputKind::Key,30,1}},2000)){result="Input edge queue exceeded its fixed bound";return false;}
    }
    {
        Impl owner;Impl::Conn c(&owner,INVALID_SOCKET);c.channel=2;c.ready=true;
        c.peer.id="relay-origin";c.peer.features=FeatureDedicatedInput|FeatureInputRelay|FeatureViewRoute;c.input_receive_session=701;
        unsigned received=0;InputPacket observed;
        owner.callbacks.input_packet=[&](const std::string&,const InputPacket& packet){++received;observed=packet;};
        InputPacket packet{701,1,input_now_us(),{InputKind::Key,30,0}};
        input_append(packet,input_node("a"));input_append(packet,input_node("b"));
        const auto valid=input_bytes(packet);
        if(!owner.dispatch(c,valid)||received!=1||observed.path_size!=2||observed.path!=packet.path){result="Routed input serialization failed";return false;}
        auto malformed=valid;malformed.pop_back();
        if(owner.dispatch(c,malformed)){result="Truncated input route accepted";return false;}
        malformed=valid;malformed.push_back(0);
        if(owner.dispatch(c,malformed)){result="Trailing input route data accepted";return false;}
        packet.path[1]=packet.path[0];
        if(owner.dispatch(c,input_bytes(packet))){result="Duplicate input route node accepted";return false;}
        packet.path[1]=0;
        if(owner.dispatch(c,input_bytes(packet))){result="Zero input route node accepted";return false;}
        // Header is type + three u64s + four u32s; route count follows it.
        malformed=valid;malformed[41]=17;
        if(owner.dispatch(c,malformed)){result="Oversized input route accepted";return false;}
        malformed=valid;malformed[41]=0;malformed.resize(42);
        if(owner.dispatch(c,malformed)){result="Empty routed input accepted";return false;}
        c.peer.features=FeatureDedicatedInput;
        if(owner.dispatch(c,valid)||received!=1){result="Unnegotiated input route accepted";return false;}
        packet.path_size=0;packet.sequence=2;
        if(!owner.dispatch(c,input_bytes(packet))||received!=2||observed.path_size){result="Legacy input format regressed";return false;}
    }
    {
        // Real encrypted third-channel loopback. The control reader is held
        // inside a clipboard callback; input must still complete and ACK. No
        // Windows input APIs are called by this test.
        std::mutex guard;std::condition_variable changed;
        bool clipboard=false,started=false,blocked=false,unblock=false,ordered=false;
        unsigned packets=0,acks=0;uint64_t last_sequence=0;bool received_clock=true,received_route=true;
        Transport sender,receiver;
        struct Release {std::mutex& guard;std::condition_variable& cv;bool& flag;~Release(){std::lock_guard lock(guard);flag=true;cv.notify_all();}} release{guard,changed,unblock};
        TransportCallbacks outgoing,incoming;
        incoming.clipboard=[&](const std::string&,const std::wstring& value){
            std::unique_lock lock(guard);if(value==L"handoff")clipboard=true;
            if(value==L"hold control"){blocked=true;changed.notify_all();changed.wait(lock,[&]{return unblock;});}
        };
        incoming.start_stream=[&](const std::string&,const StreamSettings& settings){std::lock_guard lock(guard);started=settings.session==901;};
        incoming.input_begin=[&](const std::string&,uint64_t session){std::lock_guard lock(guard);ordered=clipboard&&started&&session==901;return ordered;};
        incoming.input_packet=[&](const std::string& peer,const InputPacket& packet){
            {std::lock_guard lock(guard);++packets;last_sequence=packet.sequence;received_clock=received_clock&&packet.received_time_us&&packet.received_time_us<=input_now_us();if(packet.sequence<=11)received_route=received_route&&packet.path_size==2&&packet.path[0]==input_node("input-a")&&packet.path[1]==input_node("relay");changed.notify_all();}
            // Model cross-region RTT / temporary host scheduling delay without
            // injecting real desktop input. It must not revoke the session.
            if(packet.sequence==10)Sleep(400);
            receiver.send_input_ack(peer,{packet.session,packet.sequence,packet.capture_time_us,7,11,true});
        };
        outgoing.input_ack=[&](const std::string&,const InputAck& ack){std::lock_guard lock(guard);if(ack.injected&&ack.host_queue_us==7&&ack.inject_us==11)++acks;changed.notify_all();};
        std::string error,id;
        if(!sender.start({"input-a","Input A","",0,false,true},outgoing,error)||!receiver.start({"input-b","Input B","",0,false,true},incoming,error)||
            !sender.connect("127.0.0.1",receiver.impl_->cfg.port,id,error)) {result="Dedicated input handshake: "+error;return false;}
        if(sender.send_input(id,InputPacket{901,1,input_now_us(),{InputKind::Key,30,0}})||
            !sender.send_clipboard(id,L"handoff")||!sender.request_stream(id,{0,0,60000,901})||!sender.begin_input(id,901)) {
            result="Dedicated input admission setup failed";return false;
        }
        const auto ready_deadline=Clock::now()+std::chrono::seconds(2);
        while(!sender.input_ready(id,901)&&Clock::now()<ready_deadline)Sleep(1);
        if(!sender.input_ready(id,901)||!sender.send_clipboard(id,L"hold control")){result="Input handshake did not follow clipboard/start";return false;}
        {std::unique_lock lock(guard);if(!changed.wait_for(lock,std::chrono::seconds(2),[&]{return blocked;})){result="Control isolation setup failed";return false;}}
        auto packet=InputPacket{901,10,input_now_us(),{InputKind::Key,30,0}};packet.received_time_us=UINT64_MAX;input_append(packet,input_node("input-a"));input_append(packet,input_node("relay"));
        bool ok=sender.send_input(id,packet);packet.sequence=11;packet.capture_time_us=input_now_us();packet.event.b=1;ok=sender.send_input(id,packet)&&ok;
        {std::unique_lock lock(guard);ok=changed.wait_for(lock,std::chrono::seconds(1),[&]{return acks==2;})&&ok;ok=ok&&packets==2&&last_sequence==11&&ordered&&received_clock&&received_route;unblock=true;changed.notify_all();}
        auto stats=sender.input_diagnostics(id);
        ok=ok&&stats.ready&&stats.dedicated&&stats.sent==2&&stats.acknowledged==2&&stats.samples==2&&stats.host_queue_p95_us==7&&stats.inject_p95_us==11;
        ok=ok&&sender.end_input(id,901)&&!sender.input_ready(id,901)&&!sender.send_input(id,packet);
        {std::unique_lock lock(guard);ok=changed.wait_for(lock,std::chrono::milliseconds(500),[&]{return packets==3;})&&ok;}
        // Ended ACKs must not retain a completion timeout or tear down the pair.
        Sleep(120);ok=ok&&sender.connection_token(id)&&sender.input_diagnostics(id).inflight==0;
        if(!ok){result="Input control isolation, handshake order, metadata, completion ACK, or ending-session cleanup failed";return false;}
    }
    {
        // End one direction while its control reader is blocked. The input-lane
        // revoke must retire old ACK credit immediately, preserve reverse input,
        // and not let the late control notice revoke a replacement session.
        std::mutex guard;std::condition_variable changed;bool blocked=false,unblock=false;
        unsigned held_packets=0,notices=0;
        Transport left,right;
        struct Release {std::mutex& guard;std::condition_variable& cv;bool& flag;~Release(){std::lock_guard lock(guard);flag=true;cv.notify_all();}} release{guard,changed,unblock};
        TransportCallbacks left_cb,right_cb;
        left_cb.input_begin=[](const std::string&,uint64_t session){return session==222;};
        right_cb.input_begin=[](const std::string&,uint64_t session){return session==111||session==333;};
        left_cb.clipboard=[&](const std::string&,const std::wstring&){std::unique_lock lock(guard);blocked=true;changed.notify_all();changed.wait(lock,[&]{return unblock;});};
        left_cb.input_packet=[&](const std::string& peer,const InputPacket& p){left.send_input_ack(peer,{p.session,p.sequence,p.capture_time_us,1,2,true});};
        right_cb.input_packet=[&](const std::string& peer,const InputPacket& p){
            if(p.session==111){std::lock_guard lock(guard);++held_packets;changed.notify_all();}
            else right.send_input_ack(peer,{p.session,p.sequence,p.capture_time_us,1,2,true});
        };
        left_cb.host_ended=[&](const std::string&,uint64_t session){if(session==111){std::lock_guard lock(guard);++notices;changed.notify_all();}};
        const auto acknowledged=[&](const std::string&,const InputAck&){std::lock_guard lock(guard);changed.notify_all();};
        left_cb.input_ack=right_cb.input_ack=acknowledged;
        std::string error,id;
        if(!left.start({"kick-input-a","Kick input A","",0,false,true},left_cb,error)||!right.start({"kick-input-b","Kick input B","",0,false,true},right_cb,error)||
            !left.connect("127.0.0.1",right.impl_->cfg.port,id,error)||!left.begin_input("kick-input-b",111)||!right.begin_input("kick-input-a",222)){
            result="Reciprocal input revoke setup: "+error;return false;
        }
        auto deadline=Clock::now()+std::chrono::seconds(2);
        while((!left.input_ready("kick-input-b",111)||!right.input_ready("kick-input-a",222))&&Clock::now()<deadline)Sleep(1);
        const auto connection=left.connection_token("kick-input-b");
        bool ok=left.input_ready("kick-input-b",111)&&right.input_ready("kick-input-a",222)&&right.send_clipboard("kick-input-a",L"block control");
        {std::unique_lock lock(guard);ok=changed.wait_for(lock,std::chrono::seconds(2),[&]{return blocked;})&&ok;}
        ok=left.send_input("kick-input-b",InputPacket{111,1,input_now_us(),{InputKind::Key,30,0}})&&ok;
        {std::unique_lock lock(guard);ok=changed.wait_for(lock,std::chrono::milliseconds(250),[&]{return held_packets==1;})&&ok;}
        ok=right.end_stream("kick-input-a",111)&&ok;
        {std::unique_lock lock(guard);ok=changed.wait_for(lock,std::chrono::milliseconds(250),[&]{return notices==1;})&&ok;}
        auto retired=left.input_diagnostics("kick-input-b");
        ok=ok&&!retired.ready&&!retired.queued&&!retired.inflight&&right.input_ready("kick-input-a",222);
        ok=left.begin_input("kick-input-b",333)&&ok;
        deadline=Clock::now()+std::chrono::seconds(2);while(!left.input_ready("kick-input-b",333)&&Clock::now()<deadline)Sleep(1);
        ok=left.send_input("kick-input-b",InputPacket{333,2,input_now_us(),{InputKind::Key,30,1}})&&
            right.send_input("kick-input-a",InputPacket{222,1,input_now_us(),{InputKind::Relative,4,5}})&&ok;
        {std::unique_lock lock(guard);ok=changed.wait_for(lock,std::chrono::milliseconds(500),[&]{return left.input_diagnostics("kick-input-b").acknowledged==1&&right.input_diagnostics("kick-input-a").acknowledged==1;})&&ok;}
        Sleep(120);ok=ok&&connection&&left.connection_token("kick-input-b")==connection;
        {std::unique_lock lock(guard);unblock=true;changed.notify_all();ok=changed.wait_for(lock,std::chrono::milliseconds(500),[&]{return notices==2;})&&ok;}
        ok=ok&&left.input_ready("kick-input-b",333)&&right.input_ready("kick-input-a",222)&&left.connection_token("kick-input-b")==connection;
        ok=left.send_input("kick-input-b",InputPacket{333,3,input_now_us(),{InputKind::Relative,6,7}})&&ok;
        {std::unique_lock lock(guard);ok=changed.wait_for(lock,std::chrono::milliseconds(500),[&]{return left.input_diagnostics("kick-input-b").acknowledged==2;})&&ok;}
        left.end_input("kick-input-b",333);right.end_input("kick-input-a",222);
        if(!ok){result="Blocked-control input revoke disrupted reciprocal input, leaked old ACK credit, or revoked a replacement session";return false;}
    }
    {
        std::mutex guard;std::condition_variable changed;bool disconnected=false;Transport sender,receiver;
        TransportCallbacks outgoing,incoming;
        outgoing.peer=[&](const Peer& peer){if(!peer.authenticated){std::lock_guard lock(guard);disconnected=true;changed.notify_all();}};
        incoming.input_begin=[](const std::string&,uint64_t){return true;};
        incoming.input_packet=[](const std::string&,const InputPacket&){}; // Inject completion deliberately absent.
        std::string error,id;
        if(!sender.start({"input-stall-a","Input stall A","",0,false,true},outgoing,error)||!receiver.start({"input-stall-b","Input stall B","",0,false,true},incoming,error)||
            !sender.connect("127.0.0.1",receiver.impl_->cfg.port,id,error)||!sender.begin_input(id,902)){result="Input stall setup: "+error;return false;}
        const auto deadline=Clock::now()+std::chrono::seconds(2);while(!sender.input_ready(id,902)&&Clock::now()<deadline)Sleep(1);
        if(!sender.send_input(id,InputPacket{902,1,input_now_us(),{InputKind::Relative,1,2}})){result="Input stall packet was not admitted";return false;}
        {std::unique_lock lock(guard);
            if(changed.wait_for(lock,std::chrono::milliseconds(400),[&]{return disconnected;})){result="Subsecond input delay revoked control";return false;}
            if(!changed.wait_for(lock,SessionStallTimeout+std::chrono::seconds(1),[&]{return disconnected;})){result="Stalled input was replayable beyond its completion budget";return false;}}
        if(sender.connection_token(id)){result="Input stall did not close the authenticated channel set";return false;}
    }
    {
        // A live broker rejection must not leave a healthy-looking video pair
        // permanently swallowing keys. Ended-session ACKs were checked above.
        std::mutex guard;std::condition_variable changed;bool disconnected=false;Transport sender,receiver;
        TransportCallbacks outgoing,incoming;
        outgoing.peer=[&](const Peer& peer){if(!peer.authenticated){std::lock_guard lock(guard);disconnected=true;changed.notify_all();}};
        incoming.input_begin=[](const std::string&,uint64_t){return true;};
        incoming.input_packet=[&](const std::string& peer,const InputPacket& p){receiver.send_input_ack(peer,{p.session,p.sequence,p.capture_time_us,0,0,false});};
        std::string error,id;
        if(!sender.start({"input-reject-a","Input reject A","",0,false,true},outgoing,error)||!receiver.start({"input-reject-b","Input reject B","",0,false,true},incoming,error)||
            !sender.connect("127.0.0.1",receiver.impl_->cfg.port,id,error)||!sender.begin_input(id,903)){result="Input rejection setup: "+error;return false;}
        const auto deadline=Clock::now()+std::chrono::seconds(2);while(!sender.input_ready(id,903)&&Clock::now()<deadline)Sleep(1);
        if(!sender.send_input(id,InputPacket{903,1,input_now_us(),{InputKind::Key,30,0}})){result="Input rejection packet was not admitted";return false;}
        {std::unique_lock lock(guard);if(!changed.wait_for(lock,std::chrono::seconds(1),[&]{return disconnected;})){result="Negative injection ACK left a dead input lane connected";return false;}}
        if(sender.connection_token(id)){result="Rejected input did not revoke its authenticated channel set";return false;}
    }
    {
        VideoSendWindow timing;
        const auto empty=timing.snapshot(1000000);
        if(empty.samples||empty.sample_age_us!=UINT32_MAX){result="Empty send timing window reported stale data";return false;}
        uint64_t finished=0;
        for(uint32_t i=1;i<=20;++i){
            const uint64_t queued=1000000+uint64_t(i)*10000;
            finished=queued+i*15;
            timing.record({queued,queued+i*2,queued+i*5,finished,i*3,100,true,true});
        }
        auto measured=timing.snapshot(finished);
        if(measured.samples!=20||measured.submitted_packets!=20||measured.completed_packets!=20||measured.completed_bytes!=2000||measured.failed_packets||
            measured.encrypt_p95_us!=57||measured.encrypt_max_us!=60||measured.send_p95_us!=190||measured.send_max_us!=200||
            measured.queue_p95_us!=38||measured.queue_max_us!=40||measured.ready_gap_p95_us!=95||measured.ready_gap_max_us!=100){
            result="Bounded send timing percentiles/counters incorrect";return false;
        }
        // After all old samples have been overwritten, a prior large maximum
        // cannot keep appearing in the bounded ring's recent measurements.
        for(uint32_t i=0;i<VideoSendWindow::capacity;++i){
            const uint64_t queued=1300000+uint64_t(i)*1000;finished=queued+20;
            timing.record({queued,queued+5,queued+10,finished,5,100,true,true});
        }
        measured=timing.snapshot(finished);
        if(measured.samples!=VideoSendWindow::capacity||measured.send_max_us!=10||measured.queue_max_us!=5||measured.encrypt_max_us!=5){
            result="Video sender timing ring exceeded its bound or retained overwritten samples";return false;
        }
        VideoSendWindow idle;
        idle.record({1000000,1000100,1000300,1000800,200,100,true,true});
        idle.record({4000000,4000050,4000080,4000180,30,100,true,true});
        measured=idle.snapshot(4000180);
        if(measured.samples!=1||measured.submit_gap_max_us!=2999280||measured.ready_gap_max_us!=80||measured.send_max_us!=100||measured.queue_max_us!=50){
            result="Application supply idle was mislabeled as ready-send dispatch delay";return false;
        }
        measured=idle.snapshot(6000180);
        if(measured.samples||measured.send_max_us||measured.send_p95_us||measured.sample_age_us!=2000000||measured.completed_packets!=2){
            result="Expired send timings remained current or lost lifetime counters";return false;
        }
        idle.record({6000200,6000210,6000220,6150220,10,100,true,false});idle.expired();idle.expired();
        measured=idle.snapshot(6150220);
        if(measured.submitted_packets!=3||measured.completed_packets!=2||measured.completed_bytes!=200||measured.failed_packets!=1||measured.queue_expired!=2||measured.send_max_us!=150000){
            result="Failed/expired video send accounting hid a stall or counted delivered bytes";return false;
        }
    }
    {
        Transport sender;auto& owner=*sender.impl_;
        auto control=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);
        auto video=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);
        control->ready=video->ready=true;control->peer.id="byte-peer";
        control->peer.features=FeatureAutomaticStream|FeatureAutoRate|FeatureByteFeedback;
        video->token=21;video->received_bytes->store(9000);
        owner.connected["byte-peer"]={control,video};StreamFeedback received;
        owner.callbacks.feedback=[&](const std::string&,const StreamFeedback& f){received=f;};
        StreamFeedback report;report.session=700;
        if(!sender.send_feedback("byte-peer",report)||!owner.dispatch(*control,control->control.back())||
           !received.transport_details||received.transport_bytes||received.received_frames){result="Pre-frame byte feedback baseline failed";return false;}
        video->received_bytes->fetch_add(120000);
        if(!sender.send_feedback("byte-peer",report)||!owner.dispatch(*control,control->control.back())||
           received.transport_bytes!=120000||received.feedback_sequence!=2||received.received_frames){result="Incomplete packet progress requires a complete picture";return false;}
        StreamSettings active;active.session=report.session;active.receiver_millihz=60000;
        if(!sender.request_stream("byte-peer",active)){result="Feedback ownership setup failed";return false;}
        auto stale=report;stale.session=999;
        if(!sender.send_feedback("byte-peer",stale)||!owner.dispatch(*control,control->control.back())||received.transport_details||
            !sender.send_feedback("byte-peer",report)||!owner.dispatch(*control,control->control.back())||
            received.transport_bytes!=120000||received.feedback_sequence!=3){result="Stale feedback reset the active byte baseline or report sequence";return false;}
        const auto extended=control->control.back();auto short_packet=extended;short_packet.pop_back();
        if(owner.dispatch(*control,short_packet)){result="Truncated byte feedback accepted";return false;}
        control->peer.features&=~FeatureByteFeedback;
        if(owner.dispatch(*control,extended)||!sender.send_feedback("byte-peer",report)||
           control->control.back().size()!=30||!owner.dispatch(*control,control->control.back())||received.transport_details){result="Byte feedback capability fallback failed";return false;}
        control->peer.features|=FeatureByteFeedback;report.session=701;
        active.session=701;if(!sender.request_stream("byte-peer",active)){result="Next feedback session setup failed";return false;}
        if(!sender.send_feedback("byte-peer",report)||!owner.dispatch(*control,control->control.back())||received.transport_bytes){result="New viewing session inherited old bytes";return false;}
        video->token=22;video->received_bytes->store(77);
        if(!sender.send_feedback("byte-peer",report)||!owner.dispatch(*control,control->control.back())||received.transport_bytes||received.feedback_sequence!=1){result="Reconnected video inherited old progress";return false;}
    }
    {
        Transport sender;auto& owner=*sender.impl_;
        auto control=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);control->ready=true;
        control->peer.id="rate-peer";control->peer.features=FeatureAutomaticStream|FeatureStreamDiagnostics|FeatureEncodeTiming|FeatureAutoRate;
        owner.connected["rate-peer"]={control,{}};
        StreamFeedback received;SourceDiagnostics observed;
        owner.callbacks.feedback=[&](const std::string&,const StreamFeedback& f){received=f;};
        owner.callbacks.diagnostics=[&](const std::string&,const SourceDiagnostics& d){observed=d;};
        StreamFeedback f{91,123456,700,2100,10,false};f.rate_details=true;f.feedback_sequence=73;
        f.received_bytes=0x100001234ull;f.received_frames=240;f.received_kbps=200000;f.received_millihz=120000;
        f.receive_sample_age_us=40000;f.service_sample_age_us=2000;f.transit_extra_us=123;
        f.sequence_gaps=2;f.age_expired=3;f.queue_overflows=4;
        if(!sender.send_feedback("rate-peer",f)||!owner.dispatch(*control,control->control.back())||!received.rate_details||
           received.feedback_sequence!=73||received.received_bytes!=f.received_bytes||received.received_frames!=240||
           received.received_kbps!=200000||received.received_millihz!=120000||received.receive_sample_age_us!=40000||
           received.service_sample_age_us!=2000||received.transit_extra_us!=123||received.sequence_gaps!=2||received.age_expired!=3||received.queue_overflows!=4){
            result="Automatic-rate receiver extension roundtrip failed";return false;
        }
        const auto full_feedback=control->control.back();auto truncated=full_feedback;truncated.pop_back();
        auto trailing=full_feedback;trailing.push_back(0);
        if(owner.dispatch(*control,truncated)||owner.dispatch(*control,trailing)){result="Malformed automatic-rate feedback accepted";return false;}
        SourceDiagnostics d{91,120000,2300,10,20,5000,0,400,1900,6000,true};d.rate_details=true;d.automatic_cbr=true;d.quality_limited=true;
        d.target_bitrate_bps=180000000;d.actual_bitrate_bps=175000000;d.average_qp=20;d.rate_reason=2;
        d.receiver_service_us=2100;d.receiver_queue_us=10;d.receiver_recover=true;d.feedback_sequence=73;d.source_discarded=5;
        d.sequence_gaps=2;d.age_expired=3;d.queue_overflows=4;
        if(!sender.send_diagnostics("rate-peer",d)||!owner.dispatch(*control,control->control.back())||!observed.rate_details||
           !observed.automatic_cbr||!observed.quality_limited||!observed.receiver_recover||observed.target_bitrate_bps!=180000000||
           observed.actual_bitrate_bps!=175000000||observed.average_qp!=20||observed.rate_reason!=2||observed.receiver_service_us!=2100||
           observed.receiver_queue_us!=10||observed.feedback_sequence!=73||observed.source_discarded!=5||observed.sequence_gaps!=2||observed.age_expired!=3||observed.queue_overflows!=4){
            result="Automatic-rate source extension roundtrip failed";return false;
        }
        const auto before_reports=control->control.size();
        for(int i=0;i<1000;++i){d.encode_us=2300+i;if(!sender.send_diagnostics("rate-peer",d)){result="HUD diagnostic enqueue failed";return false;}}
        size_t actual_bytes=0;for(const auto& packet:control->control)actual_bytes+=packet.size();
        if(control->control.size()!=before_reports||actual_bytes!=control->control_bytes||control->control.front()!=full_feedback||
           !owner.dispatch(*control,control->control.back())||observed.encode_us!=3299){result="HUD reports accumulated or damaged unrelated control messages";return false;}
        d.encode_us=2300;sender.send_diagnostics("rate-peer",d);
        const auto full_diagnostics=control->control.back();truncated=full_diagnostics;truncated.pop_back();trailing=full_diagnostics;trailing.push_back(0);
        if(owner.dispatch(*control,truncated)||owner.dispatch(*control,trailing)){result="Malformed automatic-rate diagnostics accepted";return false;}
        // Old peers receive exactly their previous layouts; new peers also
        // accept old messages without inventing rate measurements.
        control->peer.features&=~FeatureAutoRate;
        if(owner.dispatch(*control,full_feedback)||owner.dispatch(*control,full_diagnostics)||
           !sender.send_feedback("rate-peer",f)||control->control.back().size()!=30||!owner.dispatch(*control,control->control.back())||received.rate_details||
           !sender.send_diagnostics("rate-peer",d)||control->control.back().size()!=45||!owner.dispatch(*control,control->control.back())||observed.rate_details||!observed.encode_details){
            result="Automatic-rate legacy capability gating failed";return false;
        }
        control->peer.features|=FeatureAutoRate;
        if(!owner.dispatch(*control,control->control.back())||observed.rate_details){result="Legacy diagnostic rejected by new peer";return false;}
        d.average_qp=52;f.received_millihz=4000001;
        if(sender.send_diagnostics("rate-peer",d)||sender.send_feedback("rate-peer",f)){result="Automatic-rate metric bounds failed";return false;}
        d.average_qp=UINT32_MAX;d.rate_reason=8;
        if(sender.send_diagnostics("rate-peer",d)){result="Automatic-rate reason bounds failed";return false;}
    }
    {
        Transport sender;auto& owner=*sender.impl_;
        auto control=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);
        auto video=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);
        control->ready=video->ready=true;video->channel=1;
        control->peer.id=video->peer.id="timing-peer";
        control->peer.features=video->peer.features=FeatureAutomaticStream|FeatureFrameTiming|FeatureHostKick;
        owner.connected["timing-peer"]={control,video};
        EncodedFrame observed;unsigned delivered=0;
        owner.callbacks.frame=[&](const std::string&,EncodedFrame frame){observed=std::move(frame);++delivered;};
        EncodedFrame frame;frame.width=1920;frame.height=1080;frame.sequence=7;frame.capture_time_us=1000000;
        frame.source_ready_us=1080000;frame.session=71;frame.keyframe=true;frame.data={1,2,3};
        if(!sender.send_frame("timing-peer",frame)||!video->video||video->video->payload[0]!=AutoFrameTimed||
            !owner.dispatch(*video,video->video->payload)||observed.capture_time_us!=frame.capture_time_us||
            observed.source_ready_us!=frame.source_ready_us||observed.session!=frame.session||observed.data!=frame.data){
            result="Source-completion video extension roundtrip failed";return false;
        }
        const auto timed=video->video->payload;
        Bytes legacy{AutoFrame};u64(legacy,frame.session);u32(legacy,frame.width);u32(legacy,frame.height);
        u64(legacy,frame.sequence);u64(legacy,frame.capture_time_us);legacy.push_back(1);legacy.insert(legacy.end(),frame.data.begin(),frame.data.end());
        // A new sender must preserve the exact old layout for an old receiver.
        video->peer.features&=~FeatureFrameTiming;
        if(!sender.send_frame("timing-peer",frame)||video->video->payload!=legacy||
            !owner.dispatch(*video,video->video->payload)||observed.source_ready_us||owner.dispatch(*video,timed)){
            result="Timed video was not capability gated for a legacy peer";return false;
        }
        // A new receiver must accept old senders without inventing provenance.
        video->peer.features|=FeatureFrameTiming;
        if(!owner.dispatch(*video,legacy)||observed.source_ready_us||observed.data!=frame.data){
            result="New receiver rejected the legacy automatic-video layout";return false;
        }
        auto missing=timed;missing.resize(41);
        auto zero=timed,before_capture=timed,unbounded=timed;
        auto overwrite_ready=[](Bytes& bytes,uint64_t value){for(unsigned i=0;i<8;++i)bytes[34+i]=uint8_t(value>>(56-i*8));};
        overwrite_ready(zero,0);overwrite_ready(before_capture,frame.capture_time_us-1);
        overwrite_ready(unbounded,frame.capture_time_us+MaxSourceWorkUs+1);
        const auto before=delivered;
        if(owner.dispatch(*video,missing)||owner.dispatch(*video,zero)||owner.dispatch(*video,before_capture)||
            owner.dispatch(*video,unbounded)||owner.dispatch(*control,timed)||delivered!=before){
            result="Malformed source-completion provenance accepted";return false;
        }
        frame.source_ready_us=frame.capture_time_us+MaxSourceWorkUs+1;
        if(sender.send_frame("timing-peer",frame)){result="Sender accepted unbounded source work";return false;}
        frame.source_ready_us=1080000;
        if(!sender.send_frame("timing-peer",frame)||!sender.end_stream("timing-peer",71)||video->video||!video->wait_keyframe){
            result="Session termination retained an obsolete timed video packet";return false;
        }
    }
    {
        // Cursor composition belongs to the encoded image, including images
        // waiting in the video slot. A separate cursor message cannot replace it.
        Transport sender;auto& owner=*sender.impl_;
        auto control=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);
        auto video=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);
        control->ready=video->ready=true;video->channel=1;
        control->peer.id=video->peer.id="composition-peer";
        control->peer.features=video->peer.features=FeatureAutomaticStream|FeatureFrameTiming|FeatureCursorComposition|FeatureHostKick;
        owner.connected["composition-peer"]={control,video};
        EncodedFrame observed;unsigned delivered=0;
        owner.callbacks.frame=[&](const std::string&,EncodedFrame frame){observed=std::move(frame);++delivered;};
        EncodedFrame frame;frame.width=1920;frame.height=1080;frame.sequence=8;frame.capture_time_us=1000000;
        frame.source_ready_us=1080000;frame.session=72;frame.keyframe=true;frame.data={0,0,0,1,99};
        for(const auto composition:{CursorComposition::Unknown,CursorComposition::Separate,CursorComposition::NoSeparate}){
            frame.cursor_composition=composition;
            if(!sender.send_frame("composition-peer",frame)||!video->video||video->video->payload[0]!=FrameCursor||
                !owner.dispatch(*video,video->video->payload)||observed.cursor_composition!=composition||
                observed.capture_time_us!=frame.capture_time_us||observed.source_ready_us!=frame.source_ready_us||
                observed.sequence!=frame.sequence||observed.session!=frame.session||observed.data!=frame.data){
                result="Image-associated cursor composition roundtrip failed";return false;
            }
        }
        const auto composed=video->video->payload;const auto before=delivered;
        auto invalid=composed;invalid[42]=3;
        if(owner.dispatch(*video,invalid)||owner.dispatch(*control,composed)||delivered!=before){
            result="Invalid composition or wrong-channel video was accepted";return false;
        }
        for(size_t size=0;size<=43;++size){
            const Bytes truncated(composed.begin(),composed.begin()+size);
            if(owner.dispatch(*video,truncated)||delivered!=before){result="Truncated composition metadata or empty image accepted";return false;}
        }
        // Capability absence preserves the exact previous timed frame layout.
        Bytes legacy{AutoFrameTimed};u64(legacy,frame.session);u32(legacy,frame.width);u32(legacy,frame.height);
        u64(legacy,frame.sequence);u64(legacy,frame.capture_time_us);legacy.push_back(1);u64(legacy,frame.source_ready_us);
        legacy.insert(legacy.end(),frame.data.begin(),frame.data.end());
        video->peer.features&=~FeatureCursorComposition;
        if(owner.dispatch(*video,composed)||!sender.send_frame("composition-peer",frame)||video->video->payload!=legacy||
            !owner.dispatch(*video,legacy)||observed.cursor_composition!=CursorComposition::Unknown||observed.source_ready_us!=frame.source_ready_us){
            result="Cursor composition failed legacy timed-layout fallback";return false;
        }
        video->peer.features|=FeatureCursorComposition;
        if(!owner.dispatch(*video,legacy)||observed.cursor_composition!=CursorComposition::Unknown){
            result="New composition receiver rejected a legacy sender";return false;
        }
        video->peer.features&=~FeatureFrameTiming;
        if(owner.dispatch(*video,composed)||!sender.send_frame("composition-peer",frame)||
            !owner.dispatch(*video,video->video->payload)||observed.source_ready_us||observed.cursor_composition!=frame.cursor_composition){
            result="Composition and timing capabilities were not independently gated";return false;
        }
        auto untimed_legacy=legacy;untimed_legacy[0]=AutoFrame;untimed_legacy.erase(untimed_legacy.begin()+34,untimed_legacy.begin()+42);
        video->peer.features&=~FeatureCursorComposition;
        if(!sender.send_frame("composition-peer",frame)||video->video->payload!=untimed_legacy||
            !owner.dispatch(*video,untimed_legacy)||observed.cursor_composition!=CursorComposition::Unknown||observed.source_ready_us){
            result="Composition changed the untimed legacy automatic-frame layout";return false;
        }
        // Manual streams also preserve ownership, without pretending to have
        // negotiated automatic sessions or source-completion clock provenance.
        video->peer.features=FeatureCursorComposition;frame.session=0;
        if(owner.dispatch(*video,composed)||!sender.send_frame("composition-peer",frame)||
            !owner.dispatch(*video,video->video->payload)||observed.session||observed.source_ready_us||observed.cursor_composition!=frame.cursor_composition){
            result="Manual composition frame required automatic stream negotiation";return false;
        }
        auto manual_timed=video->video->payload;
        for(unsigned i=0;i<8;++i)manual_timed[34+i]=uint8_t(frame.source_ready_us>>(56-i*8));
        video->peer.features|=FeatureFrameTiming;
        if(owner.dispatch(*video,manual_timed)){result="Manual composition frame accepted automatic timing provenance";return false;}
        Bytes manual{Frame};manual.insert(manual.end(),untimed_legacy.begin()+9,untimed_legacy.end());
        video->peer.features=0;
        if(!sender.send_frame("composition-peer",frame)||video->video->payload!=manual||!owner.dispatch(*video,manual)||
            observed.cursor_composition!=CursorComposition::Unknown||observed.session){result="Composition changed the legacy manual-frame layout";return false;}
        frame.cursor_composition=CursorComposition(255);
        if(sender.send_frame("composition-peer",frame)){result="Sender accepted invalid cursor composition";return false;}
        frame.cursor_composition=CursorComposition::Separate;frame.session=72;
        video->peer.features=FeatureAutomaticStream|FeatureFrameTiming|FeatureCursorComposition|FeatureHostKick;
        if(!sender.send_frame("composition-peer",frame)||!sender.end_stream("composition-peer",72)||video->video||!video->wait_keyframe){
            result="Session termination retained an obsolete composition frame";return false;
        }
    }
    {
        // Session termination must preserve the connection and the reciprocal
        // stream, reject malformed input, and never send unknown legacy commands.
        Transport sender;auto& owner=*sender.impl_;
        auto control=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);
        auto video=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);
        control->ready=video->ready=true;video->channel=1;
        control->peer.id=video->peer.id="kick-peer";
        control->peer.features=video->peer.features=FeatureAutomaticStream|FeatureHostKick;
        owner.connected["kick-peer"]={control,video};
        unsigned notices=0,stops=0;uint64_t ended_session=0;std::string ended_peer;
        owner.callbacks.host_ended=[&](const std::string& peer,uint64_t session){++notices;ended_peer=peer;ended_session=session;};
        owner.callbacks.stop_stream=[&](const std::string& peer){if(peer=="kick-peer")++stops;};
        EncodedFrame frame;frame.session=41;frame.width=frame.height=2;frame.keyframe=true;frame.data={1,2,3};
        if(!sender.send_frame("kick-peer",frame)||sender.end_stream("kick-peer",0)||
            !sender.end_stream("kick-peer",42)||!video->video||video->wait_keyframe||
            !owner.dispatch(*control,control->control.back())||ended_session!=42||
            !sender.end_stream("kick-peer",41)||video->video||!video->wait_keyframe||
            !owner.dispatch(*control,control->control.back())||ended_session!=41||ended_peer!="kick-peer"||
            !owner.dispatch(*control,Bytes{Stop})||notices!=2||stops!=1||control->closed||video->closed){
            result="Session-scoped host kick disrupted another stream or failed delivery";return false;
        }
        const auto valid=control->control.back();auto truncated=valid,trailing=valid;
        truncated.pop_back();trailing.push_back(0);Bytes zero{HostEnded};u64(zero,0);
        if(owner.dispatch(*control,truncated)||owner.dispatch(*control,trailing)||owner.dispatch(*control,zero)||
            owner.dispatch(*video,valid)||notices!=2){result="Malformed host kick accepted";return false;}
        control->peer.features=0;const auto count=control->control.size();
        if(sender.end_stream("kick-peer",41)||control->control.size()!=count||owner.dispatch(*control,valid)||notices!=2){
            result="Host kick capability gating failed";return false;
        }
        if(sender.begin_input("kick-peer",41)||sender.input_ready("kick-peer",41)||sender.send_input("kick-peer",InputPacket{41,1,1,{InputKind::ReleaseAll}})){
            result="Dedicated input did not reject a legacy channel set";return false;
        }
        auto input=std::make_shared<Impl::Conn>(&owner,INVALID_SOCKET);input->ready=true;input->channel=2;input->peer.id="kick-peer";
        input->peer.features=FeatureDedicatedInput;input->input_receive_session=41;input->input_send.reset(77);input->input_enabled=true;
        owner.connected["kick-peer"].input=input;control->ready=false;
        if(!sender.end_stream("kick-peer",41)||input->input_receive_session||!input->input_enabled||input->input_send.session()!=77||input->control.empty()||input->control.back()[0]!=InputRevoke){
            result="Dedicated revoke depended on control capacity or canceled reciprocal input";return false;
        }
    }
    {
        Impl owner;Impl::Conn c(&owner,INVALID_SOCKET);c.channel=1;c.ready=true;
        const auto now=Clock::now();
        // Full queues invalidate the entire dependent tail. A keyframe may
        // replace an unsent predecessor; a P-frame must never do so.
        if(!c.enqueue(Bytes{1},true)||c.enqueue(Bytes{2})||c.video||!c.wait_keyframe||c.enqueue(Bytes{3})||
            !c.enqueue(Bytes{4},true)||c.wait_keyframe||!c.enqueue(Bytes{5},true)){
            result="Bounded video queue dependency recovery failed";return false;
        }
        c.video->queued_at=now;
        auto packet=c.take_video(now+kVideoQueueAge-std::chrono::microseconds(1));
        if(!packet||*packet!=Bytes{5}||c.video){result="Fresh queued video was lost";return false;}
        if(!c.enqueue(Bytes{6})){result="Valid P-frame chain did not resume after IDR";return false;}
        c.video->queued_at=now;
        if(c.take_video(now+kVideoQueueAge)||!c.wait_keyframe||c.enqueue(Bytes{7})||!c.enqueue(Bytes{8},true)){
            result="Stale video did not drop dependent frames until a new IDR";return false;
        }
        // Exercise the production writer's expiry branch and verify callbacks
        // can take the queue lock: recovery and readiness must be outside it.
        c.video->queued_at=Clock::now()-kVideoQueueAge;
        unsigned recovery=0,readiness=0;owner.running=true;
        owner.callbacks.request_keyframe=[&](const std::string&){std::lock_guard lock(c.queue_mutex);++recovery;owner.running=false;};
        owner.callbacks.frame_ready=[&](const std::string&){std::lock_guard lock(c.queue_mutex);++readiness;};
        c.write_loop();
        if(recovery!=1||readiness!=1||c.video||!c.wait_keyframe){result="Expired video recovery notification failed";return false;}
    }
    {
        // Race an idle writer against close. Shutdown publication shares the
        // condition-variable mutex, so neither ordering may lose its wake.
        Impl owner;owner.running=true;
        for(unsigned round=0;round<32;++round){
            Impl::Conn c(&owner,INVALID_SOCKET);c.channel=1;c.ready=true;
            std::unique_lock hold(c.queue_mutex);
            std::thread writer([&]{c.write_loop();});
            std::thread closer([&]{c.close();});
            hold.unlock();closer.join();writer.join();
        }
    }
    {
        Agreement first,second;Bytes a,b;Key ka{},kb{};
        if(!first.generate(a)||!second.generate(b)||a==b||!first.derive(b,ka)||!second.derive(a,kb)||!equal_key(ka,kb)){result="Ephemeral ECDH agreement failed";return false;}
        b[0]^=1;if(first.derive(b,kb)){result="Invalid ECDH public key accepted";return false;}
        Transport left,right;std::string error,id;std::mutex guard;std::condition_variable changed;bool text=false,frame=false,renamed=false;
        bool ready_notified=false,version_received=false,reverse_version=false;TransportCallbacks received,sending;
        sending.peer=[&](const Peer& peer){std::lock_guard lock(guard);if(peer.version==HIVEDESK_VERSION)reverse_version=true;changed.notify_all();};
        sending.frame_ready=[&](const std::string& peer){
            // Query readiness from the callback to catch accidental invocation
            // while the transport's queue/map locks are still held.
            const bool available=left.frame_ready(peer);
            std::lock_guard lock(guard);ready_notified=available;changed.notify_all();
        };
        received.clipboard=[&](const std::string&,const std::wstring& value){std::lock_guard lock(guard);text=value==L"keyless";changed.notify_all();};
        received.frame=[&](const std::string&,EncodedFrame value){std::lock_guard lock(guard);frame=value.data==Bytes({1,2,3});changed.notify_all();};
        received.peer=[&](const Peer& peer){std::lock_guard lock(guard);if(peer.name=="Renamed without disconnect")renamed=true;if(peer.version==HIVEDESK_VERSION)version_received=true;changed.notify_all();};
        if(!left.start({"lan-a","LAN A","",0,false,true},sending,error)||!right.start({"lan-b","LAN B","",0,false,true},received,error)||
            !left.connect("127.0.0.2",right.impl_->cfg.port,id,error)){result="Keyless dual-channel handshake: "+error;return false;}
        left.rename("Renamed without disconnect");EncodedFrame picture;picture.width=2;picture.height=2;picture.keyframe=true;picture.data={1,2,3};
        const auto packet_ready=Clock::now()+std::chrono::seconds(3);
        while(!left.frame_ready(id)&&Clock::now()<packet_ready)Sleep(1);
        auto packet_lane=left.impl_->find(id,true);
        if(!packet_lane||!packet_lane->datagram||packet_lane->datagram->route()!=DatagramVideo::Route::Datagram){result="UDP did not preserve the authenticated TCP local address";return false;}
        bool ok=left.send_clipboard(id,L"keyless")&&left.send_frame(id,picture);
        {std::unique_lock lock(guard);ok=changed.wait_for(lock,std::chrono::seconds(3),[&]{return text&&frame&&renamed&&ready_notified&&version_received&&reverse_version;})&&ok;}
        if(!ok){result="Initial datagram delivery failed";return false;}
        // Exercise the actual TCP writer/reader barrier, not a mock callback.
        // Refuse one direction's UDP until its bounded fallback fires, while
        // the authenticated control/input/video connections remain unchanged.
        const auto receiver=right.impl_->find("lan-a",true);
        if(!receiver||!receiver->datagram){result="Recovery receiver missing";return false;}
        receiver->datagram->tcp_received();++picture.sequence;
        if(!left.send_frame(id,picture)){result="Recovery black-hole setup failed";return false;}
        auto until=Clock::now()+std::chrono::seconds(3);
        while(packet_lane->datagram->route()!=DatagramVideo::Route::Tcp&&Clock::now()<until)Sleep(1);
        if(packet_lane->datagram->route()!=DatagramVideo::Route::Tcp){result="UDP black hole did not fall back";return false;}
        {std::lock_guard lock(guard);frame=false;}
        ++picture.sequence;
        if(!left.send_frame(id,picture)){result="TCP fallback IDR rejected";return false;}
        {std::unique_lock lock(guard);if(!changed.wait_for(lock,std::chrono::seconds(2),[&]{return frame;})){result="TCP fallback picture not delivered";return false;}}
        until=Clock::now()+std::chrono::seconds(11);
        while(packet_lane->datagram->route()!=DatagramVideo::Route::Datagram&&Clock::now()<until)Sleep(1);
        if(packet_lane->datagram->route()!=DatagramVideo::Route::Datagram||left.impl_->find(id,true)!=packet_lane||
           right.impl_->find("lan-a",true)!=receiver){result="Recovery did not restore UDP on the same connection";return false;}
        {std::lock_guard lock(guard);frame=false;}
        ++picture.sequence;picture.keyframe=false;
        if(left.send_frame(id,picture)){result="Recovered UDP accepted dependent TCP reference";return false;}
        picture.keyframe=true;
        if(!left.send_frame(id,picture)){result="Recovered UDP IDR rejected";return false;}
        {std::unique_lock lock(guard);if(!changed.wait_for(lock,std::chrono::seconds(2),[&]{return frame;})){result="Recovered UDP picture not delivered";return false;}}
        // An old/duplicate barrier cannot reopen a fenced receive direction.
        receiver->datagram->tcp_received();
        if(receiver->datagram->resume_received(1)){result="Old UDP recovery barrier accepted";return false;}
        Transport legacy;const auto old_key=generate_group_key();
        if(!legacy.start({"legacy","Legacy",old_key,0,false},{},error)||legacy.connect("127.0.0.1",right.impl_->cfg.port,id,error))ok=false;
        if(!ok){result="Keyless encrypted channels, bidirectional version, live rename, or legacy isolation failed";return false;}
    }
    {
        auto hello=make_hello({"feature-test","Features","Test1234",47980,false},0);
        Peer peer;uint8_t channel=9;
        if(!parse_hello(hello,peer,channel)||!(peer.features&FeatureDesktopAux)||!(peer.features&FeatureAutomaticStream)||!(peer.features&FeatureHostKick)||!(peer.features&FeatureAutoRate)||!(peer.features&FeatureDedicatedInput)||!(peer.features&FeatureFrameTiming)||!(peer.features&FeatureCursorComposition)||!(peer.features&FeatureLivePeek)||(peer.features&32768)){result="Feature negotiation failed";return false;}
        std::fill(hello.end()-32,hello.end(),uint8_t(42));peer={};
        if(!parse_hello(hello,peer,channel)||peer.features){result="Legacy challenge compatibility failed";return false;}
        auto input_hello=make_hello({"feature-test","Features","Test1234",47980,false},2);peer={};
        if(!parse_hello(input_hello,peer,channel)||channel!=2){result="Dedicated input channel capability negotiation failed";return false;}
        input_hello[input_hello.size()-21]&=uint8_t(0xffu^FeatureDedicatedInput);peer={};
        if(parse_hello(input_hello,peer,channel)){result="Legacy peer accepted a dedicated input channel";return false;}
    }
    std::string key = generate_group_key(); if (key.empty()) { result = "System random generator failed."; return false; }
    Key expected{},derived{},legacy{},upper{},invalid{};
    decode_key("3c1602eaf0017fef59a19ed8e27de9fe448a8a9ffdee338b784b484aaf6b3040",expected);
    const std::string legacy_text(64,'a');decode_key(legacy_text,legacy);
    if(key.size()!=8||!valid_network_key(key)||!derive_network_key("Studio42",derived)||!equal_key(expected,derived)||
       !derive_network_key("studio42",upper)||equal_key(upper,derived)||
       !derive_network_key(legacy_text,derived)||!equal_key(legacy,derived)||
       !derive_network_key(std::string(64,'A'),upper)||!equal_key(legacy,upper)||
       derive_network_key("",invalid)||derive_network_key("abc",invalid)||derive_network_key("with space",invalid)||
       derive_network_key(std::string(65,'x'),invalid)||derive_network_key(std::string("abcd\0x",6),invalid)) {
        result="Network Key derivation, case sensitivity, legacy compatibility or validation failed.";return false;
    }
    key[0]='A'; // A case-only difference must fail the actual network handshake.
    // Exercise the real Windows GCM implementation, including tag and AAD tampering.
    Key material{}; if(!derive_network_key(key,material)){result="Network Key derivation failed.";return false;}
    Cipher cipher; if (!cipher.init(material)) { result = "AES-GCM initialization failed."; return false; }
    Bytes aad; u32(aad,3); u64(aad,1); Bytes plain{1,2,3}, encrypted, decrypted; std::array<uint8_t,16> tag{};
    if (!cipher.crypt(true,aad,plain,encrypted,tag) || !cipher.crypt(false,aad,encrypted,decrypted,tag) || decrypted != plain) { result = "AES-GCM roundtrip failed."; return false; }
    auto badtag = tag; badtag[0] ^= 1; if (cipher.crypt(false,aad,encrypted,decrypted,badtag)) { result = "Tampered AES-GCM tag accepted."; return false; }
    auto bad_aad = aad; bad_aad.back() ^= 1; if (cipher.crypt(false,bad_aad,encrypted,decrypted,tag)) { result = "Tampered packet counter accepted."; return false; }
    std::mutex mutex; std::condition_variable cv; int state = 0; bool received_frame = false, returned_text = false, input_order = false, got_stop = false, got_release = false, got_uncapped = false;
    Transport a,b,wrong;
    bool got_snapshot=false,got_cursor=false,got_diagnostics=false,got_encode_diagnostics=false,got_host_ended=false;int snapshot_count=0,cursor_count=0;bool got_warm=false,got_warm_cancel=false;
    TransportCallbacks bcb;
    bcb.clipboard = [&](const std::string&,const std::wstring& s) { std::lock_guard lock(mutex); if (s == L"Hive \x4e2d\x6587 \xd83d\xde80") state = 1; cv.notify_all(); };
    bcb.input = [&](const std::string&,const InputEvent& e) { std::lock_guard lock(mutex); if (e.kind == InputKind::ReleaseAll) got_release = true; else { input_order = state == 1 && e.kind == InputKind::Key && e.a == 30; state = 2; } cv.notify_all(); };
    bcb.start_stream = [&](const std::string&,const StreamSettings& s) { std::lock_guard lock(mutex); if(s.warm){got_warm=s.session==919&&s.receiver_millihz==100000;cv.notify_all();return;} if (state == 2 && s.fps == 237 && s.bitrate_mbps == 80) state = 3; else if (state == 3 && s.fps == 0 && s.bitrate_mbps == 80) got_uncapped = true; cv.notify_all(); };
    bcb.cancel_warm=[&](const std::string&,uint64_t session){std::lock_guard lock(mutex);got_warm_cancel=session==919;cv.notify_all();};
    bcb.stop_stream = [&](const std::string&) { std::lock_guard lock(mutex); got_stop = true; cv.notify_all(); };
    bcb.frame = [&](const std::string&,EncodedFrame f) { std::lock_guard lock(mutex); received_frame = f.width == 1920 && f.height == 1080 && f.sequence == 42 && f.capture_time_us == 123456 && f.source_ready_us == 124500 && f.session == 77 && f.keyframe && f.cursor_composition==CursorComposition::NoSeparate && f.data == Bytes({0,0,0,1,42,99}) && f.receive_started_us>1 && f.receive_completed_us>=f.receive_started_us && f.receive_progress && f.receive_progress->load()>=f.data.size(); cv.notify_all(); };
    TransportCallbacks acb; acb.clipboard = [&](const std::string&,const std::wstring& s) { std::lock_guard lock(mutex); returned_text = s == L"return"; cv.notify_all(); };
    bool got_return=false;
    bcb.return_request=[&](const std::string& peer,const ReturnRequest& r){b.send_return_reply(peer,{r.session,r.id,ReturnResult::Leaf});};
    acb.return_reply=[&](const std::string& peer,const ReturnReply& r){std::lock_guard lock(mutex);got_return=peer=="test-b"&&r.session==91&&r.id==17&&r.result==ReturnResult::Leaf;cv.notify_all();};
    acb.host_ended=[&](const std::string& peer,uint64_t session){std::lock_guard lock(mutex);got_host_ended=peer=="test-b"&&session==91;cv.notify_all();};
    acb.diagnostics=[&](const std::string&,const SourceDiagnostics& d){std::lock_guard lock(mutex);const bool base=d.session==91&&d.encoded_millihz==59940&&d.encode_us==1234&&d.capture_wait_us==5678&&d.send_wait_us_per_second==9000&&d.feedback_age_us==11000&&d.network_extra_us==13000;if(d.encode_details)got_encode_diagnostics=base&&d.color_us==500&&d.nvenc_us==734&&d.encode_age_us==10000;else got_diagnostics=base;cv.notify_all();};
    bcb.snapshot_request=[&](const std::string& peer,uint64_t request,bool native){b.send_snapshot(peer,{request,native?Bytes(1200*1024,42):Bytes({0xff,0xd8,42,0xff,0xd9}),native});};
    acb.snapshot=[&](const std::string&,Snapshot snapshot){std::lock_guard lock(mutex);++snapshot_count;got_snapshot=snapshot.request==901&&snapshot.native&&snapshot.jpeg==Bytes(1200*1024,42);cv.notify_all();};
    acb.cursor=[&](const std::string&,CursorState cursor){std::lock_guard lock(mutex);++cursor_count;
        got_cursor=cursor.session==91&&cursor.input_capture_us==123456&&cursor.position_valid&&cursor.position_revision==77&&cursor.x==12345&&cursor.y==54321&&
            (cursor_count==1?(cursor.visible&&cursor.width==2&&cursor.hotspot_x==1&&cursor.mask==Bytes(8,0xff)):
             (!cursor.visible&&cursor.width==0));cv.notify_all();};
    std::string error,id; TransportConfig acfg{"test-a","Transport A",key,0,false}, bcfg{"test-b","Transport B",key,0,false};
    acfg.datagram_video=false; // Mixed capability must retain the exact TCP timing/packet contract.
    if (!a.start(acfg,acb,error) || !b.start(bcfg,bcb,error) || !a.connect("127.0.0.1",b.impl_->cfg.port,id,error) || id != "test-b") { result = "Loopback connection failed: " + error; return false; }
    if(!a.connect("127.0.0.1",b.impl_->cfg.port,id,error)||id!="test-b"||
       !b.connect("127.0.0.1",a.impl_->cfg.port,id,error)||id!="test-a") {
        result="Connecting by IP did not reuse the existing authenticated pair.";return false;
    }
    std::string wrong_key=key;wrong_key[0]='a';
    TransportConfig wcfg{"test-wrong","Wrong key",wrong_key,0,false};
    if (!wrong.start(wcfg,{},error)) { result = "Wrong-key test setup failed."; return false; }
    if (wrong.connect("127.0.0.1",b.impl_->cfg.port,id,error) || !wrong.peers().empty()) { result = "Wrong Network Key was accepted."; return false; }
    CursorState cursor;cursor.session=91;cursor.input_capture_us=123456;cursor.position_valid=true;cursor.x=12345;cursor.y=54321;cursor.position_revision=77;cursor.width=2;cursor.height=1;cursor.hotspot_x=1;cursor.mask=Bytes(8,0xff);
    bool ok=a.send_return_request("test-b",{91,17,15})&&a.request_snapshot("test-b",901)&&b.send_cursor("test-a",cursor)&&b.send_diagnostics("test-a",{91,59940,1234,5678,9000,11000,13000});
    // Check both wire formats after delivery. Unsent diagnostic states now
    // intentionally coalesce; the separate queue test verifies that behavior.
    {std::unique_lock lock(mutex);ok=cv.wait_for(lock,std::chrono::seconds(5),[&]{return got_diagnostics;})&&ok;}
    ok=b.send_diagnostics("test-a",{91,59940,1234,5678,9000,11000,13000,500,734,10000,true})&&ok;
    {std::unique_lock lock(mutex);ok=cv.wait_for(lock,std::chrono::seconds(5),[&]{return got_return&&got_snapshot&&got_cursor&&got_diagnostics&&got_encode_diagnostics;})&&ok;
        ok=ok&&state==0&&snapshot_count==1;}
    if(!ok){result="Auxiliary loopback failed: snapshot="+std::to_string(got_snapshot)+" cursor="+std::to_string(got_cursor)+" diagnostics="+std::to_string(got_diagnostics)+" encode="+std::to_string(got_encode_diagnostics);return false;}
    cursor.width=cursor.height=cursor.hotspot_x=0;cursor.mask.clear();cursor.visible=false;
    ok=b.send_cursor("test-a",cursor)&&ok;
    ok = a.send_clipboard("test-b",L"Hive \x4e2d\x6587 \xd83d\xde80") && a.send_input("test-b",{InputKind::Key,30,0,0}) && a.request_stream("test-b",{237,80}) && a.request_stream("test-b",{0,80}) && ok;
    EncodedFrame frame; frame.width = 1920; frame.height = 1080; frame.sequence = 42; frame.capture_time_us = 123456; frame.source_ready_us = 124500; frame.session = 77; frame.keyframe = true; frame.cursor_composition=CursorComposition::NoSeparate; frame.data = {0,0,0,1,42,99}; frame.receive_started_us=1;frame.receive_completed_us=UINT64_MAX;
    const auto token_before_kick=a.connection_token("test-b");
    ok = ok && a.send_frame("test-b",frame) && b.send_clipboard("test-a",L"return") && b.end_stream("test-a",91); a.stop_stream("test-b");
    {
        std::unique_lock lock(mutex); ok = cv.wait_for(lock,std::chrono::seconds(5),[&] { return state == 3 && got_uncapped && input_order && received_frame && returned_text && got_stop && got_host_ended && got_cursor && cursor_count==2; }) && ok;
    }
    ok=ok&&token_before_kick&&a.connection_token("test-b")==token_before_kick&&a.frame_ready("test-b")&&b.frame_ready("test-a");
    if(!ok){result="Control/video loopback failed: state="+std::to_string(state)+" frame="+std::to_string(received_frame)+" text="+std::to_string(returned_text)+" stop="+std::to_string(got_stop)+" ended="+std::to_string(got_host_ended)+" cursor_count="+std::to_string(cursor_count)+" a_ready="+std::to_string(a.frame_ready("test-b"))+" b_ready="+std::to_string(b.frame_ready("test-a"));return false;}
    {
        const auto deadline=Clock::now()+std::chrono::milliseconds(500);
        auto timing=a.video_send_diagnostics("test-b");
        while(timing.available&&!timing.completed_packets&&Clock::now()<deadline){Sleep(1);timing=a.video_send_diagnostics("test-b");}
        if(!timing.available||!timing.connection||!timing.submitted_packets||!timing.completed_packets||timing.completed_bytes<frame.data.size()+28||
            timing.sample_age_us==UINT32_MAX||timing.failed_packets||a.video_send_diagnostics("missing-peer").available){
            result="Encrypted video loopback did not expose valid local sender timings";return false;
        }
    }
    StreamSettings warm_settings;warm_settings.session=919;warm_settings.receiver_millihz=100000;warm_settings.warm=true;
    auto invalid_warm=warm_settings;invalid_warm.session=0;
    ok=!a.request_stream("test-b",invalid_warm)&&a.request_stream("test-b",warm_settings)&&a.cancel_warm("test-b",919)&&!a.cancel_warm("test-b",0)&&ok;
    {std::unique_lock lock(mutex);ok=cv.wait_for(lock,std::chrono::seconds(5),[&]{return got_warm&&got_warm_cancel;})&&ok;}
    std::wstring oversized(kMaxClipboard/2+1,L'x'), malformed(1,wchar_t(0xd800));
    ok=ok&&!a.request_stream("test-b",{0,0,0,1})&&!a.request_stream("test-b",{0,0,2000001,1})&&
        !a.send_health("test-b",{1,60000,60001,0})&&!a.send_health("test-b",{1,60000,60000,8})&&!a.send_feedback("test-b",{})&&!a.send_diagnostics("test-b",{})&&!a.send_diagnostics("test-b",{1,4000001})&&!a.send_diagnostics("test-b",{1,60000,0,0,1000001});
    ok = ok && !a.send_clipboard("test-b",oversized) && !a.send_clipboard("test-b",malformed) && !a.send_input("test-b",{InputKind(999),0,0,0});
    frame.data.resize(kMaxFrame+1); ok = ok && !a.send_frame("test-b",frame);
    cursor.width=257;ok=ok&&!b.send_cursor("test-a",cursor)&&!a.request_snapshot("test-b",0)&&!b.send_snapshot("test-a",{901,Bytes(MaxSnapshotBytes+1)});
    if(!ok){result="Warm cancellation or malformed-payload bounds failed";return false;}
    {
        std::shared_ptr<Impl::Conn> lane;{std::lock_guard lock(a.impl_->mutex);lane=a.impl_->connected.at("test-b").clipboard;}
        if(!lane){result="Missing negotiated clipboard lane";return false;}const auto connection=a.connection_token("test-b");lane->close();
        const auto deadline=Clock::now()+std::chrono::seconds(2);
        while(Clock::now()<deadline){bool gone=false;{std::lock_guard lock(b.impl_->mutex);const auto& pair=b.impl_->connected.at("test-a");gone=!pair.clipboard||pair.clipboard->closed;}if(gone)break;Sleep(1);}
        if(!a.send_clipboard_packet("test-b",Bytes{99})||a.connection_token("test-b")!=connection){result="Clipboard lane repair disrupted desktop connection";return false;}
    }
    auto before = std::chrono::steady_clock::now(); wrong.stop(); ok = a.send_input("test-b",{InputKind::ReleaseAll}) && ok; a.stop();
    { std::unique_lock lock(mutex); ok = cv.wait_for(lock,std::chrono::milliseconds(500),[&] { return got_release; }) && ok; }
    b.stop();
    ok = ok && std::chrono::steady_clock::now()-before < std::chrono::seconds(3);
    if(!ok){result="Shutdown release/deadline failed: release="+std::to_string(got_release);return false;}
    {
        // Real encrypted packets of different lengths must stay in posting
        // order while two overlapped writes share the stream. No desktop input.
        Transport sender,receiver;std::mutex guard;std::condition_variable changed;
        unsigned received=0;bool ordered=true;TransportCallbacks callbacks;
        callbacks.frame=[&](const std::string&,EncodedFrame frame){
            std::lock_guard lock(guard);
            const size_t expected=received%2?65537:1024;
            ordered=ordered&&frame.sequence==received+1&&frame.data.size()==expected&&
                std::all_of(frame.data.begin(),frame.data.end(),[&](uint8_t b){return b==uint8_t(received);});
            ++received;changed.notify_all();
        };
        std::string failure,peer;
        if(!sender.start({"ordered-a","Ordered sender","",0,false,true,false},{},failure)||
            !receiver.start({"ordered-b","Ordered receiver","",0,false,true},callbacks,failure)||
            !sender.connect("127.0.0.1",receiver.impl_->cfg.port,peer,failure)){
            result="Overlapped ordering setup failed: "+failure;return false;
        }
        const auto deadline=Clock::now()+std::chrono::seconds(3);bool sent=true;
        for(unsigned i=0;i<32&&sent;++i){
            while(!sender.frame_ready(peer)&&Clock::now()<deadline)Sleep(1);
            EncodedFrame ordered_frame;ordered_frame.width=ordered_frame.height=2;ordered_frame.keyframe=true;ordered_frame.sequence=i+1;
            ordered_frame.data.assign(i%2?65537:1024,uint8_t(i));
            sent=Clock::now()<deadline&&sender.send_frame(peer,ordered_frame);
        }
        {std::unique_lock lock(guard);sent=changed.wait_until(lock,deadline,[&]{return received==32;})&&sent&&ordered;}
        sender.stop();receiver.stop();
        if(!sent){result="Overlapped encrypted video lost packet order, bytes or completion";return false;}
    }
    {
        // The receiving parser deliberately stops draining the video socket
        // after one valid encrypted packet. A full-size subsequent packet must
        // hit the bounded write deadline, cancel safely and tear down both
        // channels, rather than queue old motion for later playback.
        std::mutex guard;std::condition_variable changed;bool blocked=false,release=false,disconnected=false;unsigned frames=0;
        Transport sender,receiver;
        struct Unblock {std::mutex& guard;std::condition_variable& changed;bool& release;~Unblock(){std::lock_guard lock(guard);release=true;changed.notify_all();}} unblock{guard,changed,release};
        TransportCallbacks sending,receiving;
        sending.peer=[&](const Peer& peer){if(!peer.authenticated){std::lock_guard lock(guard);disconnected=true;changed.notify_all();}};
        receiving.frame=[&](const std::string&,EncodedFrame){
            std::unique_lock lock(guard);++frames;blocked=true;changed.notify_all();
            changed.wait_for(lock,SessionStallTimeout+std::chrono::seconds(3),[&]{return release;});
        };
        std::string failure,peer;
        if(!sender.start({"stalled-a","Stalled sender","",0,false,true,false},sending,failure)||
            !receiver.start({"stalled-b","Stalled receiver","",0,false,true},receiving,failure)||
            !sender.connect("127.0.0.1",receiver.impl_->cfg.port,peer,failure)){
            result="Stalled video setup failed: "+failure;return false;
        }
        auto video=receiver.impl_->find("stalled-a",true);
        const int receive_buffer=4096;
        if(!video||setsockopt(video->socket.load(),SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&receive_buffer),sizeof(receive_buffer))){
            result="Stalled video receive buffer setup failed";return false;
        }
        EncodedFrame initial;initial.width=initial.height=2;initial.keyframe=true;initial.data={1,2,3};
        bool stalled_ok=sender.send_frame("stalled-b",initial);
        {std::unique_lock lock(guard);stalled_ok=changed.wait_for(lock,std::chrono::seconds(1),[&]{return blocked;})&&stalled_ok;}
        EncodedFrame large=initial;large.sequence=1;large.data.resize(kMaxFrame,42);
        const auto began=Clock::now();stalled_ok=sender.send_frame("stalled-b",large)&&stalled_ok;
        auto sending_video=sender.impl_->find("stalled-b",true);
        const auto wait_flight=[&](unsigned count){
            while(sending_video&&sending_video->video_inflight<count&&Clock::now()-began<std::chrono::milliseconds(500))Sleep(1);
            return sending_video&&sending_video->video_inflight==count;
        };
        stalled_ok=wait_flight(1)&&stalled_ok;
        large.sequence=2;stalled_ok=sender.send_frame("stalled-b",large)&&stalled_ok;
        stalled_ok=wait_flight(2)&&stalled_ok;
        // A third frame may occupy only the single unsent slot, never Winsock.
        initial.sequence=3;stalled_ok=sender.send_frame("stalled-b",initial)&&stalled_ok;
        stalled_ok=!sender.frame_ready("stalled-b")&&sending_video&&sending_video->video_inflight==2&&stalled_ok;
        {std::unique_lock lock(guard);
            stalled_ok=!changed.wait_for(lock,std::chrono::milliseconds(400),[&]{return disconnected;})&&stalled_ok;
            stalled_ok=changed.wait_for(lock,SessionStallTimeout+std::chrono::seconds(1),[&]{return disconnected;})&&stalled_ok;release=true;changed.notify_all();}
        const auto elapsed=Clock::now()-began;
        stalled_ok=stalled_ok&&elapsed>=SessionStallTimeout&&elapsed<SessionStallTimeout+std::chrono::seconds(1)&&!sender.connection_token("stalled-b");
        sender.stop();receiver.stop();
        if(sending_video){const auto stats=sending_video->video_send.snapshot(transport_us());
            stalled_ok=stalled_ok&&stats.submitted_packets==3&&stats.completed_packets==1&&stats.failed_packets==2&&!sending_video->video_inflight;}
        {std::lock_guard lock(guard);stalled_ok=stalled_ok&&frames==1;}
        if(!stalled_ok){result="Stalled encrypted video did not cancel within its deadline, close both channels, or discard unsent replay";return false;}
    }
    result = ok ? "PASS: dedicated encrypted input with clipboard/start handshake, eight completed-injection credits, bounded edge-safe motion merge, submitted-send/ACK latency metrics, blocked-control independence, session finish without replay, negative/missing ACK recovery, and reciprocal input preserved across an urgent kick plus late control notice; legacy input/capability compatibility; timed-video source completion, both mixed-version directions, malformed timing bounds and 32-bit feature advertisement; short Network Key derivation/reference vector, authenticated three-channel loopback, wrong-key and AES-GCM tamper rejection; stream/frame/diagnostic bounds; host kick/session isolation; bounded shutdown; 50 ms unsent-video expiry and IDR recovery; local WSASend metrics; 400 ms delayed input ACKs preserved; stalled 16 MiB video writes canceled after the 2 s watchdog." : "Transport loopback, ordering, bounds, or shutdown failed.";
    return ok;
}
}
