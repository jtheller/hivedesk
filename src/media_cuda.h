#pragma once
#include <cstddef>
#include <cstdint>
// CUDA implementation owns scratch GPU memory and registrations, not D3D textures.
// The caller must keep its CUDA context current on this thread for every call.
// Texture conversion uses a private stream and completes that stream before
// returning, so encoder input and decoder output lifetimes remain synchronous.
struct HiveGpu;
HiveGpu* hive_gpu_create();
void hive_gpu_destroy(HiveGpu*) noexcept;
const char* hive_gpu_error(HiveGpu*) noexcept;
bool hive_gpu_encode_host(HiveGpu*, const uint8_t* bgra, uint32_t width, uint32_t height,
    uint64_t yuv, size_t yuv_pitch) noexcept;
bool hive_gpu_encode_texture(HiveGpu*, void* d3d_texture, uint32_t width, uint32_t height,
    uint64_t yuv, size_t yuv_pitch) noexcept;
bool hive_gpu_decode_host(HiveGpu*, uint64_t yuv, size_t yuv_pitch, uint32_t width,
    uint32_t height, uint8_t* bgra) noexcept;
bool hive_gpu_decode_texture(HiveGpu*, uint64_t yuv, size_t yuv_pitch, uint32_t width,
    uint32_t height, void* d3d_texture) noexcept;
