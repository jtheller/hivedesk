#include "compact_hud.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <chrono>
using namespace hive;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    DebugOverlayState state;state.now=1000;state.source_time=state.health_time=990;
    state.health={1,100000,100000,0};state.source.session=state.viewer.session=1;
    state.source.encode_us=3200;state.source.encode_age_us=1000;
    state.viewer.presented_millihz=99000;state.viewer.decode_us=1100;state.viewer.service_sample_age_us=1000;
    state.viewer.receive_kbps=380000;state.viewer.receive_sample_age_us=1000;
    auto normal=compact_hud(state,true,false);
    require(normal.width==352&&normal.cells.size()==5&&normal.cells[1].text==L"99 fps"&&normal.cells[4].text==L"380 Mbps","compact metric units/layout");
    state.health.pressure=PressureNetwork;auto warning=compact_hud(state,true,true);
    require(warning.cells.size()==5&&warning.width==normal.width&&warning.cells[0].text==L"Network","warning stacked or resized stats");
    const auto alone=compact_hud(state,false,true);
    require(alone.width==184&&alone.cells.size()==1&&alone.tone==HudTone::Amber,"stats-off warning disappeared");
    state.now=4000;state.viewer.service_sample_age_us=state.viewer.receive_sample_age_us=600000;
    auto stale=compact_hud(state,true,false);
    require(stale.cells[1].text==L"-- fps"&&stale.cells[2].text==L"E --ms"&&stale.cells[4].text==L"-- Mbps","stale metrics masquerade as live");
    require(stale.cells[2].tone==HudTone::Muted,"stale encoder remained alarm colored");
    const auto connecting=connecting_hud(false,L"PRINT & \x4e2d\x6587");
    require(connecting.tone==HudTone::Blue&&connecting.cells[1].text==L"PRINT & \x4e2d\x6587","connecting lost Unicode or treated wait as error");
    CompactHudSurface surface;
    for(int dpi:{96,120,144,192}){
        const int width=MulDiv(352,dpi,96),height=MulDiv(28,dpi,96);
        for(const auto& model:{normal,warning,stale,connecting}){
            require(surface.render(model,width,height,dpi),"HUD render/DPI");const auto* pixels=surface.pixels();
            require(pixels[0]==0,"HUD rounded corner opaque");
            require((pixels[size_t(height-3)*width+width/2]>>24)==150,"HUD body is not translucent");
            bool text=false;for(int i=0;i<width*height;++i){const auto p=pixels[i],a=p>>24;
                require((p&255)<=a&&((p>>8)&255)<=a&&((p>>16)&255)<=a,"HUD output is not premultiplied");text|=a==255;}
            require(text,"text became as translucent as background");
        }
        require(surface.render(connecting,MulDiv(100,dpi,96),height,dpi),"narrow viewer clipping failed");
    }
    const auto began=std::chrono::steady_clock::now();
    for(int i=0;i<100;++i){normal.cells[2].text=L"E "+std::to_wstring(i)+L"ms";require(surface.render(normal,352,28,96),"changing numeric render");}
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    // Native rendering evidence on both light and dark backgrounds, no desktop capture.
    constexpr int width=800,height=330;std::vector<uint32_t> preview(width*height);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)preview[size_t(y)*width+x]=x<400?0xffeef2f8:0xff152333;
    state.now=1000;state.viewer.service_sample_age_us=state.viewer.receive_sample_age_us=1000;state.health.pressure=0;
    normal=compact_hud(state,true,false);
    std::vector<CompactHud> models{normal,warning,alone,connecting,stale};
    for(int side=0;side<2;++side)for(size_t row=0;row<models.size();++row){
        require(surface.render(models[row],models[row].width,28,96),"preview render");surface.composite(preview.data(),width,height,side*400+24,24+int(row)*58);
    }
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-height;info.biPlanes=1;info.biBitCount=32;
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+DWORD(preview.size()*4);
    std::ofstream out("compact-hud-native.bmp",std::ios::binary);out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));out.write(reinterpret_cast<const char*>(preview.data()),std::streamsize(preview.size()*4));
    require(bool(out),"native preview artifact");
    std::printf("PASS compact HUD: inline warning, off-state warning, freshness, Unicode, alpha, 96-192 DPI, narrow clipping; 100 changed draws %.1f ms\n",elapsed);
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL compact HUD: %s\n",e.what());return 1;}}
