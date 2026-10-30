#include "intel_encoder.h"
#include "intel_color.h"
#include "intel_system_surfaces.h"
#include "intel_probe_limits.h"
#include "hevc_format.h"
#include "hivedesk/auto_rate.h"
#include "hivedesk/config.h"
#include <vpl/mfxdispatcher.h>
#include <d3d10.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace hive {
namespace {
using Clock=std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
uint64_t tick(){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());}
uint32_t elapsed(uint64_t end,uint64_t start){return uint32_t(std::min<uint64_t>(10000000,end>=start?end-start:0));}
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void exact(mfxStatus status,const char* stage){if(status!=MFX_ERR_NONE)throw std::runtime_error(std::string(stage)+" Intel status="+std::to_string(status));}
bool configuration_status(mfxStatus status){return status==MFX_ERR_NONE||status==MFX_WRN_INCOMPATIBLE_VIDEO_PARAM;}
template<class T> void extension(T& value,mfxU32 id){value={};value.Header.BufferId=id;value.Header.BufferSz=sizeof(T);}
struct Parameters {
    mfxVideoParam video{};
    mfxExtCodingOption coding{};
    mfxExtCodingOption2 quality{};
    mfxExtVideoSignalInfo signal{};
    mfxExtEncoderResetOption reset{};
    std::array<mfxExtBuffer*,4> extensions{};
    Parameters(){
        extension(coding,MFX_EXTBUFF_CODING_OPTION);extension(quality,MFX_EXTBUFF_CODING_OPTION2);
        extension(signal,MFX_EXTBUFF_VIDEO_SIGNAL_INFO);extension(reset,MFX_EXTBUFF_ENCODER_RESET_OPTION);bind(false);
    }
    Parameters(const Parameters& other):video(other.video),coding(other.coding),quality(other.quality),signal(other.signal),reset(other.reset){bind(other.video.NumExtParam==4);}
    Parameters& operator=(const Parameters& other){video=other.video;coding=other.coding;quality=other.quality;signal=other.signal;reset=other.reset;bind(other.video.NumExtParam==4);return *this;}
    void bind(bool resetting){extensions={&coding.Header,&quality.Header,&signal.Header,&reset.Header};video.ExtParam=extensions.data();video.NumExtParam=resetting?4:3;}
};
void set_bitrate(Parameters& p,uint32_t bitrate){
    require(bitrate>=1000000&&bitrate<=1000000000,"Intel bitrate out of bounds");
    // Fixed multiplier permits live changes up to 1 Gbit/s without changing
    // the initial bitstream allocation. Quantization is at most 16 kbit/s.
    p.video.mfx.BRCParamMultiplier=16;
    p.video.mfx.TargetKbps=p.video.mfx.MaxKbps=mfxU16((uint64_t(bitrate)+15999)/16000);
}
bool format(const Parameters& p,uint32_t width,uint32_t height){
    const auto& v=p.video;const auto& f=v.mfx.FrameInfo;
    return v.mfx.CodecId==MFX_CODEC_HEVC&&v.mfx.CodecProfile==MFX_PROFILE_HEVC_REXT&&
        f.FourCC==MFX_FOURCC_AYUV&&f.ChromaFormat==MFX_CHROMAFORMAT_YUV444&&f.BitDepthLuma==8&&f.BitDepthChroma==8&&
        f.CropW==width&&f.CropH==height&&v.mfx.GopRefDist==1&&v.AsyncDepth==1&&v.IOPattern==MFX_IOPATTERN_IN_SYSTEM_MEMORY&&
        v.mfx.RateControlMethod==MFX_RATECONTROL_CBR&&p.quality.MaxQPI==AutoRateMaxQp&&p.quality.MaxQPP==AutoRateMaxQp&&
        p.signal.VideoFullRange==1&&p.signal.ColourDescriptionPresent==1&&p.signal.MatrixCoefficients==1;
}
}
struct IntelEncoder::Impl {
    mfxLoader loader=nullptr;mfxSession session=nullptr;bool opened=false;
    Parameters parameters;
    ComPtr<ID3D11Texture2D> source;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    std::unique_ptr<IntelColor> color;IntelSystemSurfaces surfaces;
    struct Slot {
        std::vector<uint8_t> storage;mfxBitstream bits{};mfxSyncPoint point=nullptr;
        mfxExtEncodedFrameInfo info{};mfxExtBuffer* info_pointer=nullptr;
        bool used=false,force=false;uint64_t sequence=0,timestamp=0,captured=0,started=0,converted=0,submitted=0;
        CursorComposition cursor=CursorComposition::Unknown;
    };
    std::array<Slot,2> slots;
    uint32_t width=0,height=0,bitrate=0,millihz=0,count=0;
    bool automatic=false,dynamic=false,frame_info=false,finishing=false;
    mutable std::mutex mutex;std::condition_variable changed;std::deque<unsigned> pending;std::exception_ptr failure;
    std::function<bool()> cancelled;
    ~Impl(){
        // All external workers are joined before destruction. Keep buffers and
        // surfaces alive through Close even after a failed/timed-out Sync.
        if(opened)MFXVideoENCODE_Close(session);if(session)MFXClose(session);if(loader)MFXUnload(loader);
    }
    void filter(const char* name,mfxU32 value){
        auto cfg=MFXCreateConfig(loader);require(cfg!=nullptr,"Intel configuration unavailable");
        mfxVariant variant{};variant.Type=MFX_VARIANT_TYPE_U32;variant.Data.U32=value;
        exact(MFXSetConfigFilterProperty(cfg,reinterpret_cast<const mfxU8*>(name),variant),"Intel adapter filter");
    }
    template<class T> bool supports(mfxU32 id){
        T option{};extension(option,id);mfxExtBuffer* pointer=&option.Header;
        mfxVideoParam query{};query.mfx.CodecId=MFX_CODEC_HEVC;query.ExtParam=&pointer;query.NumExtParam=1;
        return MFXVideoENCODE_Query(session,nullptr,&query)==MFX_ERR_NONE;
    }
    void initialize(uint32_t w,uint32_t h,const StreamSettings& settings,IDXGIAdapter* adapter,ID3D11Texture2D* texture,std::function<bool()> stop){
        require(w&&h&&w<=8192&&h<=8192&&!(w&1)&&!(h&1),"Intel HEVC444 requires even primary dimensions up to 8192");
        width=w;height=h;source=texture;cancelled=std::move(stop);require(texture&&adapter,"Intel primary capture resources missing");
        DXGI_ADAPTER_DESC wanted{};require(SUCCEEDED(adapter->GetDesc(&wanted))&&wanted.VendorId==0x8086,"Intel encoder adapter mismatch");
        // Input is owned system-memory AYUV, not a shared D3D texture. Give the
        // runtime its own device on the capture adapter: DXGI's blocking idle
        // acquisition must not compete with codec workers for the capture
        // device/context lock. Color conversion stays on the capture device.
        const auto created=D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
        if(FAILED(created))throw std::runtime_error("Intel dedicated codec device HRESULT="+std::to_string(uint32_t(created)));
        ComPtr<ID3D10Multithread> multithread;require(SUCCEEDED(context.As(&multithread)),"Intel D3D11 multithread interface");multithread->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIFactory1> factory;require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"Intel DXGI factory");
        UINT index=0;for(;;++index){ComPtr<IDXGIAdapter1> candidate;require(factory->EnumAdapters1(index,&candidate)!=DXGI_ERROR_NOT_FOUND,"Intel primary adapter not enumerated");DXGI_ADAPTER_DESC1 desc{};
            require(SUCCEEDED(candidate->GetDesc1(&desc)),"Intel adapter identity");if(desc.AdapterLuid.LowPart==wanted.AdapterLuid.LowPart&&desc.AdapterLuid.HighPart==wanted.AdapterLuid.HighPart)break;}
        loader=MFXLoad();require(loader!=nullptr,"Intel VPL dispatcher unavailable");
        filter("mfxImplDescription.Impl",MFX_IMPL_TYPE_HARDWARE);filter("mfxImplDescription.VendorID",0x8086);
        filter("mfxImplDescription.AccelerationMode",MFX_ACCEL_MODE_VIA_D3D11);filter("DXGIAdapterIndex",index);
        exact(MFXCreateSession(loader,0,&session),"Intel hardware session");exact(MFXVideoCORE_SetHandle(session,MFX_HANDLE_D3D11_DEVICE,device.Get()),"Intel primary D3D11 binding");
        automatic=settings.session&&!settings.diagnostic_constqp;
        millihz=settings.session?std::clamp(settings.receiver_millihz,1000u,4000000u):(settings.fps?settings.fps:120)*1000;
        bitrate=automatic?initial_auto_bitrate(w,h,millihz):settings.bitrate_mbps*1000000u;
        auto& p=parameters;auto& v=p.video;auto& f=v.mfx.FrameInfo;
        // AsyncDepth > 1 makes Media SDK 1.35 retain the first HEVC picture
        // (MORE_DATA), which deadlocks single-picture warm/idle desktops.
        // Two application slots still overlap preparation with completion;
        // the codec itself must produce an output from its first input.
        v.AsyncDepth=1;v.IOPattern=MFX_IOPATTERN_IN_SYSTEM_MEMORY;v.mfx.CodecId=MFX_CODEC_HEVC;v.mfx.CodecProfile=MFX_PROFILE_HEVC_REXT;
        v.mfx.TargetUsage=MFX_TARGETUSAGE_BEST_SPEED;v.mfx.GopRefDist=1;v.mfx.GopPicSize=65535;v.mfx.NumRefFrame=1;v.mfx.RateControlMethod=MFX_RATECONTROL_CBR;
        f.FourCC=MFX_FOURCC_AYUV;f.ChromaFormat=MFX_CHROMAFORMAT_YUV444;f.BitDepthLuma=f.BitDepthChroma=8;
        f.Width=mfxU16((w+15)&~15u);f.Height=mfxU16((h+15)&~15u);f.CropW=mfxU16(w);f.CropH=mfxU16(h);f.PicStruct=MFX_PICSTRUCT_PROGRESSIVE;
        set_bitrate(p,bitrate);f.FrameRateExtN=millihz;f.FrameRateExtD=1000;
        v.mfx.BufferSizeInKB=mfxU16(std::max<uint64_t>(1,(uint64_t(w)*h*3+15999)/16000));v.mfx.InitialDelayInKB=v.mfx.BufferSizeInKB;
        p.coding.NalHrdConformance=MFX_CODINGOPTION_OFF;p.coding.VuiNalHrdParameters=MFX_CODINGOPTION_OFF;
        p.quality.RepeatPPS=MFX_CODINGOPTION_ON;
        p.quality.MinQPI=p.quality.MinQPP=p.quality.MinQPB=AutoRateMinQp;p.quality.MaxQPI=p.quality.MaxQPP=p.quality.MaxQPB=AutoRateMaxQp;
        p.signal.VideoFormat=5;p.signal.VideoFullRange=1;p.signal.ColourDescriptionPresent=1;
        p.signal.ColourPrimaries=p.signal.TransferCharacteristics=p.signal.MatrixCoefficients=1;
        Parameters requested=p;const auto query=MFXVideoENCODE_Query(session,&requested.video,&p.video);
        if(!configuration_status(query))exact(query,"Intel HEVC444 low-delay query");require(format(p,w,h),"Intel driver changed required full-chroma/quality/latency parameters");
        const auto status=MFXVideoENCODE_Init(session,&p.video);opened=status>=0;if(!configuration_status(status))exact(status,"Intel HEVC444 initialize");
        exact(MFXVideoENCODE_GetVideoParam(session,&p.video),"Intel initialized parameters");require(format(p,w,h),"Intel initialized format/quality changed");
        mfxFrameAllocRequest allocation{};exact(MFXVideoENCODE_QueryIOSurf(session,&p.video,&allocation),"Intel surface requirements");
        surfaces.allocate(p.video.mfx.FrameInfo,std::max<unsigned>(4,allocation.NumFrameSuggested));
        color=std::make_unique<IntelColor>(texture,p.video.mfx.FrameInfo.Width,p.video.mfx.FrameInfo.Height);
        frame_info=supports<mfxExtEncodedFrameInfo>(MFX_EXTBUFF_ENCODED_FRAME_INFO);
        dynamic=automatic&&supports<mfxExtEncoderResetOption>(MFX_EXTBUFF_ENCODER_RESET_OPTION);
        const auto capacity=intel_probe_bitstream_capacity(p.video.mfx);
        for(auto& slot:slots){slot.storage.resize(capacity);slot.bits.Data=slot.storage.data();slot.bits.MaxLength=capacity;
            extension(slot.info,MFX_EXTBUFF_ENCODED_FRAME_INFO);slot.info_pointer=&slot.info.Header;
            if(frame_info){slot.bits.ExtParam=&slot.info_pointer;slot.bits.NumExtParam=1;}}
        log("Intel HEVC444 hardware encoder: dedicated codec D3D11 device, full-range BT.709, GPU color + system-memory upload, 2 IO slots / codec AsyncDepth 1; QP feedback="+std::to_string(frame_info)+", dynamic CBR="+std::to_string(dynamic));
    }
};
IntelEncoder::IntelEncoder(uint32_t w,uint32_t h,const StreamSettings& s,IDXGIAdapter* a,ID3D11Texture2D* t,std::function<bool()> cancelled):impl_(std::make_unique<Impl>()){impl_->initialize(w,h,s,a,t,std::move(cancelled));}
IntelEncoder::~IntelEncoder()=default;
bool IntelEncoder::ready()const{auto& d=*impl_;std::lock_guard lock(d.mutex);if(d.failure)std::rethrow_exception(d.failure);return !d.finishing&&d.count<d.slots.size();}
uint32_t IntelEncoder::in_flight()const{std::lock_guard lock(impl_->mutex);return impl_->count;}
bool IntelEncoder::automatic_rate()const{return impl_->automatic;}
bool IntelEncoder::dynamic_rate()const{return impl_->dynamic;}
uint32_t IntelEncoder::bitrate_bps()const{return impl_->bitrate;}
uint32_t IntelEncoder::nominal_millihz()const{return impl_->millihz;}
bool IntelEncoder::reconfigure(uint32_t bitrate,uint32_t millihz){
    auto& d=*impl_;{std::lock_guard lock(d.mutex);if(d.failure)std::rethrow_exception(d.failure);if(!d.dynamic||d.finishing||d.count)return false;}
    require(millihz>=1000&&millihz<=4000000,"Intel pacing refresh out of bounds");
    // Pacing may change without rewriting HEVC sequence timing. Intel only
    // documents bitrate/QP/slice changes as reference-preserving resets;
    // changing FrameRateExtN/D caused JT's Reset to reject a successful Query.
    // millihz acknowledges the scheduler's target, not a changed codec clock.
    if(bitrate==d.bitrate){d.millihz=millihz;return true;}
    Parameters change=d.parameters;set_bitrate(change,bitrate);change.reset.StartNewSequence=MFX_CODINGOPTION_OFF;change.bind(true);
    Parameters requested=change;const auto query=MFXVideoENCODE_Query(d.session,&requested.video,&change.video);
    const auto same_timing=[&](const Parameters& candidate){return
        candidate.video.mfx.FrameInfo.FrameRateExtN==d.parameters.video.mfx.FrameInfo.FrameRateExtN&&
        candidate.video.mfx.FrameInfo.FrameRateExtD==d.parameters.video.mfx.FrameInfo.FrameRateExtD;};
    if(!configuration_status(query)||change.reset.StartNewSequence!=MFX_CODINGOPTION_OFF||!format(change,d.width,d.height)||!same_timing(change)||
       intel_probe_bitstream_capacity(change.video.mfx)>d.slots.front().storage.size()){
        d.dynamic=false;log("Intel live CBR change unavailable; retaining codec state and adaptive frame pacing. Query="+std::to_string(query));return false;
    }
    const auto status=MFXVideoENCODE_Reset(d.session,&change.video);
    if(status==MFX_ERR_INVALID_VIDEO_PARAM||status==MFX_ERR_INCOMPATIBLE_VIDEO_PARAM||status==MFX_ERR_UNSUPPORTED){
        // Some legacy implementations accept Query but reject the live reset.
        // Keep streaming only if the runtime still reports the original usable
        // configuration. Device failures and changed state remain hard errors.
        Parameters retained=d.parameters;retained.bind(false);
        exact(MFXVideoENCODE_GetVideoParam(d.session,&retained.video),"Intel parameters after rejected CBR change");
        const auto& old=d.parameters.video.mfx;const auto& now=retained.video.mfx;
        require(format(retained,d.width,d.height)&&now.BRCParamMultiplier==old.BRCParamMultiplier&&
            now.TargetKbps==old.TargetKbps&&now.MaxKbps==old.MaxKbps&&now.BufferSizeInKB==old.BufferSizeInKB&&
            now.InitialDelayInKB==old.InitialDelayInKB&&now.FrameInfo.FrameRateExtN==old.FrameInfo.FrameRateExtN&&
            now.FrameInfo.FrameRateExtD==old.FrameInfo.FrameRateExtD&&now.GopPicSize==old.GopPicSize&&now.NumRefFrame==old.NumRefFrame,
            "Intel rejected CBR change did not retain original codec state");
        d.parameters=retained;d.dynamic=false;
        log("Intel live CBR reset rejected; original parameters verified, retaining codec and adaptive pacing. Reset="+std::to_string(status));
        return false;
    }
    if(!configuration_status(status))exact(status,"Intel reference-preserving CBR change");
    change.bind(false);
    exact(MFXVideoENCODE_GetVideoParam(d.session,&change.video),"Intel parameters after CBR change");
    require(format(change,d.width,d.height)&&same_timing(change),"Intel CBR change altered format, timing or quality");
    d.parameters=change;d.bitrate=bitrate;d.millihz=millihz;return true;
}
bool IntelEncoder::submit_texture(ID3D11Texture2D* texture,uint64_t sequence,bool force,uint64_t captured,bool* queued,CursorComposition cursor){
    if(queued)*queued=false;auto& d=*impl_;require(texture==d.source.Get(),"Intel capture texture changed within encoder lifetime");unsigned index=0;
    {std::lock_guard lock(d.mutex);if(d.failure)std::rethrow_exception(d.failure);if(d.finishing||d.count==d.slots.size())return false;
        while(d.slots[index].used)++index;d.slots[index].used=true;++d.count;}
    auto& slot=d.slots[index];try{
        slot.started=tick();auto* surface=d.surfaces.available();const auto pitch=(uint32_t(surface->Data.PitchHigh)<<16)|surface->Data.PitchLow;
        d.color->convert(surface->Data.V,pitch,d.cancelled);slot.converted=tick();surface->Data.FrameOrder=uint32_t(sequence);
        slot.timestamp=(captured/100)*9+(captured%100)*9/100;surface->Data.TimeStamp=slot.timestamp;
        slot.sequence=sequence;slot.captured=captured;slot.force=force;slot.cursor=cursor;
        slot.bits.DataOffset=slot.bits.DataLength=0;slot.bits.FrameType=0;slot.point=nullptr;slot.info.QP=UINT16_MAX;
        mfxEncodeCtrl control{};if(force)control.FrameType=MFX_FRAMETYPE_I|MFX_FRAMETYPE_IDR|MFX_FRAMETYPE_REF;
        const auto deadline=Clock::now()+std::chrono::seconds(3);slot.submitted=tick();
        for(;;){const auto status=MFXVideoENCODE_EncodeFrameAsync(d.session,&control,surface,&slot.bits,&slot.point);
            if(status==MFX_WRN_DEVICE_BUSY&&!slot.point){require(Clock::now()<deadline&&(!d.cancelled||!d.cancelled()),"Intel submission busy timeout or cancellation");Sleep(1);continue;}
            exact(status,"Intel immediate EncodeFrameAsync");break;}
        require(slot.point!=nullptr,"Intel encode buffered a picture; immediate output required");
        {std::lock_guard lock(d.mutex);d.pending.push_back(index);if(queued)*queued=true;}d.changed.notify_all();return true;
    }catch(...){
        {std::lock_guard lock(d.mutex);d.failure=std::current_exception();if(!slot.point){slot.used=false;--d.count;}}
        d.changed.notify_all();throw;
    }
}
bool IntelEncoder::receive(Completion& result){
    auto& d=*impl_;unsigned index;
    {std::unique_lock lock(d.mutex);d.changed.wait(lock,[&]{return d.failure||!d.pending.empty()||d.finishing;});if(d.failure)std::rethrow_exception(d.failure);if(d.pending.empty())return false;index=d.pending.front();}
    auto& slot=d.slots[index];try{
        const auto deadline=Clock::now()+std::chrono::seconds(3);
        for(;;){const auto status=MFXVideoCORE_SyncOperation(d.session,slot.point,50);if(status==MFX_ERR_NONE)break;
            if(status!=MFX_WRN_IN_EXECUTION||Clock::now()>=deadline)exact(status,"Intel encode completion");}
        require(slot.bits.DataLength&&uint64_t(slot.bits.DataOffset)+slot.bits.DataLength<=slot.storage.size(),"Intel encoded buffer bounds");
        require(slot.bits.TimeStamp==slot.timestamp,"Intel encoder reordered a desktop picture");
        result={};result.frame.data.assign(slot.bits.Data+slot.bits.DataOffset,slot.bits.Data+slot.bits.DataOffset+slot.bits.DataLength);
        result.frame.keyframe=(slot.bits.FrameType&MFX_FRAMETYPE_IDR)!=0;
        require(!slot.force||result.frame.keyframe,"Intel failed to produce requested recovery IDR");
        if(result.frame.keyframe){const auto sps=hevc_annexb_format(result.frame.data);require(sps&&sps->full_chroma8()&&sps->width==d.width&&sps->height==d.height,"Intel actual desktop bitstream is not original-size HEVC 4:4:4/8-bit");}
        result.frame.width=d.width;result.frame.height=d.height;result.frame.sequence=slot.sequence;result.frame.capture_time_us=slot.captured;result.frame.cursor_composition=slot.cursor;
        result.completed_us=tick();result.submitted_us=slot.submitted;result.frame.source_ready_us=result.completed_us;
        result.color_us=elapsed(slot.converted,slot.started);result.codec_us=elapsed(result.completed_us,slot.converted);result.total_us=elapsed(result.completed_us,slot.started);
        if(d.frame_info&&slot.info.QP<=51)result.average_qp=slot.info.QP;
        {std::lock_guard lock(d.mutex);d.pending.pop_front();slot.point=nullptr;slot.used=false;--d.count;}d.changed.notify_all();return true;
    }catch(...){std::lock_guard lock(d.mutex);d.failure=std::current_exception();d.changed.notify_all();throw;}
}
void IntelEncoder::finish(){std::lock_guard lock(impl_->mutex);impl_->finishing=true;impl_->changed.notify_all();}
}
