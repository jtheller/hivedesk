#pragma once
#include <windows.h>
#include <chrono>
#include <stdexcept>
#include <string>

namespace hive {
// Retain the scheduled phase across ordinary wake-up jitter. Reanchoring each
// frame to its actual start adds every scheduler delay to the frame interval.
// After a real stall, skip elapsed ticks in one step rather than catching up by
// submitting the frames that would have occupied those ticks.
inline std::chrono::steady_clock::time_point advance_frame_deadline(
    std::chrono::steady_clock::time_point previous,
    std::chrono::steady_clock::time_point capture_started,
    std::chrono::steady_clock::duration interval) {
    if(interval<=std::chrono::steady_clock::duration::zero())return capture_started;
    auto next=previous+interval;
    if(next<=capture_started)next+=interval*((capture_started-next)/interval+1);
    return next;
}

// One waiting thread; other threads may notify(). Unlike a condition-variable
// timeout, the high-resolution timer is not rounded to the default clock tick.
// This does not make thread dispatch real-time: callers still check their clock
// and predicate after every return. Destroy only after the waiter has joined.
class FrameTimer {
public:
    using Clock=std::chrono::steady_clock;
    enum class Result { Deadline, Wake };
    FrameTimer() {
        wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!wake_)fail("Create frame wake event");
        // Supported since Windows 10 version 1803. Older systems retain the
        // ordinary timer path, without changing global/process timer settings.
        constexpr DWORD HighResolution=0x00000002;
        timer_=CreateWaitableTimerExW(nullptr,nullptr,HighResolution,TIMER_MODIFY_STATE|SYNCHRONIZE);
        high_resolution_=timer_!=nullptr;
        if(!timer_)timer_=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(!timer_){const auto error=GetLastError();CloseHandle(wake_);wake_=nullptr;fail("Create frame pacing timer",error);}
    }
    ~FrameTimer(){if(timer_){CancelWaitableTimer(timer_);CloseHandle(timer_);}if(wake_)CloseHandle(wake_);}
    FrameTimer(const FrameTimer&)=delete;
    FrameTimer& operator=(const FrameTimer&)=delete;
    bool high_resolution() const noexcept{return high_resolution_;}
    // Auto-reset events retain a signal sent just before wait enrollment. Do
    // not ResetEvent in wait_until: that would lose stop/keyframe notifications.
    void notify() noexcept{SetEvent(wake_);}
    Result wait_until(Clock::time_point deadline,HANDLE external=nullptr) {
        const auto remaining=deadline-Clock::now();
        if(remaining<=Clock::duration::zero())return Result::Deadline;
        using Tick=std::chrono::duration<long long,std::ratio<1,10000000>>;
        LARGE_INTEGER due{};due.QuadPart=-std::chrono::ceil<Tick>(remaining).count();
        // A one-shot relative timer is unaffected by wall-clock corrections;
        // zero tolerable delay requests no extra timer coalescing.
        if(!SetWaitableTimerEx(timer_,&due,0,nullptr,nullptr,nullptr,0))fail("Arm frame pacing timer");
        const HANDLE objects[]{wake_,timer_,external};
        const auto result=WaitForMultipleObjects(external?3:2,objects,FALSE,INFINITE);
        if(result==WAIT_OBJECT_0){CancelWaitableTimer(timer_);return Result::Wake;}
        if(external&&result==WAIT_OBJECT_0+2){CancelWaitableTimer(timer_);return Result::Wake;}
        if(result==WAIT_OBJECT_0+1)return Result::Deadline;
        fail("Wait for frame pacing timer");
    }
private:
    [[noreturn]] static void fail(const char* operation,DWORD error=GetLastError()) {
        throw std::runtime_error(std::string(operation)+" (Windows error "+std::to_string(error)+")");
    }
    HANDLE timer_=nullptr,wake_=nullptr;
    bool high_resolution_=false;
};
}
