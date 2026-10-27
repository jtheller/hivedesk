#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d2d1.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <stdexcept>
#include <algorithm>
#include <vector>

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
    Ptr<IDCompositionVisual> backdrop_;
    Ptr<IDCompositionScaleTransform> backdrop_scale_;
    UINT width_=0,height_=0;
    HWND window_=nullptr;
    ULONG_PTR revision_=0;
    bool detached_=false;
    std::vector<Ptr<IDCompositionVisual>> card_layers_;
    ULONGLONG cards_started_=0;
    struct CardMotion { Ptr<IDCompositionVisual> visual;float from=0,to=0;ULONGLONG started=0; };
    std::vector<CardMotion> drag_cards_;
    float fade_from_=0,fade_to_=0,fade_seconds_=0;
    ULONGLONG fade_start_=0;
    static void check(HRESULT result) { if(FAILED(result))throw std::runtime_error("Switcher GPU composition failed."); }
    static bool animations() { BOOL enabled=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);return enabled!=FALSE; }
    float current_opacity() const {
        const float t=fade_seconds_>0?std::clamp(float(GetTickCount64()-fade_start_)/(1000*fade_seconds_),0.0f,1.0f):1.0f;
        const float remaining=1-t;return fade_to_+(fade_from_-fade_to_)*remaining*remaining*remaining;
    }
    Ptr<IDCompositionAnimation> tween(float from,float to,float duration,float delay=0) {
        const float delta=to-from;Ptr<IDCompositionAnimation> motion;check(device_->CreateAnimation(&motion));
        if(delay>0)check(motion->AddCubic(0,from,0,0,0));
        check(motion->AddCubic(delay,from,3*delta/duration,-3*delta/(duration*duration),delta/(duration*duration*duration)));
        check(motion->End(delay+duration,to));return motion;
    }
    void fade(float from,float to,float duration) {
        fade_from_=from;fade_to_=to;fade_seconds_=animations()?duration:0;fade_start_=GetTickCount64();
        if(fade_seconds_>0){auto motion=tween(from,to,duration);check(opacity_->SetOpacity(motion.Get()));}
        else check(opacity_->SetOpacity(to));
    }
public:
    void finish_cards(bool commit=true) {
        if(card_layers_.empty())return;
        for(auto& layer:card_layers_)check(visual_->RemoveVisual(layer.Get()));card_layers_.clear();drag_cards_.clear();cards_started_=0;
        check(preview_->SetContent(surface_.Get()));check(list_->SetContent(surface_.Get()));
        check(clip_->SetTop(0.0f));check(clip_->SetBottom(float(height_)));check(list_clip_->SetTop(float(height_)));
        if(commit)check(device_->Commit());
    }
    // All layers share the same bitmap. Only DWM transforms/opacity animate;
    // snapshot arrivals can update it without restarting the 300 ms timeline.
    void reveal_cards(const std::vector<RECT>& cards,COLORREF background,int dragged=-1) {
        if((dragged<0&&!animations())||cards.empty())return;
        finish_cards(false);check(preview_->SetContent(nullptr));check(list_->SetContent(nullptr));
        const auto layer=[&](const RECT& bounds,int index){
            if(bounds.right<=bounds.left||bounds.bottom<=bounds.top)return;
            Ptr<IDCompositionVisual> v;check(device_->CreateVisual(&v));check(v->SetContent(surface_.Get()));
            const D2D_RECT_F clip{float(bounds.left),float(bounds.top),float(bounds.right),float(bounds.bottom)};check(v->SetClip(clip));
            if(index>=0&&dragged<0){
                const float delay=cards.size()>1?.12f*index/float(cards.size()-1):0;
                const float duration=.30f-delay;
                auto movement=tween(-float(width_),0,duration,delay);check(v->SetOffsetX(movement.Get()));
                Ptr<IDCompositionEffectGroup> effect;check(device_->CreateEffectGroup(&effect));
                auto opacity=tween(0,1,duration,delay);check(effect->SetOpacity(opacity.Get()));check(v->SetEffect(effect.Get()));
            }
            if(dragged>=0){
                drag_cards_.push_back({v});
                if(index==dragged){Ptr<IDCompositionEffectGroup> effect;check(device_->CreateEffectGroup(&effect));check(effect->SetOpacity(0.0f));check(v->SetEffect(effect.Get()));}
            }
            check(visual_->AddVisual(v.Get(),FALSE,nullptr));card_layers_.push_back(std::move(v));
        };
        // An opaque, solid backing remains behind moving/fading cards. Cutting
        // holes out of the original bitmap would fade through to the desktop.
        Ptr<IDCompositionSurface> backing;check(device_->CreateSurface(1,1,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&backing));
        Ptr<IDXGISurface> texture;POINT offset{};check(backing->BeginDraw(nullptr,IID_PPV_ARGS(&texture),&offset));
        Ptr<ID2D1RenderTarget> canvas;
        const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
        HRESULT result=factory_->CreateDxgiSurfaceRenderTarget(texture.Get(),&properties,&canvas);
        if(SUCCEEDED(result)){
            Ptr<ID2D1SolidColorBrush> brush;result=canvas->CreateSolidColorBrush(D2D1::ColorF(GetRValue(background)/255.f,GetGValue(background)/255.f,GetBValue(background)/255.f,1),&brush);
            if(SUCCEEDED(result)){canvas->BeginDraw();canvas->FillRectangle(D2D1::RectF(float(offset.x),float(offset.y),float(offset.x+1),float(offset.y+1)),brush.Get());result=canvas->EndDraw();}
        }
        const auto end=backing->EndDraw();check(result);check(end);
        Ptr<IDCompositionVisual> back;Ptr<IDCompositionScaleTransform> scale;
        check(device_->CreateVisual(&back));check(back->SetContent(backing.Get()));check(device_->CreateScaleTransform(&scale));
        check(scale->SetScaleX(float(width_)));check(scale->SetScaleY(float(height_)));check(back->SetTransform(scale.Get()));
        check(visual_->AddVisual(back.Get(),FALSE,nullptr));card_layers_.push_back(std::move(back));
        for(size_t i=0;i<cards.size();++i)layer(cards[i],int(i));
        cards_started_=dragged<0?GetTickCount64():0;check(device_->Commit());
        SetPropW(window_,L"HiveDesk.CardAnimationMs",reinterpret_cast<HANDLE>(300));
        SetPropW(window_,L"HiveDesk.AnimatedCards",reinterpret_cast<HANDLE>(cards.size()));
    }
    void drag_slot(int from,int slot,int first,int stride) {
        if(drag_cards_.empty())return;
        const auto now=GetTickCount64();bool changed=false;
        for(size_t n=0;n<drag_cards_.size();++n){
            auto& card=drag_cards_[n];const int i=first+int(n);
            const float target=slot>=0&&i>0&&i!=from?
                (slot<=i&&i<from?float(stride):from<i&&i<slot?-float(stride):0):0;
            if(target==card.to)continue;
            const float t=std::clamp(float(now-card.started)/140.f,0.f,1.f),remaining=1-t;
            card.from=card.to+(card.from-card.to)*remaining*remaining*remaining;card.to=target;card.started=now;
            if(animations()){auto motion=tween(card.from,target,.14f);check(card.visual->SetOffsetY(motion.Get()));}
            else check(card.visual->SetOffsetY(target));changed=true;
        }
        if(changed)check(device_->Commit());
    }
    // A full-screen waiting surface must never fade through to the previous
    // machine. Keep an opaque neutral backing while only the snapshot fades in.
    void cover(float width,float height,bool entering,bool discard=false) {
        if(!backdrop_){
            Ptr<IDCompositionSurface> surface;
            check(device_->CreateSurface(1,1,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&surface));
            Ptr<IDXGISurface> texture;POINT offset{};
            check(surface->BeginDraw(nullptr,IID_PPV_ARGS(&texture),&offset));
            Ptr<ID2D1RenderTarget> canvas;
            const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
            HRESULT result=factory_->CreateDxgiSurfaceRenderTarget(texture.Get(),&properties,&canvas);
            if(SUCCEEDED(result)){
                Ptr<ID2D1SolidColorBrush> brush;result=canvas->CreateSolidColorBrush(D2D1::ColorF(19/255.0f,22/255.0f,29/255.0f,1),&brush);
                if(SUCCEEDED(result)){canvas->BeginDraw();canvas->FillRectangle(D2D1::RectF(float(offset.x),float(offset.y),float(offset.x+1),float(offset.y+1)),brush.Get());result=canvas->EndDraw();}
            }
            const HRESULT end=surface->EndDraw();check(result);check(end);
            check(device_->CreateVisual(&backdrop_));check(backdrop_->SetContent(surface.Get()));
            check(device_->CreateScaleTransform(&backdrop_scale_));check(backdrop_->SetTransform(backdrop_scale_.Get()));
            check(visual_->AddVisual(backdrop_.Get(),FALSE,preview_.Get()));
            check(preview_->SetEffect(list_opacity_.Get()));
        }
        check(backdrop_scale_->SetScaleX(width));check(backdrop_scale_->SetScaleY(height));
        check(clip_->SetLeft(0.0f));check(clip_->SetTop(0.0f));check(clip_->SetRight(width));check(clip_->SetBottom(height));
        check(list_clip_->SetTop(height));check(list_clip_->SetBottom(height));
        if(discard){check(preview_->SetContent(nullptr));check(list_->SetContent(nullptr));detached_=true;}
        fade(1,1,0);
        if(entering&&animations()){auto motion=tween(0,1,.18f);check(list_opacity_->SetOpacity(motion.Get()));}
        else check(list_opacity_->SetOpacity(1.0f));
        check(device_->Commit());
    }
    void create(HWND window) {
        window_=window;
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
    void present(const void* bgra,UINT width,UINT height,bool commit=true) {
        if(!card_layers_.empty()&&(width!=width_||height!=height_||(cards_started_&&GetTickCount64()-cards_started_>=300)))finish_cards(false);
        if(width!=width_||height!=height_) {
            surface_.Reset();check(device_->CreateSurface(width,height,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&surface_));
            check(preview_->SetContent(surface_.Get()));check(list_->SetContent(surface_.Get()));width_=width;height_=height;
        }
        if(detached_){check(preview_->SetContent(surface_.Get()));check(list_->SetContent(surface_.Get()));detached_=false;}
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
        const HRESULT end=surface_->EndDraw();check(result);check(end);if(commit)check(device_->Commit());
        // Read-only UI diagnostics: confirms the uploaded surface, not WM_PAINT
        // delivery or the HWND size. Useful on an invisible test window station.
        SetPropW(window_,L"HiveDesk.SurfaceWidth",reinterpret_cast<HANDLE>(ULONG_PTR(width)));
        SetPropW(window_,L"HiveDesk.SurfaceHeight",reinterpret_cast<HANDLE>(ULONG_PTR(height)));
        SetPropW(window_,L"HiveDesk.SurfaceRevision",reinterpret_cast<HANDLE>(++revision_));
    }
    void show(float width,float height,float previous_height,bool entering,bool reversing) {
        finish_cards(false);RemovePropW(window_,L"HiveDesk.CardAnimationMs");RemovePropW(window_,L"HiveDesk.AnimatedCards");
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
