#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d2d1.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <stdexcept>
#include <algorithm>

// UI-thread-owned retained surface. DWM evaluates opacity on its composition
// clock; no WM_TIMER animation, moving HWNDs, or per-frame bitmap uploads.
class PanelComposition {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> gpu_;
    Ptr<ID2D1Factory> factory_;
    Ptr<IDCompositionDevice> device_;
    Ptr<IDCompositionTarget> target_;
    Ptr<IDCompositionVisual> visual_;
    Ptr<IDCompositionVisual> preview_,list_;
    Ptr<IDCompositionEffectGroup> opacity_,list_opacity_;
    Ptr<IDCompositionSurface> surface_;
    Ptr<IDCompositionRectangleClip> clip_;
    Ptr<IDCompositionRectangleClip> list_clip_;
    UINT width_=0,height_=0;
    float fade_from_=0,fade_to_=0,fade_seconds_=0;
    ULONGLONG fade_start_=0;
    static void check(HRESULT result) { if(FAILED(result))throw std::runtime_error("Switcher GPU composition failed."); }
    static bool animations() { BOOL enabled=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);return enabled!=FALSE; }
    float current_opacity() const {
        const float t=fade_seconds_>0?std::clamp(float(GetTickCount64()-fade_start_)/(1000*fade_seconds_),0.0f,1.0f):1.0f;
        const float remaining=1-t;return fade_to_+(fade_from_-fade_to_)*remaining*remaining*remaining;
    }
    Ptr<IDCompositionAnimation> tween(float from,float to,float duration) {
        const float delta=to-from;Ptr<IDCompositionAnimation> motion;check(device_->CreateAnimation(&motion));
        check(motion->AddCubic(0,from,3*delta/duration,-3*delta/(duration*duration),delta/(duration*duration*duration)));
        check(motion->End(duration,to));return motion;
    }
    void fade(float from,float to,float duration) {
        fade_from_=from;fade_to_=to;fade_seconds_=animations()?duration:0;fade_start_=GetTickCount64();
        if(fade_seconds_>0){auto motion=tween(from,to,duration);check(opacity_->SetOpacity(motion.Get()));}
        else check(opacity_->SetOpacity(to));
    }
public:
    void create(HWND window) {
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&gpu_,nullptr,nullptr));
        Ptr<IDXGIDevice> dxgi;check(gpu_.As(&dxgi));
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf()));
        check(DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&device_)));
        check(device_->CreateTargetForHwnd(window,TRUE,&target_));
        check(device_->CreateVisual(&visual_));check(target_->SetRoot(visual_.Get()));
        check(device_->CreateVisual(&preview_));check(device_->CreateVisual(&list_));
        check(visual_->AddVisual(preview_.Get(),FALSE,nullptr));check(visual_->AddVisual(list_.Get(),TRUE,preview_.Get()));
        check(device_->CreateEffectGroup(&opacity_));check(visual_->SetEffect(opacity_.Get()));
        check(opacity_->SetOpacity(0.0f));
        check(device_->CreateEffectGroup(&list_opacity_));check(list_->SetEffect(list_opacity_.Get()));
        check(device_->CreateRectangleClip(&clip_));check(preview_->SetClip(clip_.Get()));
        check(device_->CreateRectangleClip(&list_clip_));check(list_->SetClip(list_clip_.Get()));
        check(device_->Commit());
        SetPropW(window,L"HiveDesk.Composited",reinterpret_cast<HANDLE>(1));
    }
    void present(const void* bgra,UINT width,UINT height) {
        if(width!=width_||height!=height_) {
            surface_.Reset();check(device_->CreateSurface(width,height,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&surface_));
            check(preview_->SetContent(surface_.Get()));check(list_->SetContent(surface_.Get()));width_=width;height_=height;
        }
        Ptr<IDXGISurface> texture;POINT offset{};
        check(surface_->BeginDraw(nullptr,IID_PPV_ARGS(&texture),&offset));
        HRESULT result=S_OK;
        {
            Ptr<ID2D1RenderTarget> canvas;
            const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            result=factory_->CreateDxgiSurfaceRenderTarget(texture.Get(),&properties,&canvas);
            if(SUCCEEDED(result)) {
                Ptr<ID2D1Bitmap> bitmap;
                result=canvas->CreateBitmap(D2D1::SizeU(width,height),bgra,width*4,D2D1::BitmapProperties(properties.pixelFormat,96,96),&bitmap);
                if(SUCCEEDED(result)) {
                    canvas->BeginDraw();
                    canvas->DrawBitmap(bitmap.Get(),D2D1::RectF(float(offset.x),float(offset.y),float(offset.x+width),float(offset.y+height)),1,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    result=canvas->EndDraw();
                }
            }
        }
        const HRESULT end=surface_->EndDraw();check(result);check(end);check(device_->Commit());
    }
    void show(float width,float height,float previous_height,bool entering,bool reversing) {
        check(clip_->SetLeft(0.0f));check(clip_->SetTop(0.0f));check(clip_->SetRight(width));
        check(list_clip_->SetLeft(0.0f));check(list_clip_->SetRight(width));check(list_clip_->SetBottom(height));
        if(entering){
            check(clip_->SetBottom(height));check(list_clip_->SetTop(height));
            check(list_opacity_->SetOpacity(1.0f));fade(0,1,.24f);
        } else {
            if(reversing)fade(current_opacity(),1,.20f);
            // Keep the existing preview steady. Only the additional list area
            // fades in, with fixed bounds: no translation or animated clipping.
            if(previous_height<height){
                check(clip_->SetBottom(previous_height));check(list_clip_->SetTop(previous_height));
                if(animations()){auto reveal=tween(0,1,.24f);check(list_opacity_->SetOpacity(reveal.Get()));}
                else check(list_opacity_->SetOpacity(1.0f));
            }
        }
        check(device_->Commit());
    }
    UINT hide() {
        fade(current_opacity(),0,.20f);
        check(device_->Commit());
        return animations()?210:1;
    }
};
