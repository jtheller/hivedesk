#pragma once
#include "hivedesk/types.h"
#include <d3d11.h>
#include <dxgi.h>
#include <functional>
#include <memory>

namespace hive {
class IntelEncoder {
public:
    struct Completion {
        EncodedFrame frame;
        uint32_t color_us=0,codec_us=0,total_us=0,average_qp=UINT32_MAX;
        uint64_t submitted_us=0,completed_us=0;
    };
    IntelEncoder(uint32_t width,uint32_t height,const StreamSettings&,IDXGIAdapter*,ID3D11Texture2D*,std::function<bool()> cancelled);
    ~IntelEncoder();
    IntelEncoder(const IntelEncoder&)=delete;
    bool ready()const;uint32_t in_flight()const;
    bool automatic_rate()const;bool dynamic_rate()const;
    uint32_t bitrate_bps()const;uint32_t nominal_millihz()const;
    bool reconfigure(uint32_t bitrate,uint32_t millihz);
    bool submit_texture(ID3D11Texture2D*,uint64_t sequence,bool force,uint64_t captured_us,bool* completion_queued,CursorComposition);
    bool receive(Completion&);
    void finish();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
