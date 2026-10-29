#include "software_decoder.h"
#include <windows.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace hive {
namespace {
// Resolve app-local libraries explicitly before the delay-load thunks run, so
// a damaged installation reports an error instead of an unhandled loader SEH.
void load_libraries(){
    struct Libraries {
        HMODULE utility=nullptr,codec=nullptr;
        Libraries(){
            std::wstring path(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
            if(!length||length>=path.size())throw std::runtime_error("Resolve software decoder directory");
            path.resize(length);path.resize(path.find_last_of(L"\\/")+1);
            utility=LoadLibraryExW((path+L"avutil-61.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(utility)codec=LoadLibraryExW((path+L"avcodec-63.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!codec){if(utility)FreeLibrary(utility);throw std::runtime_error("Packaged software decoder DLLs are unavailable; reinstall HiveDesk");}
        }
        ~Libraries(){FreeLibrary(codec);FreeLibrary(utility);}
    };
    static Libraries libraries;
}
void checked(int code,const char* operation){
    if(code>=0)return;
    char detail[AV_ERROR_MAX_STRING_SIZE]{};av_strerror(code,detail,sizeof(detail));
    throw std::runtime_error(std::string(operation)+": "+detail);
}
}
struct SoftwareDecoder::Impl {
    AVCodecContext* context=nullptr;
    AVPacket* packet=nullptr;
    AVFrame* frame=nullptr;
    ~Impl(){av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&context);}
};
SoftwareDecoder::SoftwareDecoder(){
    load_libraries();
    impl_=std::make_unique<Impl>();
    const auto* codec=avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if(!codec)throw std::runtime_error("Packaged HEVC software decoder unavailable");
    auto& d=*impl_;d.context=avcodec_alloc_context3(codec);d.packet=av_packet_alloc();d.frame=av_frame_alloc();
    if(!d.context||!d.packet||!d.frame)throw std::runtime_error("Allocate HEVC software decoder");
    d.context->flags|=AV_CODEC_FLAG_LOW_DELAY;
    // Frame threading deliberately buffers pictures; only within-picture
    // parallelism is suitable for the existing no-B-frame live stream.
    d.context->thread_type=FF_THREAD_SLICE;
    const auto cores=std::max(1u,std::thread::hardware_concurrency());
    d.context->thread_count=int(std::clamp(cores>2?cores-2:1u,1u,8u));
    d.context->max_pixels=8192ll*8192;
    d.context->err_recognition=AV_EF_EXPLODE;
    checked(avcodec_open2(d.context,codec,nullptr),"Open HEVC software decoder");
}
SoftwareDecoder::~SoftwareDecoder()=default;
uint32_t SoftwareDecoder::decode(const EncodedFrame& input,const std::function<void(const SoftwarePicture&)>& output){
    auto& d=*impl_;
    if(input.data.empty()||input.data.size()>64*1024*1024||!input.width||!input.height||input.width>8192||input.height>8192)
        throw std::runtime_error("Invalid software decode frame bounds");
    av_packet_unref(d.packet);av_frame_unref(d.frame);
    checked(av_new_packet(d.packet,int(input.data.size())),"Allocate padded HEVC packet");
    std::memcpy(d.packet->data,input.data.data(),input.data.size());
    d.packet->pts=d.packet->dts=int64_t(input.sequence);
    const auto start=std::chrono::steady_clock::now();
    checked(avcodec_send_packet(d.context,d.packet),"Submit HEVC software packet");
    checked(avcodec_receive_frame(d.context,d.frame),"Receive immediate HEVC software picture");
    const auto cost=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    if(d.frame->format!=AV_PIX_FMT_YUV444P||d.frame->width!=int(input.width)||d.frame->height!=int(input.height)||
        d.frame->pts!=int64_t(input.sequence)||(d.frame->flags&AV_FRAME_FLAG_CORRUPT)||d.frame->decode_error_flags)
        throw std::runtime_error("Software decoder rejected format, dimensions, reordering or damaged references");
    SoftwarePicture picture{input.width,input.height,uint32_t(std::clamp<int64_t>(cost,1,1000000))};
    for(size_t i=0;i<3;++i){
        if(!d.frame->data[i]||d.frame->linesize[i]<int(input.width))throw std::runtime_error("Invalid software picture plane");
        picture.planes[i]=d.frame->data[i];picture.strides[i]=d.frame->linesize[i];
    }
    output(picture);
    av_frame_unref(d.frame);av_packet_unref(d.packet);
    const auto extra=avcodec_receive_frame(d.context,d.frame);
    if(extra!=AVERROR(EAGAIN))throw std::runtime_error("Unexpected buffered HEVC software picture");
    return uint32_t(std::clamp<int64_t>(cost,1,1000000));
}
}
