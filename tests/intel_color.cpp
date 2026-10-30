#include "../src/intel_color.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
int main(){
    try{
        using Microsoft::WRL::ComPtr;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)))return 1;
        constexpr unsigned width=18,height=10,padded_width=32,padded_height=16,pitch=160;
        std::vector<uint8_t> pixels(width*height*4),output(pitch*padded_height,0x5a);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto* p=&pixels[(y*width+x)*4];p[0]=uint8_t(x&1?255:0);p[1]=uint8_t(y*27);p[2]=uint8_t(x*15);p[3]=255;}
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels.data(),width*4,0};ComPtr<ID3D11Texture2D> texture;
        if(FAILED(device->CreateTexture2D(&desc,&initial,&texture)))return 1;
        hive::IntelColor color(texture.Get(),padded_width,padded_height);color.convert(output.data(),pitch,{});
        for(unsigned y=0;y<padded_height;++y){for(unsigned x=0;x<padded_width;++x){
            const auto* p=&pixels[(std::min(y,height-1)*width+std::min(x,width-1))*4];
            const float luma=.2126f*p[2]+.7152f*p[1]+.0722f*p[0];
            const std::array<float,4> expected{(p[2]-luma)/1.5748f+128,(p[0]-luma)/1.8556f+128,luma,255};
            for(unsigned c=0;c<4;++c)if(std::abs(int(output[y*pitch+x*4+c])-int(std::floor(std::clamp(expected[c],0.f,255.f)+.5f)))>1)return 1;
        }for(unsigned x=padded_width*4;x<pitch;++x)if(output[y*pitch+x]!=0x5a)return 1;}
        bool cancelled=false;try{color.convert(output.data(),pitch,[]{return true;});}catch(...){cancelled=true;}if(!cancelled)return 1;
        std::puts("PASS D3D11 full-range BT.709 AYUV byte order, single-pixel chroma, edge padding, row guards and cancellation");return 0;
    }catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}
}
