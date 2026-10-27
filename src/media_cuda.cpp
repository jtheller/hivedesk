#include "media_cuda.h"
#include "color_kernels.h"
#include <windows.h>
#include <ffnvcodec/dynlink_cuda.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <array>

// Load only the stable driver API from Windows' NVIDIA driver. In particular,
// this file must never link cudart, NVRTC or import a versioned CUDA toolkit DLL.
struct HiveGpu {
    HMODULE driver = nullptr;
    CUdeviceptr rgba = 0;
    size_t allocated = 0;
    CUgraphicsResource graphics = nullptr;
    void* texture = nullptr;
    struct RegisteredTexture {void* texture=nullptr;CUgraphicsResource graphics=nullptr;uint64_t used=0;};
    // A live viewer rotates three RGB handoff surfaces. Keep their expensive
    // D3D/CUDA registrations persistent, with a bounded LRU for size changes.
    // Only the conversion worker accesses this cache, after its previous
    // conversion has unmapped and completed on our stream.
    std::array<RegisteredTexture,3> registrations{};
    uint64_t registration_use=0;
    CUmodule module = nullptr;
    CUfunction encode = nullptr, decode = nullptr;
    CUstream stream = nullptr;
    char error[512]{};
    tcuMemAlloc_v2* alloc{}; tcuMemFree_v2* free{};
    tcuMemcpy2D_v2* copy{};
    tcuMemcpy2DAsync_v2* copy_async{};
    tcuStreamCreate* create_stream{}; tcuStreamDestroy_v2* destroy_stream{};
    tcuStreamSynchronize* synchronize{};
    tcuGraphicsD3D11RegisterResource* register_texture{};
    tcuGraphicsUnregisterResource* unregister_texture{};
    tcuGraphicsMapResources* map{}; tcuGraphicsUnmapResources* unmap{};
    tcuGraphicsSubResourceGetMappedArray* array{};
    tcuModuleLoadData* load_module{}; tcuModuleUnload* unload_module{};
    tcuModuleGetFunction* function{}; tcuLaunchKernel* launch{};
    tcuGetErrorString* describe{};
    template<class F> void bind(F*& pointer, const char* symbol) {
        pointer = reinterpret_cast<F*>(GetProcAddress(driver,symbol));
        if(!pointer)throw std::runtime_error(std::string("NVIDIA driver function unavailable: ")+symbol);
    }
    HiveGpu() {
        driver=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!driver)throw std::runtime_error("NVIDIA CUDA driver is not installed.");
        try {
            bind(alloc,"cuMemAlloc_v2");bind(free,"cuMemFree_v2");bind(copy,"cuMemcpy2D_v2");
            bind(copy_async,"cuMemcpy2DAsync_v2");
            bind(create_stream,"cuStreamCreate");bind(destroy_stream,"cuStreamDestroy_v2");
            bind(synchronize,"cuStreamSynchronize");
            bind(register_texture,"cuGraphicsD3D11RegisterResource");bind(unregister_texture,"cuGraphicsUnregisterResource");
            bind(map,"cuGraphicsMapResources");bind(unmap,"cuGraphicsUnmapResources");bind(array,"cuGraphicsSubResourceGetMappedArray");
            bind(load_module,"cuModuleLoadData");bind(unload_module,"cuModuleUnload");
            bind(function,"cuModuleGetFunction");bind(launch,"cuLaunchKernel");
            bind(describe,"cuGetErrorString");
        } catch(...) {FreeLibrary(driver);driver=nullptr;throw;}
    }
    ~HiveGpu() {
        // Failed conversions may still have queued work. Finish our own work
        // before unregistering textures or freeing scratch memory.
        if(stream)synchronize(stream);
        for(auto& entry:registrations)if(entry.graphics)unregister_texture(entry.graphics);
        if(rgba)free(rgba);
        if(module)unload_module(module);
        if(stream)destroy_stream(stream);
        if(driver)FreeLibrary(driver);
    }
    bool check(CUresult result, const char* operation) noexcept {
        if(result==CUDA_SUCCESS)return true;
        const char* detail=nullptr;describe(result,&detail);
        std::snprintf(error,sizeof(error),"%s: CUDA %d (%s)",operation,int(result),detail?detail:"unknown driver error");return false;
    }
    bool prepare(uint32_t width,uint32_t height) noexcept {
        if(!width||!height||width>8192||height>8192){std::snprintf(error,sizeof(error),"Invalid color-conversion dimensions");return false;}
        if(!stream&&!check(create_stream(&stream,CU_STREAM_NON_BLOCKING),"Create color-conversion stream"))return false;
        if(!module){
            if(!check(load_module(&module,HiveColorPtx),"Load compatible PTX 6.4 color kernels"))return false;
            if(!check(function(&encode,module,"hive_bgra_to_yuv"),"Find BGRA to YUV444 kernel")||
               !check(function(&decode,module,"hive_yuv_to_bgra"),"Find YUV444 to BGRA kernel")){
                unload_module(module);module=nullptr;encode=decode=nullptr;return false;
            }
        }
        const size_t bytes=size_t(width)*height*4;
        if(bytes<=allocated)return true;
        if(rgba&&!check(free(rgba),"Free previous BGRA scratch"))return false;
        rgba=0;allocated=0;
        if(!check(alloc(&rgba,bytes),"Allocate BGRA scratch using CUDA driver"))return false;
        allocated=bytes;return true;
    }
    bool register_resource(void* value) noexcept {
        for(auto& entry:registrations)if(entry.texture==value&&entry.graphics){
            entry.used=++registration_use;texture=value;graphics=entry.graphics;return true;
        }
        auto* victim=&registrations[0];
        for(auto& entry:registrations){if(!entry.graphics){victim=&entry;break;}if(entry.used<victim->used)victim=&entry;}
        if(victim->graphics){
            if(!check(unregister_texture(victim->graphics),"Unregister replaced D3D11 texture"))return false;
            if(graphics==victim->graphics){graphics=nullptr;texture=nullptr;}
            *victim={};
        }
        CUgraphicsResource registered=nullptr;
        if(!check(register_texture(&registered,value,0),"Register D3D11 texture with CUDA driver"))return false;
        *victim={value,registered,++registration_use};texture=value;graphics=registered;return true;
    }
    bool convert(bool encoding,uint64_t yuv,size_t pitch,uint32_t width,uint32_t height,CUstream work_stream) noexcept {
        if(pitch<width){std::snprintf(error,sizeof(error),"Invalid YUV444 pitch");return false;}
        void* arguments[]{&rgba,&yuv,&pitch,&width,&height};
        return check(launch(encoding?encode:decode,(width+31)/32,(height+7)/8,1,32,8,1,0,work_stream,arguments,nullptr),
            encoding?"Launch BGRA to YUV444 kernel":"Launch YUV444 to BGRA kernel");
    }
};

HiveGpu* hive_gpu_create(){return new HiveGpu;}
void hive_gpu_destroy(HiveGpu* gpu) noexcept{delete gpu;}
const char* hive_gpu_error(HiveGpu* gpu) noexcept{return gpu?gpu->error:"CUDA converter is null";}

bool hive_gpu_encode_host(HiveGpu* gpu,const uint8_t* bgra,uint32_t width,uint32_t height,uint64_t yuv,size_t pitch) noexcept {
    if(!gpu->prepare(width,height))return false;
    CUDA_MEMCPY2D transfer{};transfer.srcMemoryType=CU_MEMORYTYPE_HOST;transfer.srcHost=bgra;transfer.srcPitch=size_t(width)*4;
    transfer.dstMemoryType=CU_MEMORYTYPE_DEVICE;transfer.dstDevice=gpu->rgba;transfer.dstPitch=transfer.srcPitch;
    transfer.WidthInBytes=transfer.srcPitch;transfer.Height=height;
    // Keep pageable-host transfers and their dependent kernel on the same
    // default stream: a synchronous HtoD copy may return after host staging.
    return gpu->check(gpu->copy(&transfer),"Upload test BGRA")&&gpu->convert(true,yuv,pitch,width,height,nullptr)&&
        gpu->check(gpu->synchronize(nullptr),"Finish encode color conversion");
}
bool hive_gpu_decode_host(HiveGpu* gpu,uint64_t yuv,size_t pitch,uint32_t width,uint32_t height,uint8_t* bgra) noexcept {
    if(!gpu->prepare(width,height)||!gpu->convert(false,yuv,pitch,width,height,nullptr))return false;
    CUDA_MEMCPY2D transfer{};transfer.srcMemoryType=CU_MEMORYTYPE_DEVICE;transfer.srcDevice=gpu->rgba;transfer.srcPitch=size_t(width)*4;
    transfer.dstMemoryType=CU_MEMORYTYPE_HOST;transfer.dstHost=bgra;transfer.dstPitch=transfer.srcPitch;
    transfer.WidthInBytes=transfer.srcPitch;transfer.Height=height;
    return gpu->check(gpu->copy(&transfer),"Read test decoded BGRA");
}
namespace {
bool convert_texture(HiveGpu* gpu,void* texture,uint32_t width,uint32_t height,uint64_t yuv,size_t pitch,bool encoding) noexcept {
    if(!gpu->prepare(width,height)||!gpu->register_resource(texture))return false;
    // Map, device copy, kernel and unmap share one nonblocking stream. Graphics
    // interoperability supplies the D3D/CUDA ordering; a context-wide fence or
    // legacy-stream copy additionally waits for other work in this context.
    if(!gpu->check(gpu->map(1,&gpu->graphics,gpu->stream),"Map D3D11 texture"))return false;
    CUarray array=nullptr;
    bool ok=gpu->check(gpu->array(&array,gpu->graphics,0,0),"Get mapped D3D11 array");
    CUDA_MEMCPY2D transfer{};transfer.WidthInBytes=size_t(width)*4;transfer.Height=height;
    if(encoding){
        transfer.srcMemoryType=CU_MEMORYTYPE_ARRAY;transfer.srcArray=array;
        transfer.dstMemoryType=CU_MEMORYTYPE_DEVICE;transfer.dstDevice=gpu->rgba;transfer.dstPitch=transfer.WidthInBytes;
        if(ok)ok=gpu->check(gpu->copy_async(&transfer,gpu->stream),"Copy capture texture on GPU");
        if(ok)ok=gpu->convert(true,yuv,pitch,width,height,gpu->stream);
    }else{
        transfer.srcMemoryType=CU_MEMORYTYPE_DEVICE;transfer.srcDevice=gpu->rgba;transfer.srcPitch=transfer.WidthInBytes;
        transfer.dstMemoryType=CU_MEMORYTYPE_ARRAY;transfer.dstArray=array;
        if(ok)ok=gpu->convert(false,yuv,pitch,width,height,gpu->stream);
        if(ok)ok=gpu->check(gpu->copy_async(&transfer,gpu->stream),"Copy decoded texture on GPU");
    }
    // Always unmap, including failed conversion. Preserve the first useful error.
    const auto unmapped=gpu->unmap(1,&gpu->graphics,gpu->stream);
    // Complete before handing YUV to NVENC or returning a mapped NVDEC frame.
    // Also drain failed submissions so callers can safely tear down resources.
    const auto completed=gpu->synchronize(gpu->stream);
    if(!ok)return false;
    return gpu->check(unmapped,"Unmap D3D11 texture")&&gpu->check(completed,"Finish GPU color conversion");
}
}
bool hive_gpu_encode_texture(HiveGpu* gpu,void* texture,uint32_t width,uint32_t height,uint64_t yuv,size_t pitch) noexcept {
    return convert_texture(gpu,texture,width,height,yuv,pitch,true);
}
bool hive_gpu_decode_texture(HiveGpu* gpu,uint64_t yuv,size_t pitch,uint32_t width,uint32_t height,void* texture) noexcept {
    return convert_texture(gpu,texture,width,height,yuv,pitch,false);
}
