#include "hivedesk/diagnostic_window.h"
#include "debug_model.h"
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include <chrono>
void require(bool good,const char* message){if(!good)throw std::runtime_error(message);}
int main(){try{
    hive::DiagnosticWindow<2> window;window.sample(0,{0,0});
    for(uint64_t i=1;i<=60;++i){const auto value=window.sample(i*10000,{i,i*1000});require(value.rate(0,1000000000)==100000,"steady FPS changed with HUD ticks");require(value.rate(1,8000000)==800000,"bitrate units wrong");}
    for(uint64_t i=61;i<=90;++i)window.sample(i*10000,{60,60000});
    require(window.sample(910000,{60,60000}).rate(0,1000000000)==0,"idle rates retained old frames");
    require(window.sample(920000,{0,0}).us==0,"session counter reset not isolated");
    hive::DiagnosticWindow<1> fast;fast.sample(0,{0});
    for(uint64_t i=1;i<=1000;++i)fast.sample(i*1000,{i});
    require(fast.sample(1001000,{1001}).rate(0,1000000000)==1000000,"fast sampling lost history");
    for(uint64_t i=1002;i<=1300;++i)fast.sample(i*1000,{1001});
    require(fast.sample(1301000,{1001}).rate(0,1000000000)==0,"fast history retained expired samples");
    using T=hive::DiagnosticTone;
    require(hive::diagnostic_budget(144000)==6944,"refresh-relative budget wrong");
    require(hive::diagnostic_threshold(9,10,20)==T::Normal&&hive::diagnostic_threshold(10,10,20)==T::Warning&&hive::diagnostic_threshold(20,10,20)==T::Critical,"severity boundaries wrong");
    require(hive::diagnostic_threshold(UINT32_MAX,10,20,false)==T::Muted,"stale value shown critical");
    hive::DiagnosticPulse pulse;require(!pulse.update(0,20)&&pulse.update(16,21)&&pulse.update(400,21)&&!pulse.update(600,21),"counter pulse did not expire");
    require(!pulse.update(700,0),"reset counter flashed");
    require(pulse.update(710,1)&&!pulse.update(720,0),"counter reset retained old warning");
    hive::DebugSurface surface;
    HFONT font=CreateFontW(-13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Consolas");
    hive::DebugOverlayState state;state.width=3840;state.height=2160;state.now=1000;state.source_time=state.health_time=990;
    state.health={1,144000,120000,3};state.source.session=1;state.source.encode_us=9000;state.source.encode_age_us=2000;state.source.rate_details=state.source.encode_details=true;
    state.source.automatic_cbr=true;state.source.target_bitrate_bps=420000000;state.source.actual_bitrate_bps=380000000;state.source.average_qp=25;
    state.viewer.session=1;state.viewer.decode_us=13000;state.viewer.render_us=5500;state.viewer.received_millihz=state.viewer.decoded_millihz=state.viewer.presented_millihz=120000;
    state.viewer.service_sample_age_us=2000;state.viewer.queue_us=18000;state.viewer.queue_depth=3;state.input.ready=true;state.input.credit_limit=64;state.input.inflight=64;state.input.samples=50;state.input.sample_age_us=1000;state.input.ack_roundtrip_p99_us=23000;
    std::array<hive::DiagnosticPulse,6> pulses{};auto rows=hive::build_debug_rows(state,pulses);
    require(rows[1][0].text==L"Viewer (this PC): Unknown"&&rows[2][0].text==L"Host (remote): Unknown"&&rows[2][0].tone==T::Muted,"Unknown peer version was invented");
    state.viewer_version=L"0.1.0-alpha.36";state.host_version=L"0.1.0-alpha.35";rows=hive::build_debug_rows(state,pulses);
    require(rows[1][0].text.find(state.viewer_version)!=std::wstring::npos&&rows[2][0].text.find(state.host_version)!=std::wstring::npos&&rows[2][0].tone==T::Warning,"Endpoint versions or mismatch warning missing");
    state.host_version=state.viewer_version;rows=hive::build_debug_rows(state,pulses);require(rows[2][0].tone==T::Normal,"Matching endpoint versions flagged");
    state.host_version=std::wstring(32,L'9');rows=hive::build_debug_rows(state,pulses);
    require(rows[0][0].text.find(L"NVDEC")!=std::wstring::npos,"Hardware decode label missing");
    state.viewer.software_decode=true;rows=hive::build_debug_rows(state,pulses);
    require(rows[0][0].text.find(L"CPU")!=std::wstring::npos&&rows[0][0].text.find(L"NVDEC")==std::wstring::npos,"Software decode mislabeled as NVDEC");
    require(rows.size()*18+24<=474,"HUD rows exceed height");
    HDC measuring=CreateCompatibleDC(nullptr);auto previous=SelectObject(measuring,font);
    for(const auto& row:rows){int width=0;for(const auto& span:row){SIZE extent{};GetTextExtentPoint32W(measuring,span.text.c_str(),int(span.text.size()),&extent);width+=extent.cx;}require(width<=436,"HUD text clipped at 96dpi");}
    SelectObject(measuring,previous);DeleteDC(measuring);
    require(surface.render(460,474,96,font,rows),"offscreen rendering failed");
    auto pixels=surface.pixels();require(pixels[0]==0,"rounded corner is opaque");require((pixels[471*460+200]>>24)==158,"body opacity wrong");
    bool opaque_text=false;for(int i=0;i<460*474;++i){const auto p=pixels[i],a=p>>24;require((p&255)<=a&&((p>>8)&255)<=a&&((p>>16)&255)<=a,"non-premultiplied output");opaque_text|=a==255;}
    require(opaque_text,"text inherited body translucency");
    HWND window_handle=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"STATIC",L"Hidden debug surface check",WS_POPUP,0,0,460,474,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(window_handle&&surface.present(window_handle),"UpdateLayeredWindow failed");require(!IsWindowVisible(window_handle),"headless probe became visible");DestroyWindow(window_handle);
    const auto began=std::chrono::steady_clock::now();for(int i=0;i<60;++i){state.source.encode_us=1000+i*300;state.now=1000+i*16;state.source_time=state.now-10;rows=hive::build_debug_rows(state,pulses);require(surface.render(460,474,96,font,rows),"repeat draw failed");}
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    state.host_version=L"0.1.0-alpha.35";rows=hive::build_debug_rows(state,pulses);require(surface.render(460,474,96,font,rows),"Version preview rendering failed");
    // A BMP artifact for visual inspection; no live desktop capture or hooks.
    BITMAPFILEHEADER header{};BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=460;info.biHeight=-474;info.biPlanes=1;info.biBitCount=32;
    header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);header.bfSize=header.bfOffBits+460*474*4;
    std::ofstream file("debug-surface.bmp",std::ios::binary);file.write(reinterpret_cast<char*>(&header),sizeof(header));file.write(reinterpret_cast<char*>(&info),sizeof(info));file.write(reinterpret_cast<const char*>(surface.pixels()),460*474*4);
    for(const int dpi:{144,192}){
        HFONT scaled=CreateFontW(-MulDiv(13,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Consolas");
        HDC measuring=CreateCompatibleDC(nullptr);auto old=SelectObject(measuring,scaled);
        for(const auto& row:rows){int width=0;for(const auto& span:row)for(auto c:span.text){SIZE extent{};GetTextExtentPoint32W(measuring,&c,1,&extent);width+=extent.cx;}require(width<=MulDiv(436,dpi,96),"scaled HUD text clipped");}
        SelectObject(measuring,old);DeleteDC(measuring);
        require(surface.render(MulDiv(460,dpi,96),MulDiv(474,dpi,96),dpi,scaled,rows),"scaled glyph cache failed");
        DeleteObject(scaled);
    }
    DeleteObject(font);std::printf("PASS rolling rates, freshness, severity, pulses, premultiplied translucent surface at 100/150/200 percent DPI and hidden layered window; %.3f ms/draw at 96dpi\n",elapsed/60);return 0;
}catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}}
