#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <bcrypt.h>
#include "datagram_video.h"
#include "datagram_window.h"
#include "frame_timer.h"
#include <mutex>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace hive {
namespace {
using Bytes=DatagramVideo::Bytes;
using Key=std::array<uint8_t,32>;
uint64_t now_us(){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(FrameTimer::Clock::now().time_since_epoch()).count());}
struct Crypto {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_KEY_HANDLE key=nullptr;
    uint64_t sequence=0,highest=0;std::vector<uint64_t> seen=std::vector<uint64_t>(65536);
    ~Crypto(){if(key)BCryptDestroyKey(key);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    bool init(const Key& root,bool dialer) {
        // Independent labels and keys from TCP, the reverse direction, and every
        // other connection. The pair root binds the complete ECDH transcript.
        const char* label=dialer?"HiveDesk.Datagram.v1.dialer":"HiveDesk.Datagram.v1.listener";
        BCRYPT_ALG_HANDLE hash=nullptr;Key material{};
        bool ok=BCryptOpenAlgorithmProvider(&hash,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG)>=0;
        if(ok)ok=BCryptHash(hash,const_cast<PUCHAR>(root.data()),ULONG(root.size()),
            reinterpret_cast<PUCHAR>(const_cast<char*>(label)),ULONG(strlen(label)),material.data(),ULONG(material.size()))>=0;
        if(hash)BCryptCloseAlgorithmProvider(hash,0);
        if(ok)ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_AES_ALGORITHM,nullptr,0)>=0&&
            BCryptSetProperty(algorithm,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),sizeof(BCRYPT_CHAIN_MODE_GCM),0)>=0&&
            BCryptGenerateSymmetricKey(algorithm,&key,nullptr,0,material.data(),ULONG(material.size()),0)>=0;
        SecureZeroMemory(material.data(),material.size());return ok;
    }
    bool crypt(bool encrypt,const Bytes& header,const Bytes& source,Bytes& target,std::array<uint8_t,16>& tag) {
        std::array<uint8_t,12> nonce{};std::copy(header.end()-8,header.end(),nonce.begin()+4);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce=nonce.data();info.cbNonce=ULONG(nonce.size());info.pbAuthData=const_cast<PUCHAR>(header.data());info.cbAuthData=ULONG(header.size());info.pbTag=tag.data();info.cbTag=ULONG(tag.size());
        target.resize(source.size());ULONG done=0;
        auto status=encrypt?BCryptEncrypt(key,const_cast<PUCHAR>(source.data()),ULONG(source.size()),&info,nullptr,0,target.data(),ULONG(target.size()),&done,0):
            BCryptDecrypt(key,const_cast<PUCHAR>(source.data()),ULONG(source.size()),&info,nullptr,0,target.data(),ULONG(target.size()),&done,0);
        return status>=0&&done==source.size();
    }
    bool seal(const Bytes& plain,Bytes& wire) {
        if(sequence==UINT64_MAX||plain.empty()||plain.size()>1160)return false;
        Bytes header{'H','D','U',1};datagram::put(header,++sequence,8);Bytes encrypted;std::array<uint8_t,16>tag{};
        if(!crypt(true,header,plain,encrypted,tag))return false;
        wire=std::move(header);wire.insert(wire.end(),encrypted.begin(),encrypted.end());wire.insert(wire.end(),tag.begin(),tag.end());return true;
    }
    bool open(const Bytes& wire,Bytes& plain) {
        if(wire.size()<29||wire.size()>1188||wire[0]!='H'||wire[1]!='D'||wire[2]!='U'||wire[3]!=1)return false;
        datagram::Reader r{wire,4};const auto n=r.get(8);
        if(!n||(highest>=n&&highest-n>=seen.size())||seen[n%seen.size()]==n)return false;
        Bytes header(wire.begin(),wire.begin()+12),encrypted(wire.begin()+12,wire.end()-16);std::array<uint8_t,16>tag{};
        std::copy(wire.end()-16,wire.end(),tag.begin());if(!crypt(false,header,encrypted,plain,tag))return false;
        // Never let an unauthenticated counter advance the replay window.
        seen[n%seen.size()]=n;highest=std::max(highest,n);return true;
    }
};
}

struct DatagramVideo::Impl {
    Hooks hooks;std::shared_ptr<std::atomic<uint64_t>> progress;
    SOCKET socket=INVALID_SOCKET;WSAEVENT readable=WSA_INVALID_EVENT;uint16_t local_port=0,remote_port=0;
    sockaddr_in remote{};Crypto tx,rx;FrameTimer timer;
    mutable std::mutex mutex;std::mutex delivery_mutex;datagram::Window window;
    std::atomic<bool> stopping=false;bool receive_tcp=false,offered=false,tcp_key=false;Route mode=Route::Negotiating;
    std::thread worker;uint64_t started=0,last_hello=0,last_received=0,invalid=0;
    Impl(const std::string& address,const Key& root,bool outbound,std::shared_ptr<std::atomic<uint64_t>> bytes,Hooks h):hooks(std::move(h)),progress(std::move(bytes)) {
        remote.sin_family=AF_INET;
        if(inet_pton(AF_INET,address.c_str(),&remote.sin_addr)!=1||!tx.init(root,outbound)||!rx.init(root,!outbound))throw std::runtime_error("UDP video key initialization failed");
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);readable=WSACreateEvent();sockaddr_in local{};local.sin_family=AF_INET;int length=sizeof(local);
        if(socket==INVALID_SOCKET||readable==WSA_INVALID_EVENT||bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))||
            getsockname(socket,reinterpret_cast<sockaddr*>(&local),&length)||WSAEventSelect(socket,readable,FD_READ)) {
            if(socket!=INVALID_SOCKET)closesocket(socket);socket=INVALID_SOCKET;if(readable!=WSA_INVALID_EVENT)WSACloseEvent(readable);readable=WSA_INVALID_EVENT;
            throw std::runtime_error("UDP video socket initialization failed");
        }
        local_port=ntohs(local.sin_port);int receive_buffer=4*1024*1024,send_buffer=256*1024;
        setsockopt(socket,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&receive_buffer),sizeof(receive_buffer));
        setsockopt(socket,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&send_buffer),sizeof(send_buffer));
        BOOL reset=FALSE;DWORD returned=0;WSAIoctl(socket,SIO_UDP_CONNRESET,&reset,sizeof(reset),nullptr,0,&returned,nullptr,nullptr);
    }
    ~Impl(){stop();if(worker.joinable())worker.join();if(socket!=INVALID_SOCKET)closesocket(socket);if(readable!=WSA_INVALID_EVENT)WSACloseEvent(readable);}
    void stop(){stopping=true;timer.notify();}
    void say(const std::string& text){if(hooks.status)hooks.status(text);}
    bool fallback_locked() {if(mode==Route::Tcp)return false;mode=Route::Tcp;window.abandon();return true;}
    void send_wire(const Bytes& plain) {
        Bytes wire;if(!tx.seal(plain,wire))throw std::runtime_error("UDP video encryption failed");
        const auto sent=sendto(socket,reinterpret_cast<const char*>(wire.data()),int(wire.size()),0,reinterpret_cast<sockaddr*>(&remote),sizeof(remote));
        // Nonblocking send pressure is packet loss, recovered through the same
        // bounded ACK/RTO loop. Never spin on a full kernel buffer.
        if(sent==SOCKET_ERROR) {const auto code=WSAGetLastError();if(code!=WSAEWOULDBLOCK&&code!=WSAENOBUFS&&code!=WSAECONNRESET)throw std::runtime_error("UDP video send failed");}
    }
    void run() {
        started=now_us();
        try {
            while(!stopping) {
                bool credit=false,key=false,fallback=false,activated=false;uint64_t wake=0;
                {
                    std::lock_guard lock(mutex);const auto now=now_us();
                    const bool before=mode==Route::Datagram&&window.ready();
                    WSANETWORKEVENTS events{};WSAEnumNetworkEvents(socket,readable,&events);
                    // Bounded batches keep stop/cancel/producer work responsive.
                    for(unsigned i=0;i<512;++i) {
                        std::array<uint8_t,1500> buffer{};sockaddr_in from{};int size=sizeof(from);
                        const int n=recvfrom(socket,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&from),&size);
                        if(n<0)break;
                        if(!remote_port||from.sin_addr.s_addr!=remote.sin_addr.s_addr||from.sin_port!=remote.sin_port)continue;
                        Bytes plain;if(!rx.open(Bytes(buffer.begin(),buffer.begin()+n),plain)){++invalid;continue;}
                        if(receive_tcp&&plain[0]==datagram::Window::Data)continue;
                        const auto bytes=window.stats().received_bytes;
                        if(!window.receive(plain,now)){++invalid;continue;}
                        progress->fetch_add(window.stats().received_bytes-bytes,std::memory_order_relaxed);last_received=now;
                        if(mode==Route::Negotiating){mode=Route::Datagram;activated=true;}
                    }
                    if(mode==Route::Negotiating&&now-started>=3000000)fallback=fallback_locked();
                    if(remote_port) {
                        if(now-last_hello>=(mode==Route::Negotiating?100000:1000000)) {send_wire(Bytes{datagram::Window::Hello});last_hello=now;}
                        const auto prior=window.stats();
                        for(unsigned i=0;i<256;++i){auto packet=window.poll(now);if(!packet)break;send_wire(*packet);}
                        if(prior.owned&&window.stats().expired>prior.expired)fallback=fallback_locked();
                    }
                    key=window.take_key_request();
                    credit=activated||fallback||(!before&&mode==Route::Datagram&&window.ready());
                    wake=std::min(window.next_wake(now),now+1000000);
                    if(mode==Route::Negotiating)wake=std::min(wake,now+100000);
                }
                if(activated)say("Video packet transport ready (authenticated UDP).");
                if(fallback)say("Video packet path unavailable; continuing with TCP and a fresh reference frame.");
                if(key&&hooks.keyframe)hooks.keyframe();if(credit&&hooks.credit)hooks.credit();
                for(;;) {
                    // Fallback and delivery serialize, so the last UDP callback
                    // cannot overtake the first TCP frame on another reader.
                    std::lock_guard delivery(delivery_mutex);
                    if(receive_tcp||stopping)break;
                    const datagram::Window::Picture* pending=nullptr;
                    {std::lock_guard lock(mutex);pending=window.front_picture();}
                    if(!pending)break;
                    // Only this worker changes the receive queue; TCP fencing
                    // holds delivery_mutex. Query consumer capacity outside the
                    // transport state lock and keep ACK/pacing work independent.
                    if(hooks.receive_ready&&!hooks.receive_ready(pending->bytes)){
                        wake=std::min(wake,now_us()+1000);break;
                    }
                    std::optional<datagram::Window::Picture> picture;
                    {std::lock_guard lock(mutex);picture=window.take_picture();}
                    if(picture&&hooks.picture)hooks.picture(std::move(picture->bytes),picture->started,picture->completed);
                }
                timer.wait_until(FrameTimer::Clock::time_point(std::chrono::microseconds(wake)),readable);
            }
        } catch(const std::exception& e) {
            {std::lock_guard lock(mutex);fallback_locked();}
            try{say(e.what());if(hooks.keyframe)hooks.keyframe();if(hooks.credit)hooks.credit();}catch(...){}
        }
    }
};

DatagramVideo::DatagramVideo(const std::string& address,const Key& key,bool outbound,std::shared_ptr<std::atomic<uint64_t>> progress,Hooks hooks):
    impl_(std::make_unique<Impl>(address,key,outbound,std::move(progress),std::move(hooks))){}
DatagramVideo::~DatagramVideo()=default;
uint16_t DatagramVideo::port()const{return impl_->local_port;}
bool DatagramVideo::offer(uint16_t port){
    {std::lock_guard lock(impl_->mutex);if(impl_->offered)return false;impl_->offered=true;
        if(!port)impl_->fallback_locked();else {impl_->remote_port=port;impl_->remote.sin_port=htons(port);}}
    impl_->timer.notify();if(!port&&impl_->hooks.credit)impl_->hooks.credit();return true;
}
void DatagramVideo::start(){
    try{impl_->worker=std::thread([p=impl_.get()]{p->run();});}
    catch(const std::exception&){
        {std::lock_guard lock(impl_->mutex);impl_->fallback_locked();}
        if(impl_->hooks.keyframe)impl_->hooks.keyframe();if(impl_->hooks.credit)impl_->hooks.credit();
    }
}
void DatagramVideo::stop(){impl_->stop();}
void DatagramVideo::join(){
    if(impl_->worker.joinable())impl_->worker.join();
    // Release sockets before Transport's final WSACleanup, not during a later
    // shared connection destructor. No worker can access these handles now.
    if(impl_->socket!=INVALID_SOCKET){closesocket(impl_->socket);impl_->socket=INVALID_SOCKET;}
    if(impl_->readable!=WSA_INVALID_EVENT){WSACloseEvent(impl_->readable);impl_->readable=WSA_INVALID_EVENT;}
}
DatagramVideo::Route DatagramVideo::route()const{std::lock_guard lock(impl_->mutex);return impl_->mode;}
bool DatagramVideo::ready()const{std::lock_guard lock(impl_->mutex);return !impl_->stopping&&impl_->mode==Route::Datagram&&impl_->window.ready();}
bool DatagramVideo::send(Bytes bytes,bool key,uint64_t session){
    std::lock_guard lock(impl_->mutex);if(impl_->stopping||impl_->mode!=Route::Datagram)return false;
    const bool ok=impl_->window.enqueue(std::move(bytes),key,session,now_us());if(ok)impl_->timer.notify();return ok;
}
bool DatagramVideo::accept_tcp(bool key){std::lock_guard lock(impl_->mutex);if(impl_->mode!=Route::Tcp)return false;if(key)impl_->tcp_key=true;return impl_->tcp_key;}
void DatagramVideo::cancel(uint64_t session){std::lock_guard lock(impl_->mutex);impl_->window.cancel(session);impl_->timer.notify();}
void DatagramVideo::tcp_received(){std::lock_guard delivery(impl_->delivery_mutex);std::lock_guard lock(impl_->mutex);impl_->receive_tcp=true;impl_->window.discard_received(true);}
std::string DatagramVideo::diagnostics()const{
    std::lock_guard lock(impl_->mutex);auto s=impl_->window.stats();std::ostringstream out;
    out<<" media="<<(impl_->mode==Route::Datagram?"udp":impl_->mode==Route::Tcp?"tcp-fallback":"udp-negotiating")
       <<" udp_tx="<<s.sent<<" rtx="<<s.retransmitted<<" rx="<<s.received<<" invalid="<<impl_->invalid
       <<" rtt_us="<<s.srtt<<" cwnd="<<size_t(s.cwnd)<<" flight="<<s.flight<<" owned="<<s.owned<<" expired="<<s.expired;return out.str();
}
void DatagramVideo::diagnostics(VideoSendDiagnostics& result)const{
    std::lock_guard lock(impl_->mutex);auto s=impl_->window.stats();
    result.packet_transport=impl_->mode==Route::Datagram?2:impl_->mode==Route::Tcp?3:1;
    result.packet_rtt_us=uint32_t(std::min<uint64_t>(s.srtt,UINT32_MAX));result.packet_min_rtt_us=uint32_t(std::min<uint64_t>(s.min_rtt,UINT32_MAX));
    result.packet_window=uint32_t(s.cwnd);result.packet_inflight=uint32_t(s.flight);result.packet_owned_bytes=s.owned;
    result.packet_sent=s.sent;result.packet_retransmitted=s.retransmitted;result.packet_received=s.received;result.packet_invalid=impl_->invalid;result.packet_expired=s.expired;
}
bool DatagramVideo::self_test(std::string& result){
    Key key{};for(size_t i=0;i<key.size();++i)key[i]=uint8_t(i+1);
    Crypto tx,rx,reverse,other;Key other_key=key;other_key[0]^=1;
    if(!tx.init(key,true)||!rx.init(key,true)||!reverse.init(key,false)||!other.init(other_key,true)){result="crypto initialization";return false;}
    Bytes first,second,plain;const Bytes data{1,2,3,4};
    if(!tx.seal(data,first)||!tx.seal(data,second)){result="seal";return false;}
    Bytes damaged=second;damaged[4]=255;
    auto payload=second;payload[12]^=1;auto tag=second;tag.back()^=1;
    if(rx.open(damaged,plain)||rx.open(payload,plain)||rx.open(tag,plain)||reverse.open(first,plain)||other.open(first,plain)||!rx.open(second,plain)||plain!=data||
       !rx.open(first,plain)||plain!=data||rx.open(first,plain)){result="authentication, replay or direction isolation";return false;}
    result="UDP AES-GCM authenticates payload/counter, fences keys/directions, accepts reorder and rejects replay";return true;
}
}
