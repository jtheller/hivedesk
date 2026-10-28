#pragma once
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <cstdint>

// Only prewarmed HWNDs use composition. Their video opacity can change without
// copying pixels, changing input focus or rebuilding the decoder/swap chain.
class ViewerComposition {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<IDCompositionDevice> device_;
    Ptr<IDCompositionTarget> target_;
    Ptr<IDCompositionVisual> visual_;
    Ptr<IDCompositionEffectGroup> effect_;
    std::mutex mutex_;
    float from_=0,to_=0;ULONGLONG start_=0;bool animated_=false;
    static void check(HRESULT hr,const char* step="update"){
        if(FAILED(hr)){std::ostringstream message;message<<"Viewer composition "<<step<<" failed: HRESULT 0x"<<std::hex<<uint32_t(hr);throw std::runtime_error(message.str());}
    }
public:
    void create(HWND window,ID3D11Device* gpu,IDXGISwapChain* swap){
        Ptr<IDXGIDevice> dxgi;check(gpu->QueryInterface(IID_PPV_ARGS(&dxgi)));
        check(DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&device_)),"create device");
        check(device_->CreateTargetForHwnd(window,TRUE,&target_),"attach HWND");
        check(device_->CreateVisual(&visual_));check(visual_->SetContent(swap));
        check(device_->CreateEffectGroup(&effect_));check(effect_->SetOpacity(0.0f));
        check(visual_->SetEffect(effect_.Get()));check(target_->SetRoot(visual_.Get()));check(device_->Commit());
    }
    void opacity(bool visible,bool animate){
        std::lock_guard lock(mutex_);const float target=visible?1.0f:0.0f;
        if(target==to_&&animate)return;
        const auto now=GetTickCount64();const float t=animated_?std::clamp(float(now-start_)/180,0.0f,1.0f):1;
        const float remain=1-t;from_=to_+(from_-to_)*remain*remain*remain;to_=target;start_=now;
        BOOL enabled=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);animated_=animate&&enabled;
        if(animated_){
            Ptr<IDCompositionAnimation> motion;check(device_->CreateAnimation(&motion));const float d=to_-from_,s=.18f;
            check(motion->AddCubic(0,from_,3*d/s,-3*d/(s*s),d/(s*s*s)));check(motion->End(s,to_));check(effect_->SetOpacity(motion.Get()));
        }else check(effect_->SetOpacity(to_));
        check(device_->Commit());
    }
};
