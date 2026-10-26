// Real background Desktop Duplication/NVENC pipeline. No visible test pattern,
// screenshots, keyboard/mouse injection or installed service changes.
#include "hivedesk/media.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
static uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}
struct FrameObservation {
    uint64_t sequence = 0, captured_us = 0, arrived_us = 0;
    bool keyframe = false;
};
struct Shared {
    std::mutex mutex;
    std::condition_variable changed;
    bool credit = true, release = false, abort = false, threw = false;
    unsigned callbacks = 0, blocked_checks = 0;
    uint64_t released_us = 0;
    Clock::time_point blocked_at{};
    FrameObservation blocked, resumed;
};
struct Cleanup {
    Shared& state;
    hive::MediaHost& host;
    ~Cleanup() {
        {
            std::lock_guard lock(state.mutex);
            state.abort = state.release = true;
        }
        state.changed.notify_all();
        // Releasing our deliberately blocked callback must precede join.
        try { host.stop(); } catch (...) {}
    }
};
int main() {
    try {
        Shared state; // Outlives MediaHost and every callback.
        hive::MediaHost host;
        Cleanup cleanup{state, host};
        auto wait = [&](auto predicate, const char* message) {
            std::unique_lock lock(state.mutex);
            if (!state.changed.wait_for(lock, std::chrono::seconds(5), predicate)) throw std::runtime_error(message);
        };
        auto until = [&](Clock::time_point deadline) {
            std::unique_lock lock(state.mutex);
            state.changed.wait_until(lock, deadline, [&] { return state.abort; });
        };
        std::string error;
        if (!host.start({0,0,60000,9191}, [&](hive::EncodedFrame frame) {
            const FrameObservation observation{frame.sequence, frame.capture_time_us, now_us(), frame.keyframe};
            std::unique_lock lock(state.mutex);
            ++state.callbacks;
            if (state.callbacks == 1) {
                state.blocked = observation;
                state.blocked_at = Clock::now();
                state.changed.notify_all();
                state.changed.wait(lock, [&] { return state.release || state.abort; });
            } else if (state.callbacks == 2) {
                state.resumed = observation;
                state.credit = false;
                state.blocked_checks = 0;
                state.changed.notify_all();
            }
        }, error, [&] {
            std::lock_guard lock(state.mutex);
            if (!state.credit) { ++state.blocked_checks; state.changed.notify_all(); }
            return state.credit;
        })) throw std::runtime_error(error);

        wait([&] { return state.callbacks == 1; }, "The real host did not deliver its first frame.");
        Clock::time_point blocked_at;
        { std::lock_guard lock(state.mutex); blocked_at = state.blocked_at; }
        // Public IDR requests make progress even on a completely static desktop.
        // Stage work early, then deny new capture credit so its age can exceed
        // the source queue budget while delivery remains blocked for 250 ms.
        host.request_keyframe();
        until(blocked_at + std::chrono::milliseconds(40));
        host.request_keyframe();
        until(blocked_at + std::chrono::milliseconds(75));
        { std::lock_guard lock(state.mutex); state.credit = false; }
        host.notify_ready();
        until(blocked_at + std::chrono::milliseconds(250));
        {
            std::lock_guard lock(state.mutex);
            state.released_us = now_us(); state.credit = true; state.release = true;
        }
        state.changed.notify_all(); host.notify_ready();
        // Deliberately do not request another keyframe here. The real source
        // queue must discard its aged tail and initiate recovery itself.
        wait([&] { return state.callbacks >= 2; }, "The stalled source did not resume after delivery was released.");
        FrameObservation blocked, resumed;
        uint64_t released_us = 0;
        {
            std::lock_guard lock(state.mutex);
            blocked = state.blocked; resumed = state.resumed; released_us = state.released_us;
        }
        if (!blocked.keyframe || !resumed.keyframe)
            throw std::runtime_error("Stall recovery emitted a dependent frame before a fresh IDR.");
        if (resumed.sequence <= blocked.sequence + 1)
            throw std::runtime_error("No obsolete submitted frame was skipped; the blocked delivery did not exercise independent encoding.");
        if (resumed.captured_us <= blocked.captured_us || resumed.captured_us + 100000 < released_us)
            throw std::runtime_error("Stall recovery replayed an aged capture instead of a fresh IDR.");
        wait([&] { return state.blocked_checks >= 2; }, "Source did not enter the final denied-credit wait.");
        const auto stopping = Clock::now(); host.stop();
        const double stop_ms = std::chrono::duration<double, std::milli>(Clock::now()-stopping).count();
        if (host.running() || stop_ms > 1000)
            throw std::runtime_error("Stopping a source with denied credit did not wake and join promptly.");

        // A non-std callback exception used to be able to escape the final
        // worker catch and terminate the process. Exercise the real delivery
        // worker, then join all encoder/output workers and reuse the object.
        if (!host.start({0,0,60000,9192}, [&](hive::EncodedFrame) {
            { std::lock_guard lock(state.mutex); state.threw = true; }
            state.changed.notify_all();
            throw 42;
        }, error)) throw std::runtime_error(error);
        wait([&] { return state.threw; }, "Non-standard exception test never reached the output callback.");
        const auto failure_deadline = Clock::now() + std::chrono::seconds(3);
        while (host.running() && Clock::now() < failure_deadline) {
            std::unique_lock lock(state.mutex);
            state.changed.wait_for(lock, std::chrono::milliseconds(10));
        }
        if (host.running()) throw std::runtime_error("A throwing output callback did not stop the media host.");
        host.stop();
        const double recovery_age_ms = (resumed.arrived_us >= resumed.captured_us ? resumed.arrived_us-resumed.captured_us : 0)/1000.0;
        std::printf("PASS real MediaHost freshness: 250 ms blocked delivery, skipped %llu obsolete sequence(s), resumed at IDR with %.1f ms capture age; denied-credit stop %.1f ms; non-std callback exception stops and joins without termination. Static desktop uses forced IDRs before the stall release; P-reference invalidation is covered by source_queue. No visible stimulus or input.\n",
            static_cast<unsigned long long>(resumed.sequence-blocked.sequence-1), recovery_age_ms, stop_ms);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL media freshness: %s\n", error.what()); return 1;
    }
}
