#pragma once
#include <cstdint>

namespace hive {
// Desktop Duplication reports pointer-only acquisitions with LastPresentTime
// zero. Cursor shape/position travels independently, so these acquisitions must
// not consume a video slot or replace the last copied desktop image.
class CaptureUpdateFilter {
    bool have_picture_=false;
public:
    bool needs_copy(int64_t last_present_time) const {
        // The initial resource is still a valid desktop image even if the
        // acquisition was triggered by the pointer. Never leave it uninitialized.
        return !have_picture_||last_present_time!=0;
    }
    void copied(){have_picture_=true;}
    bool have_picture()const{return have_picture_;}
};
}
