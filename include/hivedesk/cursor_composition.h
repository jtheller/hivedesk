#pragma once
#include "types.h"

namespace hive {
// DXGI pointer metadata is valid only on an acquisition with a mouse update.
// Keep observed ownership separate from the last successfully copied image:
// an ownership-only acquisition must copy once, and a failed copy must retry.
class CaptureCursorComposition {
    CursorComposition observed_=CursorComposition::Unknown;
    CursorComposition picture_=CursorComposition::Unknown;
public:
    void observe(int64_t last_mouse_update_time,bool separate_visible) {
        if(last_mouse_update_time!=0)
            observed_=separate_visible?CursorComposition::Separate:CursorComposition::NoSeparate;
    }
    bool needs_copy() const {return observed_!=picture_;}
    void copied() {picture_=observed_;}
    CursorComposition observed() const {return observed_;}
    CursorComposition picture() const {return picture_;}
};

// Only an actually presented picture may change who owns the visible cursor.
// The separate cursor channel still owns shape, visibility and host motion.
class PresentedCursorComposition {
    uint64_t session_=0,sequence_=0;
    bool presented_=false;
    CursorComposition mode_=CursorComposition::Unknown;
public:
    void reset(uint64_t session=0) {
        session_=session;sequence_=0;presented_=false;mode_=CursorComposition::Unknown;
    }
    bool present(uint64_t session,uint64_t sequence,CursorComposition mode) {
        if(session!=session_||(presented_&&sequence<=sequence_)||!valid_cursor_composition(mode))return false;
        sequence_=sequence;presented_=true;mode_=mode;return true;
    }
    bool visible(bool cursor_visible,bool relative_mode) const {
        return cursor_visible&&!relative_mode&&mode_!=CursorComposition::NoSeparate;
    }
    CursorComposition mode() const {return mode_;}
};
}
