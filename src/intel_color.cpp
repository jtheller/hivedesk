#include "intel_color.h"
#include <d3dcompiler.h>
#include <d3d10.h>
#include <wrl/client.h>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace hive {
namespace {
void checked(HRESULT result,const char* stage){if(FAILED(result))throw std::runtime_error(std::string(stage)+" HRESULT="+std::to_string(uint32_t(result)));}
using Microsoft::WRL::ComPtr;
}
struct IntelColor::Impl {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D10Multithread> multithread;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11Texture2D> packed,staging;
    ComPtr<ID3D11UnorderedAccessView> output;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> dimensions;
    uint32_t width=0,height=0;
};
IntelColor::IntelColor(ID3D11Texture2D* source,uint32_t padded_width,uint32_t padded_height):impl_(std::make_unique<Impl>()){
    if(!source)throw std::runtime_error("Intel color source missing");auto& d=*impl_;
    D3D11_TEXTURE2D_DESC original{};source->GetDesc(&original);
    if(original.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||!original.Width||!original.Height||
       padded_width<original.Width||padded_height<original.Height||padded_width>8192||padded_height>8192)
        throw std::runtime_error("Intel color requires primary BGRA pixels and bounded padded dimensions");
    source->GetDevice(&d.device);d.device->GetImmediateContext(&d.context);
    checked(d.context.As(&d.multithread),"Intel color context protection");d.multithread->SetMultithreadProtected(TRUE);
    checked(d.device->CreateShaderResourceView(source,nullptr,&d.source),"Intel color input view");
    d.width=padded_width;d.height=padded_height;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=d.width;desc.Height=d.height;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    checked(d.device->CreateTexture2D(&desc,nullptr,&d.packed),"Intel color packed texture");
    checked(d.device->CreateUnorderedAccessView(d.packed.Get(),nullptr,&d.output),"Intel color output view");
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    checked(d.device->CreateTexture2D(&desc,nullptr,&d.staging),"Intel color readback texture");
    const uint32_t dimensions[]={original.Width,original.Height,d.width,d.height};
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(dimensions);buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{dimensions,0,0};checked(d.device->CreateBuffer(&buffer,&initial,&d.dimensions),"Intel color dimensions");
    constexpr char shader[]=R"(
        Texture2D<float4> source:register(t0);RWTexture2D<unorm float4> packed:register(u0);
        cbuffer Size:register(b0){uint width;uint height;uint paddedWidth;uint paddedHeight;};
        [numthreads(16,16,1)] void main(uint3 id:SV_DispatchThreadID){
            if(id.x>=paddedWidth||id.y>=paddedHeight)return;
            float3 rgb=source.Load(int3(min(id.xy,uint2(width-1,height-1)),0)).rgb*255.0;
            float y=dot(rgb,float3(.2126,.7152,.0722));
            float u=(rgb.b-y)/1.8556+128.0,v=(rgb.r-y)/1.5748+128.0;
            packed[id.xy]=float4(floor(clamp(float3(v,u,y),0.0,255.0)+.5)/255.0,1.0);
        })";
    ComPtr<ID3DBlob> code,errors;
    checked(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors),"Intel full-chroma shader");
    checked(d.device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&d.shader),"Intel color shader creation");
}
IntelColor::~IntelColor()=default;
void IntelColor::convert(uint8_t* destination,uint32_t pitch,const std::function<bool()>& cancelled){
    auto& d=*impl_;if(!destination||pitch<d.width*4)throw std::runtime_error("Intel color destination pitch");
    if(cancelled&&cancelled())throw std::runtime_error("Intel color conversion cancelled");
    auto* input=d.source.Get();auto* output=d.output.Get();auto* dimensions=d.dimensions.Get();
    // Per-call protection alone cannot protect a bind/dispatch sequence from
    // the Intel runtime using the same immediate context on its worker.
    d.multithread->Enter();
    d.context->CSSetShader(d.shader.Get(),nullptr,0);d.context->CSSetShaderResources(0,1,&input);
    d.context->CSSetUnorderedAccessViews(0,1,&output,nullptr);d.context->CSSetConstantBuffers(0,1,&dimensions);
    d.context->Dispatch((d.width+15)/16,(d.height+15)/16,1);
    ID3D11ShaderResourceView* empty_input=nullptr;ID3D11UnorderedAccessView* empty_output=nullptr;
    d.context->CSSetShaderResources(0,1,&empty_input);d.context->CSSetUnorderedAccessViews(0,1,&empty_output,nullptr);
    d.context->CSSetShader(nullptr,nullptr,0);
    d.context->CopyResource(d.staging.Get(),d.packed.Get());d.context->Flush();
    d.multithread->Leave();
    D3D11_MAPPED_SUBRESOURCE mapped{};const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    for(;;){const auto result=d.context->Map(d.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(result!=DXGI_ERROR_WAS_STILL_DRAWING){checked(result,"Intel color readback");break;}
        if((cancelled&&cancelled())||std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("Intel color readback cancelled or timed out");
        SwitchToThread();
    }
    for(uint32_t y=0;y<d.height;++y)std::memcpy(destination+size_t(y)*pitch,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(d.width)*4);
    d.context->Unmap(d.staging.Get(),0);
}
}
