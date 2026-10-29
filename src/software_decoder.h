#pragma once
#include "hivedesk/types.h"
#include <array>
#include <functional>
#include <memory>

namespace hive {
// Planes are borrowed only for the synchronous output callback. No CUDA or
// vendor-specific device is required by this backend.
struct SoftwarePicture {
    uint32_t width=0,height=0;
    uint32_t decode_us=0;
    std::array<const uint8_t*,3> planes{};
    std::array<int,3> strides{};
};
class SoftwareDecoder {
public:
    SoftwareDecoder();
    ~SoftwareDecoder();
    SoftwareDecoder(const SoftwareDecoder&)=delete;
    // One low-delay access unit in, one picture out. Reference decoding is
    // mandatory even when the caller elects not to display an old picture.
    uint32_t decode(const EncodedFrame&,const std::function<void(const SoftwarePicture&)>&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
