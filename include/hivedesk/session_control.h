#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace hive {
// Immutable session identity; revocation can close input admission without
// waiting for a desktop broker, GPU teardown, or the outbound command worker.
struct HostControl {
    const std::string peer;
    const uint64_t session, connection;
    std::atomic<bool> revoked{false},cycle_rejected{false};
    std::atomic<bool> warming{false},previewing{false};
    std::atomic<uint64_t> warm_until{0};
    std::atomic<bool> input_enabled{false},input_error_reported{false};
    HostControl(std::string id,uint64_t stream,uint64_t token):peer(std::move(id)),session(stream),connection(token){}
    bool revoke(){return !revoked.exchange(true);}
};
struct ViewControl {
    const std::string peer;
    const uint64_t session, generation;
    ViewControl(std::string id,uint64_t stream,uint64_t intent):peer(std::move(id)),session(stream),generation(intent){}
    // A delayed termination must not cancel a newer selection, even while the
    // old viewer is still published and the new selection waits in the queue.
    bool cancel(const std::string& source,uint64_t stream,std::atomic<uint64_t>& intent)const{
        if(!stream||peer!=source||session!=stream)return false;
        auto expected=generation;
        return intent.compare_exchange_strong(expected,generation+1);
    }
};
}
