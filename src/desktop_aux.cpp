#include "hivedesk/desktop_aux.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace hive { namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr, const char* what) { if(FAILED(hr)) throw std::runtime_error(what); }
struct Apartment {
    HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Apartment(){if(FAILED(result)&&result!=RPC_E_CHANGED_MODE)throw std::runtime_error("Initialize snapshot COM");}
    ~Apartment(){if(SUCCEEDED(result))CoUninitialize();}
};
struct Bitmap {
    HBITMAP value=nullptr;
    ~Bitmap(){if(value)DeleteObject(value);}
};
struct DC {
    HDC value=CreateCompatibleDC(nullptr);
    ~DC(){if(value)DeleteDC(value);}
};
struct Screen {
    HDC value=GetDC(nullptr);
    ~Screen(){if(value)ReleaseDC(nullptr,value);}
};
struct Selection {
    HDC dc; HGDIOBJ old;
    Selection(HDC dc,HGDIOBJ value):dc(dc),old(SelectObject(dc,value)){}
    ~Selection(){SelectObject(dc,old);}
};
struct MonoInfo { BITMAPINFOHEADER header{}; RGBQUAD palette[2]{{0,0,0,0},{255,255,255,0}}; };
BITMAPINFO color_info(uint32_t width,uint32_t height) {
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=LONG(width);info.bmiHeader.biHeight=-LONG(height);
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;return info;
}
MonoInfo mono_info(uint32_t width,uint32_t height) {
    MonoInfo info{};info.header=color_info(width,height).bmiHeader;info.header.biBitCount=1;info.header.biClrUsed=2;return info;
}
ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> result;
    check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&result)),"Create snapshot image factory");return result;
}
std::vector<uint8_t> encode_jpeg(const uint8_t* bgra) {
    Apartment apartment;auto imaging=factory();ComPtr<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr,TRUE,&stream),"Create snapshot stream");
    ComPtr<IWICBitmapEncoder> encoder;check(imaging->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder),"Create JPEG encoder");
    check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Initialize JPEG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> options;
    check(encoder->CreateNewFrame(&frame,&options),"Create JPEG frame");
    PROPBAG2 property{};property.pstrName=const_cast<wchar_t*>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=.78f;
    check(options->Write(1,&property,&quality),"Set snapshot quality");check(frame->Initialize(options.Get()),"Initialize JPEG frame");
    check(frame->SetSize(SnapshotWidth,SnapshotHeight),"Set snapshot dimensions");
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;check(frame->SetPixelFormat(&format),"Set JPEG pixel format");
    if(format!=GUID_WICPixelFormat24bppBGR)throw std::runtime_error("Unexpected JPEG pixel format");
    std::vector<uint8_t> bgr(size_t(SnapshotWidth)*SnapshotHeight*3);
    for(size_t i=0;i<bgr.size()/3;++i)std::memcpy(bgr.data()+i*3,bgra+i*4,3);
    check(frame->WritePixels(SnapshotHeight,SnapshotWidth*3,UINT(bgr.size()),bgr.data()),"Encode snapshot pixels");
    check(frame->Commit(),"Commit JPEG frame");check(encoder->Commit(),"Commit JPEG stream");
    STATSTG stat{};check(stream->Stat(&stat,STATFLAG_NONAME),"Read JPEG size");
    if(!stat.cbSize.QuadPart||stat.cbSize.QuadPart>MaxSnapshotBytes)throw std::runtime_error("Snapshot exceeds size limit");
    check(stream->Seek({},STREAM_SEEK_SET,nullptr),"Rewind JPEG stream");
    std::vector<uint8_t> bytes(size_t(stat.cbSize.QuadPart));ULONG read=0;
    check(stream->Read(bytes.data(),ULONG(bytes.size()),&read),"Read JPEG bytes");
    if(read!=bytes.size())throw std::runtime_error("Incomplete JPEG output");return bytes;
}
}
Snapshot capture_snapshot(uint64_t request) {
    Screen screen;DC dc;Bitmap bitmap;void* pixels=nullptr;auto info=color_info(SnapshotWidth,SnapshotHeight);
    bitmap.value=CreateDIBSection(dc.value,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!screen.value||!dc.value||!bitmap.value)throw std::runtime_error("Allocate snapshot bitmap");
    Selection selected(dc.value,bitmap.value);
    const int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
    if(width<=0||height<=0)throw std::runtime_error("Primary display is unavailable");
    std::memset(pixels,0,size_t(SnapshotWidth)*SnapshotHeight*4);
    const double scale=std::min(double(SnapshotWidth)/width,double(SnapshotHeight)/height);
    const int w=std::max(1,int(width*scale)),h=std::max(1,int(height*scale));
    SetStretchBltMode(dc.value,HALFTONE);SetBrushOrgEx(dc.value,0,0,nullptr);
    if(!StretchBlt(dc.value,(int(SnapshotWidth)-w)/2,(int(SnapshotHeight)-h)/2,w,h,screen.value,0,0,width,height,SRCCOPY|CAPTUREBLT))
        throw std::runtime_error("Capture primary snapshot");
    GdiFlush();return {request,encode_jpeg(static_cast<const uint8_t*>(pixels))};
}
Thumbnail decode_snapshot(const Snapshot& snapshot) {
    if(snapshot.jpeg.empty()||snapshot.jpeg.size()>MaxSnapshotBytes)throw std::runtime_error("Invalid snapshot size");
    Apartment apartment;auto imaging=factory();ComPtr<IWICStream> stream;
    check(imaging->CreateStream(&stream),"Create JPEG input stream");
    check(stream->InitializeFromMemory(const_cast<BYTE*>(snapshot.jpeg.data()),DWORD(snapshot.jpeg.size())),"Read snapshot input");
    ComPtr<IWICBitmapDecoder> decoder;
    check(imaging->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder),"Decode snapshot JPEG");
    GUID format{};check(decoder->GetContainerFormat(&format),"Read snapshot format");
    if(format!=GUID_ContainerFormatJpeg)throw std::runtime_error("Snapshot must be JPEG");
    ComPtr<IWICBitmapFrameDecode> frame;check(decoder->GetFrame(0,&frame),"Read snapshot frame");
    UINT width=0,height=0;check(frame->GetSize(&width,&height),"Read snapshot dimensions");
    if(width!=SnapshotWidth||height!=SnapshotHeight)throw std::runtime_error("Unexpected snapshot dimensions");
    ComPtr<IWICFormatConverter> converter;check(imaging->CreateFormatConverter(&converter),"Create snapshot converter");
    check(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Convert snapshot pixels");
    Thumbnail result;result.bgra.resize(size_t(width)*height*4);
    check(converter->CopyPixels(nullptr,width*4,UINT(result.bgra.size()),result.bgra.data()),"Read snapshot pixels");return result;
}
bool read_cursor_shape(HCURSOR cursor,CursorState& state) {
    ICONINFO icon{};if(!cursor||!GetIconInfo(cursor,&icon))return false;
    Bitmap color{icon.hbmColor},mask{icon.hbmMask};BITMAP desc{};
    if(!GetObjectW(mask.value,sizeof(desc),&desc)||desc.bmWidth<=0||desc.bmHeight<=0)return false;
    CursorState result=state;result.width=uint32_t(desc.bmWidth);result.height=uint32_t(desc.bmHeight)/(color.value?1:2);
    result.hotspot_x=icon.xHotspot;result.hotspot_y=icon.yHotspot;
    if(result.width>256||!result.height||result.height>256||result.hotspot_x>=result.width||result.hotspot_y>=result.height)return false;
    DC dc;if(!dc.value)return false;
    auto mono=mono_info(result.width,uint32_t(desc.bmHeight));
    result.mask.resize(size_t((result.width+31)/32)*4*desc.bmHeight);
    if(GetDIBits(dc.value,mask.value,0,UINT(desc.bmHeight),result.mask.data(),reinterpret_cast<BITMAPINFO*>(&mono),DIB_RGB_COLORS)!=desc.bmHeight)return false;
    result.color.clear();
    if(color.value){
        auto info=color_info(result.width,result.height);result.color.resize(size_t(result.width)*result.height*4);
        if(GetDIBits(dc.value,color.value,0,result.height,result.color.data(),&info,DIB_RGB_COLORS)!=int(result.height))return false;
    }
    if(!valid_cursor(result))return false;state=std::move(result);return true;
}
HCURSOR make_cursor(const CursorState& state) {
    if(!valid_cursor(state)||!state.width)return nullptr;
    Bitmap color,mask;void* pixels=nullptr;auto mono=mono_info(state.width,state.height*(state.color.empty()?2:1));
    mask.value=CreateDIBSection(nullptr,reinterpret_cast<BITMAPINFO*>(&mono),DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!mask.value)return nullptr;std::memcpy(pixels,state.mask.data(),state.mask.size());
    if(!state.color.empty()){
        auto info=color_info(state.width,state.height);color.value=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!color.value)return nullptr;std::memcpy(pixels,state.color.data(),state.color.size());
    }
    ICONINFO icon{};icon.xHotspot=state.hotspot_x;icon.yHotspot=state.hotspot_y;icon.hbmColor=color.value;icon.hbmMask=mask.value;
    return static_cast<HCURSOR>(CreateIconIndirect(&icon));
}
bool desktop_aux_self_test(std::string& report) {
    try {
        std::vector<uint8_t> source(size_t(SnapshotWidth)*SnapshotHeight*4);
        for(size_t i=0;i<source.size()/4;++i){source[i*4]=30;source[i*4+1]=120;source[i*4+2]=220;source[i*4+3]=255;}
        auto decoded=decode_snapshot({7,encode_jpeg(source.data())});
        for(size_t i=0;i<source.size();++i)if(std::abs(int(source[i])-int(decoded.bgra[i]))>3)throw std::runtime_error("Snapshot JPEG color mismatch");
        bool rejected=false;try{decode_snapshot({1,{0,1,2,3}});}catch(...){rejected=true;}
        if(!rejected)throw std::runtime_error("Malformed snapshot accepted");
        // Compare real native cursor rendering, including monochrome invert pixels.
        for(auto name:{IDC_ARROW,IDC_IBEAM,IDC_SIZEWE,IDC_HAND}){
            HCURSOR original=LoadCursorW(nullptr,name);CursorState shape;
            if(!read_cursor_shape(original,shape))throw std::runtime_error("Read system cursor shape");
            HCURSOR copy=make_cursor(shape);if(!copy)throw std::runtime_error("Reconstruct system cursor");
            bool same=true;
            for(uint8_t background:{uint8_t(0),uint8_t(180)}){
                DC left,right;Bitmap a,b;void *pa=nullptr,*pb=nullptr;auto info=color_info(shape.width,shape.height);
                a.value=CreateDIBSection(left.value,&info,DIB_RGB_COLORS,&pa,nullptr,0);b.value=CreateDIBSection(right.value,&info,DIB_RGB_COLORS,&pb,nullptr,0);
                if(!a.value||!b.value){same=false;break;}
                Selection sa(left.value,a.value),sb(right.value,b.value);size_t bytes=size_t(shape.width)*shape.height*4;
                std::memset(pa,background,bytes);std::memset(pb,background,bytes);
                same=DrawIconEx(left.value,0,0,original,shape.width,shape.height,0,nullptr,DI_NORMAL)&&
                    DrawIconEx(right.value,0,0,copy,shape.width,shape.height,0,nullptr,DI_NORMAL)&&same;
                GdiFlush();same=std::memcmp(pa,pb,bytes)==0&&same;
            }
            DestroyCursor(copy);if(!same)throw std::runtime_error("Native cursor pixels changed on roundtrip");
            shape.hotspot_x=shape.width;if(make_cursor(shape))throw std::runtime_error("Invalid cursor hotspot accepted");
        }
        report="PASS: bounded JPEG snapshot decode/color roundtrip; native arrow, I-beam, resize and hand cursor pixel/hotspot roundtrip on light/dark backgrounds; malformed image and cursor rejection.";return true;
    }catch(const std::exception& error){report=std::string("FAIL: ")+error.what();return false;}
}
bool snapshot_capture_self_test(std::string& report) {
    HWND window=nullptr;
    try {
        WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"HiveDeskSnapshotProbe";
        cls.lpfnWndProc=[](HWND hwnd,UINT msg,WPARAM w,LPARAM l)->LRESULT {
            if(msg!=WM_PAINT)return DefWindowProcW(hwnd,msg,w,l);
            PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);RECT rect{};GetClientRect(hwnd,&rect);
            HBRUSH brush=CreateSolidBrush(RGB(210,80,30));FillRect(dc,&rect,brush);DeleteObject(brush);EndPaint(hwnd,&paint);return 0;
        };
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Create snapshot probe class");
        window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"HiveDesk snapshot test",WS_POPUP,64,64,320,200,nullptr,nullptr,cls.hInstance,nullptr);
        if(!window)throw std::runtime_error("Create snapshot probe window");
        ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);DwmFlush();
        auto snapshot=capture_snapshot(23);auto image=decode_snapshot(snapshot);
        const int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
        const double scale=std::min(double(SnapshotWidth)/width,double(SnapshotHeight)/height);
        const int x=(int(SnapshotWidth)-int(width*scale))/2+int(224*scale),y=(int(SnapshotHeight)-int(height*scale))/2+int(164*scale);
        const auto* pixel=image.bgra.data()+(size_t(y)*SnapshotWidth+x)*4;
        if(std::abs(int(pixel[0])-30)>10||std::abs(int(pixel[1])-80)>10||std::abs(int(pixel[2])-210)>10)throw std::runtime_error("Real primary snapshot did not contain the known test window");
        DestroyWindow(window);window=nullptr;
        report="PASS: actual primary snapshot -> bounded JPEG -> thumbnail decode; known visible test color verified, aspect fit, "+std::to_string(snapshot.jpeg.size())+" bytes. Test window closed; no image saved.";return true;
    }catch(const std::exception& error){if(window)DestroyWindow(window);report=std::string("FAIL: ")+error.what();return false;}
}
}
