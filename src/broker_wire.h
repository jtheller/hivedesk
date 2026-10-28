#pragma once
#include "hivedesk/types.h"
#include <windows.h>
#include <vector>
#include <string>
#include <filesystem>
#include <cstring>
#include <atomic>
#include <utility>
namespace hive::broker {
using Bytes=std::vector<uint8_t>;
// Frame layout changed with image-associated cursor composition. Refuse a
// mixed install rather than treating metadata bytes as compressed image data.
constexpr uint32_t Magic=0x48425633,MaxBytes=16*1024*1024+128;
enum Kind:uint32_t {Start=1,Ready=2,Input=3,Feedback=4,Keyframe=5,Frame=6,Health=7,Started=8,Failure=9,Notice=10,Diagnostics=11,InputStart=12,InputStarted=13,InputComplete=14,Activate=15,Activated=16,Preview=17};
inline std::wstring pipe_name(DWORD session){return L"\\\\.\\pipe\\HiveDesk.Host."+std::to_wstring(session);}
inline std::wstring input_pipe_name(DWORD session){return L"\\\\.\\pipe\\HiveDesk.Input."+std::to_wstring(session);}
inline std::filesystem::path module_directory(){wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);return std::filesystem::path(path).parent_path();}
inline void u32(Bytes& data,uint32_t n){for(int i=0;i<4;++i)data.push_back(uint8_t(n>>(i*8)));}
inline void u64(Bytes& data,uint64_t n){u32(data,uint32_t(n));u32(data,uint32_t(n>>32));}
struct Reader {const Bytes& data;size_t at=0;bool good=true;
    uint32_t u32(){if(at+4>data.size()){good=false;return 0;}uint32_t n=0;for(int i=0;i<4;++i)n|=uint32_t(data[at++])<<(i*8);return n;}
    uint64_t u64(){const auto low=u32();return low|(uint64_t(u32())<<32);}
    bool end()const{return good&&at==data.size();}
};
inline Bytes write_frame(const EncodedFrame& frame){
    Bytes bytes;bytes.reserve(frame.data.size()+48);
    u32(bytes,frame.width);u32(bytes,frame.height);u64(bytes,frame.sequence);u64(bytes,frame.capture_time_us);
    u64(bytes,frame.session);u32(bytes,frame.keyframe?1:0);u64(bytes,frame.source_ready_us);
    u32(bytes,uint32_t(frame.cursor_composition));bytes.insert(bytes.end(),frame.data.begin(),frame.data.end());return bytes;
}
inline bool read_frame(const Bytes& bytes,EncodedFrame& result){
    Reader reader{bytes};EncodedFrame frame;
    frame.width=reader.u32();frame.height=reader.u32();frame.sequence=reader.u64();frame.capture_time_us=reader.u64();
    frame.session=reader.u64();const auto key=reader.u32();frame.keyframe=key!=0;frame.source_ready_us=reader.u64();
    const auto composition=reader.u32();
    // Check the full wire integer before narrowing to the enum's byte.
    if(!reader.good||!frame.width||!frame.height||frame.width>8192||frame.height>8192||key>1||
        composition>uint32_t(CursorComposition::NoSeparate)||reader.at==bytes.size()||
        bytes.size()-reader.at>16*1024*1024||!valid_frame_timing(frame))return false;
    frame.cursor_composition=CursorComposition(composition);
    frame.data.assign(bytes.begin()+reader.at,bytes.end());result=std::move(frame);return true;
}
// Overlapped I/O with an explicit cancellation flag avoids a stop/ReadFile race
// and unbounded joins when a peer stops consuming a large encoded frame.
inline bool transfer(HANDLE pipe,void* data,size_t bytes,bool write,const std::atomic<bool>* cancel){
    OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!operation.hEvent)return false;
    bool ok=true;auto* p=static_cast<uint8_t*>(data);
    while(bytes){
        if(cancel&&cancel->load()){ok=false;break;}ResetEvent(operation.hEvent);DWORD count=0;
        BOOL done=write?WriteFile(pipe,p,DWORD(bytes),&count,&operation):ReadFile(pipe,p,DWORD(bytes),&count,&operation);
        if(!done&&GetLastError()!=ERROR_IO_PENDING){ok=false;break;}
        if(!done){
            while(WaitForSingleObject(operation.hEvent,100)==WAIT_TIMEOUT){
                if(cancel&&cancel->load()){CancelIoEx(pipe,&operation);break;}
            }
            if(!GetOverlappedResult(pipe,&operation,&count,TRUE)){ok=false;break;}
        }
        if(!count){ok=false;break;}p+=count;bytes-=count;
    }
    CloseHandle(operation.hEvent);return ok;
}
inline bool send(HANDLE pipe,Kind kind,const Bytes& data={},const std::atomic<bool>* cancel=nullptr){if(data.size()>MaxBytes)return false;uint32_t header[]{Magic,kind,uint32_t(data.size())};return transfer(pipe,header,sizeof(header),true,cancel)&&(data.empty()||transfer(pipe,const_cast<uint8_t*>(data.data()),data.size(),true,cancel));}
inline bool receive(HANDLE pipe,Kind& kind,Bytes& data,const std::atomic<bool>* cancel=nullptr,uint32_t max_bytes=MaxBytes){uint32_t header[3]{};if(!transfer(pipe,header,sizeof(header),false,cancel)||header[0]!=Magic||header[1]<Start||header[1]>Activated||header[2]>max_bytes)return false;kind=Kind(header[1]);data.resize(header[2]);return data.empty()||transfer(pipe,data.data(),data.size(),false,cancel);}
inline bool valid_input(const InputEvent& e){switch(e.kind){
case InputKind::Key:return e.a>0&&e.a<=255&&(e.b==0||e.b==1)&&(e.c==0||e.c==1);
case InputKind::Absolute:return e.a>=0&&e.a<=65535&&e.b>=0&&e.b<=65535;
case InputKind::Relative:return true;
case InputKind::Button:return e.a>=1&&e.a<=5&&(e.b==0||e.b==1);
case InputKind::Wheel:return e.b==0||e.b==1;
case InputKind::ReleaseAll:return true;
default:return false;}}
}
