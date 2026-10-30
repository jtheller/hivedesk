#pragma once
#include <windows.h>

namespace hive {
struct WindowsPowerRequests {
    static HANDLE create(){
        REASON_CONTEXT reason{};reason.Version=POWER_REQUEST_CONTEXT_VERSION;reason.Flags=POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        reason.Reason.SimpleReasonString=const_cast<wchar_t*>(L"HiveDesk active remote desktop session");return PowerCreateRequest(&reason);
    }
    static bool set(HANDLE h,POWER_REQUEST_TYPE t){return PowerSetRequest(h,t)!=FALSE;}
    static void clear(HANDLE h,POWER_REQUEST_TYPE t){PowerClearRequest(h,t);}
    static void close(HANDLE h){CloseHandle(h);}
    static void wake_display(){SetThreadExecutionState(ES_DISPLAY_REQUIRED);}
};
// Owned by the SYSTEM desktop broker session, never by warm/Peek/discovery.
// Handles are process-scoped, so pipe cancellation, exceptions and process exit
// release requests even when capture or its original thread fails.
template<class Api=WindowsPowerRequests> class SessionPower {
    HANDLE handle_=INVALID_HANDLE_VALUE;
    bool held_[3]{};
    static constexpr POWER_REQUEST_TYPE types_[3]={PowerRequestSystemRequired,PowerRequestExecutionRequired,PowerRequestDisplayRequired};
public:
    SessionPower()=default;SessionPower(const SessionPower&)=delete;SessionPower& operator=(const SessionPower&)=delete;
    ~SessionPower(){release();}
    bool activate(){
        if(handle_!=INVALID_HANDLE_VALUE)return held_[0]&&held_[1]&&held_[2];
        handle_=Api::create();if(!handle_||handle_==INVALID_HANDLE_VALUE){handle_=INVALID_HANDLE_VALUE;return false;}
        bool ok=true;for(unsigned i=0;i<3;++i){held_[i]=Api::set(handle_,types_[i]);ok&=held_[i];}
        // A display-required request preserves an already-on display; the pulse
        // resets its idle timer for a screen that was off before activation.
        Api::wake_display();return ok;
    }
    void release(){if(handle_==INVALID_HANDLE_VALUE)return;for(unsigned i=0;i<3;++i)if(held_[i]){Api::clear(handle_,types_[i]);held_[i]=false;}Api::close(handle_);handle_=INVALID_HANDLE_VALUE;}
};
}
