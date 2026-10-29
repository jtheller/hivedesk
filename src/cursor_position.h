#pragma once
#include "hivedesk/types.h"
namespace hive {
inline bool cursor_on_primary(int64_t x,int64_t y,int64_t width,int64_t height){
    return width>0&&height>0&&x>=0&&y>=0&&x<width&&y<height;
}
// A timestamp is echoed from this viewer's input clock, never compared between
// machines. Consume rejected revisions so a later shape packet cannot replay it.
class CursorPositionGate {
    uint64_t session_=0,revision_=0;
public:
    enum class Result { Allowed, Baseline, Session, Revision, LocalInput };
    void reset(uint64_t session){session_=session;revision_=0;}
    Result observe(const CursorState& c,bool input_current){
        if(!session_||c.session!=session_)return Result::Session;
        if(c.position_revision<=revision_)return Result::Revision;
        revision_=c.position_revision;
        if(!c.position_valid)return Result::Baseline;
        return input_current?Result::Allowed:Result::LocalInput;
    }
};
}
