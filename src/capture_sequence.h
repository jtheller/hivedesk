#pragma once
#include <cstdint>
#include <stdexcept>
namespace hive {
// Capture/codec lifetimes are shorter than a remote stream: lock/unlock and
// DXGI access loss rebuild the producer. Keep sequence identity with the stream
// so an existing viewer accepts the new IDR without accepting old/duplicate
// packets. Every accepted submission consumes an index, including dropped work.
class CaptureSequence {
    uint64_t session_=0,next_=0;bool bound_=false,first_=true;
public:
    void begin(uint64_t session){first_=true;if(!bound_||session_!=session){session_=session;next_=0;bound_=true;}}
    bool first()const{return first_;}
    uint64_t next()const{if(next_==UINT64_MAX)throw std::overflow_error("Capture sequence exhausted");return next_;}
    void submitted(){next();++next_;first_=false;}
};
}
