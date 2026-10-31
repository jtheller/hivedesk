#include "compact_hud.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <chrono>
using namespace hive;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    DebugOverlayState state;state.now=1000;state.source_time=state.health_time=990;
    state.health={1,100000,60000,0};state.source.session=state.viewer.session=1;
    state.source.backend=EncoderBackend::Nvenc;state.source.encode_us=8600;state.source.encode_age_us=1000;
    state.viewer.presented_millihz=99000;state.viewer.decode_us=2400;state.viewer.service_sample_age_us=1000;
    state.viewer.receive_kbps=128000;state.viewer.receive_sample_age_us=1000;
    auto normal=compact_hud(state,true,false,L"Jateruy-VI");
    require(normal.cells.size()==5&&normal.cells[0].text==L"Jateruy-VI"&&normal.cells[1].text==L"99 / 100 fps"&&normal.cells[4].text==L"128 Mbps","name/case/actual versus negotiated FPS");
    require(normal.cells[2].text==L"NVENC 8.6ms"&&normal.cells[2].icon==HudIcon::HardwareEncode&&normal.cells[3].icon==HudIcon::HardwareDecode,"hardware backend identity");
    require(hud_color(normal.cells[0].tone)==hud_color(normal.cells[1].tone)&&hud_color(HudTone::Normal)==RGB(236,236,236),"name/numeric neutral white mismatch");
    state.source.backend=EncoderBackend::Intel;state.viewer.software_decode=true;
    const auto intel=compact_hud(state,true,false,L"JT-WORKS-PRINT-SHOP");
    require(intel.cells[2].text==L"QSV 8.6ms"&&intel.cells[3].text==L"FFmpeg 2.4ms"&&intel.cells[3].icon==HudIcon::SoftwareDecode,"Intel or FFmpeg mislabeled");
    state.source.backend=EncoderBackend::Unknown;require(compact_hud(state,true,false).cells[2].icon==HudIcon::Unknown,"legacy host guessed as NVIDIA");
    state.source.backend=EncoderBackend::Software;require(compact_hud(state,true,false).cells[2].icon==HudIcon::SoftwareEncode,"software encoder icon");
    state.source.backend=EncoderBackend::Nvenc;state.health.pressure=PressureNetwork;
    auto warning=compact_hud(state,true,true,L"Jateruy-VI");
    require(warning.cells[0].text==L"Jateruy-VI"&&warning.cells[4].tone==HudTone::Amber,"inline warning lost identity");
    const auto alone=compact_hud(state,false,true);require(alone.cells.size()==1&&alone.cells[0].icon==HudIcon::Warning,"stats-off warning disappeared");
    state.now=4000;state.viewer.service_sample_age_us=state.viewer.receive_sample_age_us=600000;
    auto stale=compact_hud(state,true,false);
    require(stale.cells[1].text==L"-- / 100 fps"&&stale.cells[2].text==L"NVENC --ms"&&stale.cells[4].text==L"-- Mbps"&&stale.cells[2].tone==HudTone::Muted,"stale readings masquerade as live");
    const std::wstring unicode=L"Jateruy-VI & 工作室・東京・München";
    const auto connecting=connecting_hud(false,unicode);require(connecting.cells[1].text==unicode,"connection name altered");
    const auto retry=connecting_hud(true,unicode),error=unavailable_hud(L"Remote stats unavailable · Try again");
    auto unicode_hud=normal;unicode_hud.cells[0].text=unicode;
    CompactHudSurface surface;
    for(int dpi:{96,120,144,192}){
        const int height=MulDiv(28,dpi,96);
        for(const auto& model:{normal,intel,warning,stale,alone,connecting,retry,error,unicode_hud}){
            const auto layout=layout_hud(model,dpi);const int width=layout.width;
            require(surface.render(model,width,height,dpi),"HUD render/DPI");const auto* pixels=surface.pixels();
            require(pixels[0]==0,"rounded corner opaque");bool text=false;
            for(int i=0;i<width*height;++i){const auto p=pixels[i],a=p>>24;require((p&255)<=a&&((p>>8)&255)<=a&&((p>>16)&255)<=a,"nonpremultiplied pixels");text|=a==255;}
            require(text,"text lacks opaque cores");
            if(model.divider){const int gap=MulDiv(20,dpi,96),icon_gap=MulDiv(16,dpi,96)+MulDiv(6,dpi,96);
                for(size_t i=1;i<layout.cells.size();++i)require(layout.cells[i].x-icon_gap-(layout.cells[i-1].x+layout.cells[i-1].width)==gap,"group gaps differ");}
        }
        for(int value=int(HudIcon::Frames);value<=int(HudIcon::Unavailable);++value){const auto icon=hud_icon(HudIcon(value),dpi);require(std::any_of(icon->alpha.begin(),icon->alpha.end(),[](auto a){return a>0;}),"empty icon");}
        auto limited=layout_hud(unicode_hud,dpi,MulDiv(300,dpi,96));
        require(limited.width<=MulDiv(300,dpi,96)&&limited.cells[0].text==unicode,"narrow layout mutated original name");
        require(surface.render(unicode_hud,limited.width,height,dpi),"narrow viewer clipping");
    }
    const auto began=std::chrono::steady_clock::now();const auto before=layout_hud(normal,96).width;
    for(int i=0;i<100;++i){normal.cells[2].text=L"NVENC "+std::to_wstring(i%10)+L".6ms";auto layout=layout_hud(normal,96);
        require(layout.width==before,"numeric sampling made strip jitter");require(surface.render(normal,layout.width,28,96),"changing render");}
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    constexpr int width=1500,height=450;std::vector<uint32_t> preview(width*height);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)preview[size_t(y)*width+x]=x<750?0xffe9edf3:0xff20364a;
    std::vector<CompactHud> models{normal,intel,warning,alone,connecting,stale,unicode_hud};
    for(int side=0;side<2;++side)for(size_t row=0;row<models.size();++row){auto layout=layout_hud(models[row],96,710);
        require(surface.render(models[row],layout.width,28,96),"preview render");surface.composite(preview.data(),width,height,side*750+20,20+int(row)*59);}
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-height;info.biPlanes=1;info.biBitCount=32;
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+DWORD(preview.size()*4);
    std::ofstream out("compact-hud-native.bmp",std::ios::binary);out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));out.write(reinterpret_cast<const char*>(preview.data()),std::streamsize(preview.size()*4));require(bool(out),"preview artifact");
    std::printf("PASS compact HUD: Unicode/case, backend identity, current/target FPS, uniform gaps, stable numeric width, warnings, freshness, alpha, 96-192 DPI, narrow viewport; 100 draws %.1f ms\n",elapsed);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL compact HUD: %s\n",e.what());return 1;}}
