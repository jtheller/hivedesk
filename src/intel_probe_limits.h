#pragma once
#include <vpl/mfxvideo.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

// GetVideoParam reports the maximum compressed-frame capacity after Init.
// BRCParamMultiplier applies to BufferSizeInKB as well as the bit rate.
inline uint32_t intel_probe_bitstream_capacity(const mfxInfoMFX& info) {
    const uint64_t bytes=uint64_t(info.BufferSizeInKB)*std::max<unsigned>(1,info.BRCParamMultiplier)*1000;
    if(bytes>64ull*1024*1024)throw std::runtime_error("Intel bitstream capacity exceeds 64 MiB");
    return uint32_t(std::max<uint64_t>(4ull*1024*1024,bytes));
}
