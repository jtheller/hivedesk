// Standalone vendor bring-up: never loaded by HiveDesk/Host, never advertises a
// working backend based on a capability query. No capture, network or input.
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#include "core/Factory.h"
#include "core/Version.h"
#include "components/VideoEncoderHEVC.h"
#include <cstdio>
#include <string>
#include <stdexcept>
#include <cstring>
#include "intel_roundtrip.h"

using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
std::string utf8(const wchar_t* value){
    const auto n=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
    std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,value,-1,out.data(),n,nullptr,nullptr);
    if(!out.empty())out.pop_back();return out;
}
struct Vpl {
    mfxLoader loader=MFXLoad();mfxSession session=nullptr;
    ~Vpl(){if(session)MFXClose(session);if(loader)MFXUnload(loader);}
    void filter(const char* name,mfxU32 value){
        require(loader!=nullptr,"Intel dispatcher unavailable");
        auto cfg=MFXCreateConfig(loader);require(cfg!=nullptr,"Intel configuration allocation");
        mfxVariant v{};v.Type=MFX_VARIANT_TYPE_U32;v.Data.U32=value;
        require(MFXSetConfigFilterProperty(cfg,reinterpret_cast<const mfxU8*>(name),v)==MFX_ERR_NONE,"Intel adapter filter unsupported");
    }
};
mfxVideoParam hevc444(bool encode){
    mfxVideoParam p{};p.AsyncDepth=1;
    p.IOPattern=mfxU16(encode?MFX_IOPATTERN_IN_VIDEO_MEMORY:MFX_IOPATTERN_OUT_VIDEO_MEMORY);
    p.mfx.CodecId=MFX_CODEC_HEVC;p.mfx.CodecProfile=MFX_PROFILE_HEVC_REXT;
    auto& f=p.mfx.FrameInfo;f.FourCC=MFX_FOURCC_AYUV;f.ChromaFormat=MFX_CHROMAFORMAT_YUV444;
    f.BitDepthLuma=8;f.BitDepthChroma=8;f.Width=1920;f.Height=1088;f.CropW=1920;f.CropH=1080;
    f.FrameRateExtN=60;f.FrameRateExtD=1;f.PicStruct=MFX_PICSTRUCT_PROGRESSIVE;
    if(encode){p.mfx.TargetUsage=MFX_TARGETUSAGE_BEST_SPEED;p.mfx.GopRefDist=1;p.mfx.GopPicSize=120;
        p.mfx.NumRefFrame=1;p.mfx.RateControlMethod=MFX_RATECONTROL_CBR;p.mfx.TargetKbps=30000;}
    return p;
}
void intel(UINT index,ID3D11Device* device,bool roundtrip){
    Vpl vpl;vpl.filter("mfxImplDescription.Impl",MFX_IMPL_TYPE_HARDWARE);
    vpl.filter("mfxImplDescription.VendorID",0x8086);
    vpl.filter("mfxImplDescription.AccelerationMode",MFX_ACCEL_MODE_VIA_D3D11);
    vpl.filter("DXGIAdapterIndex",index);
    const auto created=MFXCreateSession(vpl.loader,0,&vpl.session);
    std::printf("  intel.session_status=%d\n",created);if(created<0){require(!roundtrip,"Intel hardware session unavailable for roundtrip");return;}
    const auto bound=MFXVideoCORE_SetHandle(vpl.session,MFX_HANDLE_D3D11_DEVICE,device);
    std::printf("  intel.d3d11_bind_status=%d\n",bound);if(bound<0){require(!roundtrip,"Intel D3D11 binding unavailable for roundtrip");return;}
    for(bool encode:{true,false}){
        auto requested=hevc444(encode),actual=requested;
        const auto status=encode?MFXVideoENCODE_Query(vpl.session,&requested,&actual):MFXVideoDECODE_Query(vpl.session,&requested,&actual);
        const auto& f=actual.mfx.FrameInfo;
        const bool exact=status>=0&&status!=MFX_WRN_PARTIAL_ACCELERATION&&actual.mfx.CodecId==MFX_CODEC_HEVC&&
            actual.mfx.CodecProfile==MFX_PROFILE_HEVC_REXT&&f.FourCC==MFX_FOURCC_AYUV&&
            f.ChromaFormat==MFX_CHROMAFORMAT_YUV444&&f.BitDepthLuma==8&&f.BitDepthChroma==8&&
            actual.IOPattern==requested.IOPattern&&(!encode||actual.mfx.GopRefDist==1);
        std::printf("  intel.hevc444_%s.query_status=%d exact_format=%s profile=%u chroma=%u fourcc=0x%08x\n",
            encode?"encode":"decode",status,exact?"yes":"no",actual.mfx.CodecProfile,f.ChromaFormat,f.FourCC);
    }
    if(roundtrip)std::printf("  intel.roundtrip=PASS %s\n",intel_roundtrip(vpl.session).c_str());
    std::puts("  intel.backend_ready=no (desktop capture, async pipeline integration and display qualification pending)");
}
struct Library {HMODULE handle=nullptr;~Library(){if(handle)FreeLibrary(handle);}};
void amd(ID3D11Device* device){
    Library library{LoadLibraryExW(AMF_DLL_NAME,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)};
    require(library.handle!=nullptr,"AMD driver AMF runtime unavailable in System32");
    auto init=reinterpret_cast<AMFInit_Fn>(GetProcAddress(library.handle,AMF_INIT_FUNCTION_NAME));
    require(init!=nullptr,"AMD AMF entry point unavailable");
    amf::AMFFactory* factory=nullptr;require(init(AMF_FULL_VERSION,&factory)==AMF_OK&&factory,"AMD AMF factory initialization");
    amf::AMFContextPtr context;require(factory->CreateContext(&context)==AMF_OK,"AMD context creation");
    require(context->InitDX11(device)==AMF_OK,"AMD D3D11 adapter binding");
    amf::AMFComponentPtr encoder;
    const auto created=factory->CreateComponent(context,AMFVideoEncoder_HEVC,&encoder);
    std::printf("  amd.hevc.component_status=%d\n",created);
    if(created==AMF_OK){
        amf::AMFCapsPtr caps;require(encoder->GetCaps(&caps)==AMF_OK,"AMD encoder capabilities");
        amf_int64 profile=0;const auto profile_status=caps->GetProperty(AMF_VIDEO_ENCODER_HEVC_CAP_MAX_PROFILE,&profile);
        std::printf("  amd.hevc.acceleration=%d max_profile_status=%d max_profile=%lld\n",caps->GetAccelerationType(),profile_status,profile);
        amf::AMFIOCapsPtr input;
        if(caps->GetInputCaps(&input)==AMF_OK){
            for(amf_int32 i=0;i<input->GetNumOfFormats();++i){
                amf::AMF_SURFACE_FORMAT format{};amf_bool native=false;
                if(input->GetFormatAt(i,&format,&native)==AMF_OK)
                    std::printf("  amd.hevc.input_format=%d native=%s (input format is NOT bitstream chroma)\n",format,native?"yes":"no");
            }
        }
        encoder->Terminate();
    }
    encoder=nullptr;context->Terminate();
    std::puts("  amd.backend_ready=no (AMF HEVC exposes Main/Main10, not a qualified native 4:4:4 path; no 4:2:0 fallback)");
}
}
int main(int argc,char** argv){
    const bool roundtrip=argc==2&&std::strcmp(argv[1],"--intel-roundtrip")==0;
    if(argc>1&&!roundtrip){std::puts("Usage: HiveDeskMediaProbe [--intel-roundtrip]");return 1;}
    setvbuf(stdout,nullptr,_IONBF,0);
    unsigned tested=0,failed=0;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    std::puts("HiveDesk media capability probe " HIVEDESK_VERSION " - no capture, no codec fallback, no production backend changes");
    ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 1;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> adapter;if(FAILED(factory->EnumAdapters1(i,&adapter)))break;
        DXGI_ADAPTER_DESC1 d{};if(FAILED(adapter->GetDesc1(&d)))continue;
        std::printf("adapter=%u name=%s vendor=0x%04x device=0x%04x luid=%08x:%08x\n",i,utf8(d.Description).c_str(),d.VendorId,d.DeviceId,d.AdapterLuid.HighPart,d.AdapterLuid.LowPart);
        LARGE_INTEGER driver{};
        if(SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver)))
            std::printf("  driver=%u.%u.%u.%u\n",unsigned(HIWORD(driver.HighPart)),unsigned(LOWORD(driver.HighPart)),unsigned(HIWORD(driver.LowPart)),unsigned(LOWORD(driver.LowPart)));
        for(UINT n=0;;++n){ComPtr<IDXGIOutput> output;if(FAILED(adapter->EnumOutputs(n,&output)))break;DXGI_OUTPUT_DESC screen{};
            if(SUCCEEDED(output->GetDesc(&screen)))std::printf("  display=%s attached=%d size=%ldx%ld primary=%d\n",utf8(screen.DeviceName).c_str(),screen.AttachedToDesktop,screen.DesktopCoordinates.right-screen.DesktopCoordinates.left,screen.DesktopCoordinates.bottom-screen.DesktopCoordinates.top,screen.Monitor==MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY));}
        if(d.VendorId!=0x8086&&d.VendorId!=0x1002){std::puts("  existing NVIDIA path untouched; foreign probe skipped");continue;}
        try{
            ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
            require(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"D3D11 video device creation");
            ComPtr<ID3D10Multithread> multithread;
            require(SUCCEEDED(context.As(&multithread)),"D3D11 multithread protection unavailable");
            multithread->SetMultithreadProtected(TRUE);
            if(d.VendorId==0x8086){if(roundtrip)++tested;intel(i,device.Get(),roundtrip);}else amd(device.Get());
        }catch(const std::exception& error){std::printf("  probe_unavailable=%s\n",error.what());if(roundtrip&&d.VendorId==0x8086)++failed;}
    }
    if(roundtrip&&!tested&&!failed){std::puts("SKIP Intel roundtrip: no Intel hardware adapter");return 2;}
    return failed?1:0;
}
