#include "intel_roundtrip.h"
#include "hevc_format.h"
#include <vpl/mfxmemory.h>
#include <windows.h>
#include <d3d11.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <cmath>

namespace {
using Clock=std::chrono::steady_clock;
constexpr unsigned Width=640,Height=360,Frames=16;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void exact(mfxStatus status,const char* stage){if(status!=MFX_ERR_NONE)throw std::runtime_error(std::string(stage)+" status="+std::to_string(status));}
void sync(mfxSession session,mfxSyncPoint point){
    const auto deadline=Clock::now()+std::chrono::seconds(3);
    for(;;){const auto s=MFXVideoCORE_SyncOperation(session,point,50);if(s==MFX_ERR_NONE)return;
        if(s!=MFX_WRN_IN_EXECUTION||Clock::now()>=deadline)throw std::runtime_error("Intel sync status="+std::to_string(s));}
}
struct Surface {
    mfxFrameSurface1* value=nullptr;bool mapped=false;
    ~Surface(){if(value&&value->FrameInterface){if(mapped)value->FrameInterface->Unmap(value);value->FrameInterface->Release(value);}}
    void map(mfxU32 mode){
        require(value&&value->FrameInterface,"Intel managed surface interface missing");
        mfxHDL native=nullptr;mfxResourceType type{};
        exact(value->FrameInterface->GetNativeHandle(value,&native,&type),"GetNativeHandle");
        require(native&&type==MFX_RESOURCE_DX11_TEXTURE,"Intel surface is not D3D11 video memory");
        D3D11_TEXTURE2D_DESC desc{};static_cast<ID3D11Texture2D*>(native)->GetDesc(&desc);
        require(desc.Format==DXGI_FORMAT_AYUV,"Intel native texture lost AYUV format");
        exact(value->FrameInterface->Map(value,mode),"Map generated surface");mapped=true;
        require(value->Info.FourCC==MFX_FOURCC_AYUV&&value->Info.ChromaFormat==MFX_CHROMAFORMAT_YUV444,"Intel mapped surface changed chroma");
        const auto pitch=(uint32_t(value->Data.PitchHigh)<<16)|value->Data.PitchLow;
        require(value->Data.Y&&value->Data.U&&value->Data.V&&pitch>=uint32_t(value->Info.Width)*4,"Intel mapped AYUV planes invalid");
    }
    void unmap(){exact(value->FrameInterface->Unmap(value),"Unmap generated surface");mapped=false;}
};
std::array<uint8_t,3> pixel(unsigned x,unsigned y,unsigned frame){
    return {uint8_t(16+(x*3+y*5+frame*7)%220),uint8_t(x&1?48:208),uint8_t(y&1?64:192)};
}
bool format(const mfxVideoParam& p){
    const auto& f=p.mfx.FrameInfo;
    return p.mfx.CodecId==MFX_CODEC_HEVC&&f.FourCC==MFX_FOURCC_AYUV&&f.ChromaFormat==MFX_CHROMAFORMAT_YUV444&&
        f.BitDepthLuma==8&&f.BitDepthChroma==8&&f.CropW==Width&&f.CropH==Height;
}
}
std::string intel_roundtrip(mfxSession session){
    mfxVersion api{};exact(MFXQueryVersion(session,&api),"QueryVersion");require(api.Major>=2,"Intel managed-surface test requires VPL 2.x");
    mfxVideoParam p{};p.AsyncDepth=1;p.IOPattern=MFX_IOPATTERN_IN_VIDEO_MEMORY;
    p.mfx.CodecId=MFX_CODEC_HEVC;p.mfx.CodecProfile=MFX_PROFILE_HEVC_REXT;
    p.mfx.TargetUsage=MFX_TARGETUSAGE_BEST_SPEED;p.mfx.GopRefDist=1;p.mfx.GopPicSize=120;p.mfx.NumRefFrame=1;
    p.mfx.RateControlMethod=MFX_RATECONTROL_CBR;p.mfx.BRCParamMultiplier=2;p.mfx.TargetKbps=50000;
    auto& f=p.mfx.FrameInfo;f.FourCC=MFX_FOURCC_AYUV;f.ChromaFormat=MFX_CHROMAFORMAT_YUV444;f.BitDepthLuma=f.BitDepthChroma=8;
    f.Width=Width;f.Height=368;f.CropW=Width;f.CropH=Height;f.FrameRateExtN=60;f.FrameRateExtD=1;f.PicStruct=MFX_PICSTRUCT_PROGRESSIVE;
    auto requested=p;exact(MFXVideoENCODE_Query(session,&requested,&p),"HEVC444 encode Query");
    require(format(p)&&p.mfx.GopRefDist==1&&p.AsyncDepth==1&&p.mfx.RateControlMethod==MFX_RATECONTROL_CBR,"Intel query changed the required format/low-delay configuration");
    // Close the encoder before freeing bitstream storage, including on a sync
    // timeout: outstanding asynchronous driver work may still own this memory.
    std::vector<uint8_t> storage(4*1024*1024),encoded;
    mfxBitstream bits{};bits.Data=storage.data();bits.MaxLength=mfxU32(storage.size());unsigned outputs=0;
    struct Encoder {mfxSession s;bool open=false;~Encoder(){if(open)MFXVideoENCODE_Close(s);}} encoder{session};
    const auto initialized=MFXVideoENCODE_Init(session,&p);encoder.open=initialized>=0;exact(initialized,"HEVC444 encode Init");
    mfxVideoParam actual{};exact(MFXVideoENCODE_GetVideoParam(session,&actual),"Encode GetVideoParam");
    require(format(actual)&&actual.mfx.GopRefDist==1,"Intel initialized encoder changed format or frame ordering");
    const auto encode=[&](mfxFrameSurface1* surface){
        bits.DataOffset=bits.DataLength=0;mfxSyncPoint point=nullptr;
        const auto deadline=Clock::now()+std::chrono::seconds(3);mfxStatus status;
        for(;;){status=MFXVideoENCODE_EncodeFrameAsync(session,nullptr,surface,&bits,&point);
            if(status!=MFX_WRN_DEVICE_BUSY||point)break;
            require(Clock::now()<deadline,"Intel encode device busy timeout");Sleep(1);}
        if(status==MFX_ERR_MORE_DATA)return false;
        exact(status,"EncodeFrameAsync");require(point!=nullptr,"Intel encode returned no completion");sync(session,point);
        require(bits.DataLength&&uint64_t(bits.DataOffset)+bits.DataLength<=storage.size(),"Intel invalid encoded range");
        require(encoded.size()+bits.DataLength<=64ull*1024*1024,"Intel generated stream exceeded diagnostic bound");
        encoded.insert(encoded.end(),bits.Data+bits.DataOffset,bits.Data+bits.DataOffset+bits.DataLength);++outputs;return true;
    };
    for(unsigned i=0;i<Frames;++i){
        Surface surface;exact(MFXMemory_GetSurfaceForEncode(session,&surface.value),"GetSurfaceForEncode");surface.map(MFX_MAP_WRITE);
        auto& data=surface.value->Data;const auto pitch=(uint32_t(data.PitchHigh)<<16)|data.PitchLow;
        for(unsigned y=0;y<surface.value->Info.Height;++y)for(unsigned x=0;x<surface.value->Info.Width;++x){
            const auto c=pixel(x,y,i);const auto at=size_t(y)*pitch+x*4;data.Y[at]=c[0];data.U[at]=c[1];data.V[at]=c[2];if(data.A)data.A[at]=255;
        }
        surface.unmap();data.FrameOrder=i;data.TimeStamp=i*1500;encode(surface.value);
    }
    unsigned drain=0;while(encode(nullptr))require(++drain<=Frames,"Intel encode drain did not end");
    require(outputs==Frames,"Intel encoded frame count changed");
    const auto sps=hive::hevc_annexb_format(encoded);
    require(sps&&sps->full_chroma8()&&sps->width==Width&&sps->height==Height,"Actual Intel bitstream is not 640x360 8-bit HEVC 4:4:4");
    exact(MFXVideoENCODE_Close(session),"Encode Close");encoder.open=false;

    mfxBitstream input{};input.Data=encoded.data();input.MaxLength=input.DataLength=mfxU32(encoded.size());
    mfxVideoParam decode{};decode.mfx.CodecId=MFX_CODEC_HEVC;
    exact(MFXVideoDECODE_DecodeHeader(session,&input,&decode),"DecodeHeader");
    require(format(decode),"Intel decoder header changed output format");
    decode.AsyncDepth=1;decode.IOPattern=MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    struct Decoder {mfxSession s;bool open=false;~Decoder(){if(open)MFXVideoDECODE_Close(s);}} decoder{session};
    const auto decoder_status=MFXVideoDECODE_Init(session,&decode);decoder.open=decoder_status>=0;exact(decoder_status,"HEVC444 decode Init");
    input.DataOffset=0;input.DataLength=mfxU32(encoded.size());
    unsigned decoded=0;bool draining=false;std::array<uint64_t,3> errors{};std::array<unsigned,3> maximum{};
    const auto deadline=Clock::now()+std::chrono::seconds(10);
    for(;;){
        require(Clock::now()<deadline,"Intel decode drain timeout");Surface output;mfxSyncPoint point=nullptr;
        if(!input.DataLength)draining=true;
        const auto status=MFXVideoDECODE_DecodeFrameAsync(session,draining?nullptr:&input,nullptr,&output.value,&point);
        if(status==MFX_ERR_MORE_DATA){if(draining)break;draining=true;continue;}
        if(status==MFX_WRN_DEVICE_BUSY&&!point){Sleep(1);continue;}
        exact(status,"DecodeFrameAsync");require(output.value&&point,"Intel decode returned no output/completion");sync(session,point);
        require(decoded<Frames,"Intel decoder produced extra frames");output.map(MFX_MAP_READ);
        const auto& data=output.value->Data;const auto& info=output.value->Info;
        require(info.CropW==Width&&info.CropH==Height,"Intel decoded dimensions changed");
        const auto pitch=(uint32_t(data.PitchHigh)<<16)|data.PitchLow;
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
            const auto expected=pixel(x,y,decoded);const auto at=size_t(y+info.CropY)*pitch+(x+info.CropX)*4;
            const std::array<uint8_t,3> received{data.Y[at],data.U[at],data.V[at]};
            for(unsigned c=0;c<3;++c){const auto error=unsigned(std::abs(int(received[c])-int(expected[c])));errors[c]+=error;maximum[c]=std::max(maximum[c],error);}
        }
        output.unmap();++decoded;
    }
    require(decoded==Frames,"Intel roundtrip lost frames");
    const double samples=double(Width)*Height*Frames;
    for(auto error:errors)require(double(error)/samples<12.0,"Intel generated full-chroma pattern error exceeds qualification bound");
    std::ostringstream report;report<<"generated_frames="<<decoded<<" actual_sps=HEVC444/8bit bytes="<<encoded.size();
    report<<" mean_abs_YUV="<<errors[0]/samples<<","<<errors[1]/samples<<","<<errors[2]/samples;
    report<<" max_abs_YUV="<<maximum[0]<<","<<maximum[1]<<","<<maximum[2];
    report<<" (D3D11 managed surfaces, async completion, CPU-mapped diagnostic pattern; not desktop streaming or latency validation)";return report.str();
}
