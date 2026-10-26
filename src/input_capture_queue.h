#pragma once
#include "hivedesk/types.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hive {
struct CapturedInput {
    InputEvent event;
    uint64_t generation=0;
    uint64_t captured_us=0;
};

// A fixed allocation input FIFO. Motion can merge only with an adjacent motion
// of the same kind and generation: button/key/wheel boundaries retain order.
// Overflow is reported to the owner, which releases the session rather than
// silently losing a key-up or replaying an unbounded old input sequence.
template<size_t Capacity=256> class InputCaptureQueue {
public:
    static_assert(Capacity>1);
    bool push(CapturedInput input) {
        if(count_) {
            auto& tail=items_[(first_+count_-1)%Capacity];
            if(tail.generation==input.generation&&tail.event.kind==input.event.kind&&motion(input.event.kind)) {
                if(input.event.kind==InputKind::Absolute)tail.event=input.event;
                else {
                    tail.event.a=int32_t(std::clamp<int64_t>(int64_t(tail.event.a)+input.event.a,INT32_MIN,INT32_MAX));
                    tail.event.b=int32_t(std::clamp<int64_t>(int64_t(tail.event.b)+input.event.b,INT32_MIN,INT32_MAX));
                }
                // The oldest sample determines age for relative accumulation;
                // absolute motion replaces the old position entirely.
                if(input.event.kind==InputKind::Absolute)tail.captured_us=input.captured_us;
                ++coalesced_;return true;
            }
        }
        if(count_==Capacity)return false;
        items_[(first_+count_)%Capacity]=input;++count_;peak_=std::max(peak_,count_);return true;
    }
    std::optional<CapturedInput> pop() {
        if(!count_)return {};
        auto input=items_[first_];first_=(first_+1)%Capacity;--count_;return input;
    }
    void clear(){first_=count_=0;}
    size_t size()const{return count_;}
    size_t peak()const{return peak_;}
    uint64_t coalesced()const{return coalesced_;}
    static bool motion(InputKind kind){return kind==InputKind::Absolute||kind==InputKind::Relative;}
private:
    std::array<CapturedInput,Capacity> items_{};
    size_t first_=0,count_=0,peak_=0;
    uint64_t coalesced_=0;
};
}
