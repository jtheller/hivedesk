#pragma once
#include <vpl/mfxvideo.h>
#include <memory>
#include <vector>
#include <stdexcept>

// Bounded Media SDK-compatible surfaces. The hardware runtime performs its
// own upload/readback. Storage must outlive codec Close, including failed Sync.
struct IntelSystemSurface {
    mfxFrameSurface1 frame{};
    std::vector<mfxU8> storage;
    explicit IntelSystemSurface(const mfxFrameInfo& info) {
        if(info.FourCC!=MFX_FOURCC_AYUV||info.ChromaFormat!=MFX_CHROMAFORMAT_YUV444||
           info.BitDepthLuma!=8||info.BitDepthChroma!=8||!info.Width||!info.Height||
           info.Width>8192||info.Height>8192||info.CropX+info.CropW>info.Width||info.CropY+info.CropH>info.Height)
            throw std::runtime_error("Invalid Intel system-memory AYUV surface");
        frame.Info=info;const uint32_t pitch=uint32_t(info.Width)*4;
        storage.resize(size_t(pitch)*info.Height);
        frame.Data.PitchLow=mfxU16(pitch);frame.Data.PitchHigh=mfxU16(pitch>>16);
        // AYUV is packed V,U,Y,A in increasing byte addresses.
        frame.Data.V=storage.data();frame.Data.U=storage.data()+1;
        frame.Data.Y=storage.data()+2;frame.Data.A=storage.data()+3;
    }
};
class IntelSystemSurfaces {
    std::vector<std::unique_ptr<IntelSystemSurface>> surfaces_;
public:
    void allocate(const mfxFrameInfo& info,unsigned count) {
        if(!surfaces_.empty()||!count||count>32||uint64_t(info.Width)*info.Height*4*count>128ull*1024*1024)
            throw std::runtime_error("Intel diagnostic surface pool exceeds bounds");
        for(unsigned i=0;i<count;++i)surfaces_.push_back(std::make_unique<IntelSystemSurface>(info));
    }
    mfxFrameSurface1* available() {
        for(auto& surface:surfaces_)if(!surface->frame.Data.Locked)return &surface->frame;
        throw std::runtime_error("Intel diagnostic has no unlocked system-memory surface");
    }
};
