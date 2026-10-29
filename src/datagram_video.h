#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "hivedesk/video_send_diagnostics.h"

namespace hive {
// One authenticated, full-duplex UDP lane bound to a specific TCP video
// handshake. Control/input never share this lane. No public configuration knobs.
class DatagramVideo {
public:
    using Bytes=std::vector<uint8_t>;
    struct Hooks {
        std::function<void(Bytes,uint64_t,uint64_t)> picture;
        std::function<bool(const Bytes&)> receive_ready;
        std::function<void()> credit,keyframe;
        // Queue an ordered TCP video barrier after all previously submitted
        // frames. New media is paused until the peer acknowledges this epoch.
        std::function<bool(uint64_t)> resume;
        std::function<void(const std::string&)> status;
    };
    enum class Route { Negotiating, Datagram, Tcp, Recovering };
    DatagramVideo(const std::string& address,const std::array<uint8_t,32>& pair_key,bool outbound,
                  std::shared_ptr<std::atomic<uint64_t>> progress,Hooks hooks,const std::string& local_address="0.0.0.0",bool recovery=false);
    ~DatagramVideo();
    DatagramVideo(const DatagramVideo&)=delete;
    DatagramVideo& operator=(const DatagramVideo&)=delete;
    uint16_t port() const;
    bool offer(uint16_t port);
    void start();
    void stop(); // Non-blocking; destruction joins. Hooks never run under state locks.
    void join();
    Route route() const;
    bool ready() const;
    bool send(Bytes bytes,bool key,uint64_t session);
    bool accept_tcp(bool key);
    void cancel(uint64_t session);
    void tcp_received(); // Fence UDP until a newer ordered recovery barrier.
    bool resume_received(uint64_t epoch);
    std::string diagnostics() const;
    void diagnostics(VideoSendDiagnostics&) const;
    static bool self_test(std::string& result);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
