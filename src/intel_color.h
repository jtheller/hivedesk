#pragma once
#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>

namespace hive {
// GPU full-range BT.709 conversion, followed by bounded readback for legacy
// Media SDK system-memory AYUV. No scaling or chroma subsampling.
class IntelColor {
public:
    IntelColor(ID3D11Texture2D* source,uint32_t padded_width,uint32_t padded_height);
    ~IntelColor();
    IntelColor(const IntelColor&)=delete;
    void convert(uint8_t* destination,uint32_t pitch,const std::function<bool()>& cancelled);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
