#include "hivedesk/shell.h"
#include <fstream>
#include <cstdio>
#include <stdexcept>
namespace {
void require(bool value,const char* error){if(!value)throw std::runtime_error(error);}
void pump(DWORD ms){const auto end=GetTickCount64()+ms;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5);}while(GetTickCount64()<end);}
void capture(HWND window){
    RECT r{};GetClientRect(window,&r);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=r.right;info.bmiHeader.biHeight=-r.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);void* data=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&data,nullptr,0);auto old=SelectObject(dc,bitmap);PrintWindow(window,dc,2);
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);header.bfSize=header.bfOffBits+r.right*r.bottom*4;
    std::ofstream file("build/debug-overlay.bmp",std::ios::binary);file.write(reinterpret_cast<char*>(&header),sizeof(header));file.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(info.bmiHeader));file.write(static_cast<char*>(data),r.right*r.bottom*4);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
}
int main(){SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);try{
    hive::Shell shell;hive::DesktopPreferences preferences;preferences.startup=false;preferences.tray_icon=false;preferences.debug_overlay=true;std::string error;
    hive::ShellCallbacks callbacks;callbacks.select_peer=[&](const std::string& id){shell.set_remote_size(2560,1440);shell.show_remote(id,L"Synthetic diagnostics");};
    require(shell.create(GetModuleHandleW(nullptr),{L"Diagnostic preview","",{},preferences},callbacks,error),error.c_str());
    shell.update_peers({{"debug-test","Synthetic diagnostics","127.0.0.1",0,true,7}});
    HWND owner=FindWindowW(L"HiveDesk.Owner",nullptr),panel=FindWindowW(L"HiveDesk.Switcher",nullptr);require(owner&&panel,"Missing shell windows");
    SendMessageW(owner,WM_HOTKEY,81,0);pump(250);
    const auto px=[&](int value){return MulDiv(value,GetDpiForWindow(panel),96);};const int tile_height=(px(200)-2*px(12))*9/16;
    SendMessageW(panel,WM_LBUTTONDOWN,0,MAKELPARAM(px(100),px(12)+tile_height+px(16)+tile_height/2));
    SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(px(100),px(12)+tile_height+px(16)+tile_height/2));
    shell.update_health("debug-test",{1,144000,60000,hive::PressureDecode});
    hive::SourceDiagnostics source{1,59940,3200,2400,4000,38000,1200,1200,2000,12000,true};
    source.rate_details=source.automatic_cbr=source.quality_limited=true;
    source.target_bitrate_bps=92160000;source.actual_bitrate_bps=75200000;source.average_qp=21;source.rate_reason=3;source.source_discarded=7;
    shell.update_source_diagnostics("debug-test",source);
    hive::ViewerDiagnostics view;view.session=1;view.received_millihz=59940;view.decoded_millihz=59940;view.presented_millihz=58500;view.receive_kbps=123400;view.decode_us=2500;view.render_us=1500;view.present_us=300;view.service_p90_us=8900;view.skipped=12;shell.update_viewer_diagnostics("debug-test",view);pump(500);
    HWND badge=FindWindowW(L"HiveDesk.Health",nullptr);require(badge!=nullptr,"Missing debug overlay");
    require(GetWindowLongPtrW(badge,GWL_EXSTYLE)&WS_EX_NOACTIVATE,"Debug overlay can activate");require(SendMessageW(badge,WM_NCHITTEST,0,0)==HTTRANSPARENT,"Debug overlay intercepts input");
    require(IsWindowVisible(badge),"Windows denied foreground or debug overlay did not show");RECT r{};GetClientRect(badge,&r);const int dpi=int(GetDpiForWindow(badge));require(r.right==MulDiv(460,dpi,96)&&r.bottom==MulDiv(474,dpi,96),"Incorrect debug dimensions");capture(badge);
    shell.show_local();pump(50);require(!IsWindowVisible(badge),"Debug overlay remained on local desktop");
    std::puts("PASS synthetic debug overlay: visible, fixed bounds, nonactivating, pointer-transparent, hidden on local return; rendered BMP saved.");return 0;
}catch(const std::exception& e){std::printf("FAIL debug overlay: %s\n",e.what());return 1;}}
