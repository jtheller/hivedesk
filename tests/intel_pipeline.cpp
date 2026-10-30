#include "../src/intel_encoder.h"
#include "../src/software_decoder.h"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
namespace {
std::array<uint8_t,3> rgb(unsigned x,unsigned y,unsigned frame){
    if((x+frame*9)%160<24)return {240,220,30};
    if(y<180)return x<320?std::array<uint8_t,3>{225,30,55}:std::array<uint8_t,3>{20,210,65};
    return x<320?std::array<uint8_t,3>{35,60,230}:std::array<uint8_t,3>{185,45,210};
}
}
// Generated texture only. Exercises the production Intel backend without
// displaying a test window, moving the mouse, or modifying the user's desktop.
void intel_pipeline_check(){
    using Microsoft::WRL::ComPtr;ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))throw std::runtime_error("Pipeline DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;const auto primary=MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
    for(UINT a=0;!adapter;++a){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(a,&candidate)==DXGI_ERROR_NOT_FOUND)break;
        for(UINT o=0;;++o){ComPtr<IDXGIOutput> output;if(candidate->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;DXGI_OUTPUT_DESC desc{};
            if(SUCCEEDED(output->GetDesc(&desc))&&desc.AttachedToDesktop&&desc.Monitor==primary){adapter=candidate;break;}}}
    DXGI_ADAPTER_DESC1 description{};if(!adapter||FAILED(adapter->GetDesc1(&description)))throw std::runtime_error("Pipeline primary adapter");
    if(description.VendorId!=0x8086){std::puts("SKIP generated Intel pipeline: primary adapter is not Intel");return;}
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)))throw std::runtime_error("Pipeline Intel device");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=640;desc.Height=360;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;if(FAILED(device->CreateTexture2D(&desc,nullptr,&texture)))throw std::runtime_error("Pipeline generated texture");
    hive::StreamSettings settings;settings.session=1;settings.receiver_millihz=60000;
    std::atomic<bool> failed=false;std::exception_ptr failure;std::mutex mutex;
    hive::IntelEncoder encoder(640,360,settings,adapter.Get(),texture.Get(),[&]{return failed.load();});
    uint64_t error_sum=0,samples=0;unsigned decoded=0,changes=0;hive::SoftwareDecoder decoder;
    std::thread consumer([&]{try{hive::IntelEncoder::Completion result;while(encoder.receive(result)){
        if(result.frame.sequence!=decoded||result.frame.keyframe!=(decoded==0||decoded==24))throw std::runtime_error("Intel pipeline sequence/IDR changed");
        decoder.decode(result.frame,[&](const hive::SoftwarePicture& picture){
            for(unsigned y=16;y<360;y+=32)for(unsigned x=16;x<640;x+=32){const auto color=rgb(x,y,decoded);
                const float luma=.2126f*color[0]+.7152f*color[1]+.0722f*color[2];
                const std::array<float,3> expected{luma,(color[2]-luma)/1.8556f+128,(color[0]-luma)/1.5748f+128};
                for(unsigned c=0;c<3;++c){error_sum+=uint64_t(std::abs(int(picture.planes[c][y*picture.strides[c]+x])-int(std::floor(expected[c]+.5f))));++samples;}
            }
        });++decoded;
    }}catch(...){std::lock_guard lock(mutex);failure=std::current_exception();failed=true;}});
    const auto began=GetTickCount64();
    const auto wait=[&](auto predicate){while(!predicate()){if(failed||GetTickCount64()-began>15000)throw std::runtime_error("Intel pipeline completion failed or timed out");Sleep(1);}};
    try{
        std::vector<uint8_t> pixels(640*360*4);
        for(unsigned frame=0;frame<48;++frame){
            if(frame==16||frame==32){wait([&]{return encoder.in_flight()==0;});if(encoder.dynamic_rate()&&encoder.reconfigure(frame==16?12000000:8000000,frame==16?50000:60000))++changes;}
            wait([&]{return encoder.ready();});
            for(unsigned y=0;y<360;++y)for(unsigned x=0;x<640;++x){const auto color=rgb(x,y,frame);auto* p=&pixels[(y*640+x)*4];p[0]=color[2];p[1]=color[1];p[2]=color[0];p[3]=255;}
            context->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),640*4,0);bool queued=false;
            const auto stamp=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if(!encoder.submit_texture(texture.Get(),frame,frame==0||frame==24,stamp,&queued,hive::CursorComposition::Unknown)||!queued)throw std::runtime_error("Intel pipeline lost submission");
        }
        encoder.finish();consumer.join();if(failure)std::rethrow_exception(failure);
    }catch(...){encoder.finish();if(consumer.joinable())consumer.join();if(failure)std::rethrow_exception(failure);throw;}
    const double mean=samples?double(error_sum)/samples:1000;
    if(decoded!=48||mean>8.0||encoder.in_flight()!=0||encoder.ready())throw std::runtime_error("Intel pipeline frame/color/drain bounds failed");
    std::printf("PASS generated Intel production pipeline: frames=%u mean_abs_YUV=%.3f rate_changes=%u time_ms=%llu; sequence, forced IDR, no extra IDR, two-slot ownership and finish verified\n",decoded,mean,changes,GetTickCount64()-began);
}
