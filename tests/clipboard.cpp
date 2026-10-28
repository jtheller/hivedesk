#include "hivedesk/clipboard.h"
#include "hivedesk/transport.h"
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
using Microsoft::WRL::ComPtr;
using Bytes=std::vector<uint8_t>;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void until(F predicate,const char* message){const auto end=GetTickCount64()+5000;while(!predicate()){if(GetTickCount64()>end)throw std::runtime_error(message);MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(2);}}
void set_native(HWND owner,UINT format,const Bytes& bytes){require(OpenClipboard(owner)!=FALSE,"Open isolated clipboard");EmptyClipboard();auto h=GlobalAlloc(GMEM_MOVEABLE,bytes.size());auto p=GlobalLock(h);memcpy(p,bytes.data(),bytes.size());GlobalUnlock(h);require(SetClipboardData(format,h)!=nullptr,"Set isolated clipboard");CloseClipboard();}
Bytes data(IDataObject* object,UINT format,LONG index=-1){FORMATETC f{CLIPFORMAT(format),nullptr,DVASPECT_CONTENT,index,TYMED_HGLOBAL};STGMEDIUM m{};require(SUCCEEDED(object->GetData(&f,&m)),"Render clipboard format");const auto p=static_cast<uint8_t*>(GlobalLock(m.hGlobal));Bytes b(p,p+GlobalSize(m.hGlobal));GlobalUnlock(m.hGlobal);ReleaseStgMedium(&m);return b;}
ComPtr<IStream> stream(IDataObject* object,UINT index){FORMATETC f{CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILECONTENTS)),nullptr,DVASPECT_CONTENT,LONG(index),TYMED_ISTREAM};STGMEDIUM m{};require(SUCCEEDED(object->GetData(&f,&m)),"Get virtual file stream");ComPtr<IStream> result=m.pstm;ReleaseStgMedium(&m);return result;}
void isolated_station(){
    USEROBJECTFLAGS flags{};DWORD size=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&size)&&!(flags.dwFlags&WSF_VISIBLE),"Refuse clipboard consumer on interactive station");
}
int consume(const std::wstring& kind,const std::filesystem::path& output){
    isolated_station();require(SUCCEEDED(OleInitialize(nullptr)),"Consumer OLE init");
    struct Ole{~Ole(){OleUninitialize();}}ole;
    if(kind==L"files"){
        ComPtr<IDataObject> object;require(SUCCEEDED(OleGetClipboard(&object)),"Cross-process OleGetClipboard");
        ComPtr<IDataObjectAsyncCapability> async;require(SUCCEEDED(object.As(&async)),"Cross-process async capability");
        ComPtr<IShellItem> folder;require(SUCCEEDED(SHCreateItemFromParsingName(output.c_str(),nullptr,IID_PPV_ARGS(&folder))),"Consumer destination");
        ComPtr<IDropTarget> target;require(SUCCEEDED(folder->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&target))),"Consumer folder drop target");
        DWORD effect=DROPEFFECT_COPY;POINTL point{};
        require(SUCCEEDED(target->DragEnter(object.Get(),MK_LBUTTON|MK_CONTROL,point,&effect))&&(effect&DROPEFFECT_COPY),"Cross-process Shell accepts files");
        require(SUCCEEDED(target->Drop(object.Get(),MK_CONTROL,point,&effect)),"Cross-process Shell paste");
        until([&]{BOOL active=FALSE;async->InOperation(&active);return !active&&std::filesystem::exists(output/L"zero.txt");},"Cross-process Shell copy completion");
    }else{
        // Ordinary applications call Win32 GetClipboardData, not our IDataObject
        // directly. Exercise OLE delayed rendering and its cross-process marshal.
        const UINT format=kind==L"png"?RegisterClipboardFormatW(L"PNG"):CF_DIBV5;
        require(OpenClipboard(nullptr)!=FALSE,"Consumer opens clipboard");
        const auto handle=GetClipboardData(format);if(!handle){CloseClipboard();throw std::runtime_error("Cross-process native image paste");}
        const auto size=GlobalSize(handle);const auto bytes=GlobalLock(handle);
        if(!bytes){CloseClipboard();throw std::runtime_error("Consumer locks image");}
        {std::ofstream file(output,std::ios::binary);file.write(static_cast<const char*>(bytes),size);require(bool(file),"Save pasted image fixture");}
        GlobalUnlock(handle);CloseClipboard();
    }
    std::puts("PASS separate-process clipboard consumer");return 0;
}
void consume_process(const wchar_t* kind,const std::filesystem::path& output){
    isolated_station();wchar_t executable[32768]{},station[256]{},desktop[256]{};DWORD size=0;
    GetModuleFileNameW(nullptr,executable,32768);
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,station,sizeof(station),&size)&&GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&size),"Consumer desktop name");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --consumer "+kind+L" \""+output.wstring()+L"\"",location=std::wstring(station)+L"\\"+desktop;
    STARTUPINFOW startup{sizeof(startup)};startup.lpDesktop=location.data();startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"Launch clipboard consumer");
    const auto deadline=GetTickCount64()+15000;
    while(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT&&GetTickCount64()<deadline){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(2);}
    DWORD code=124;if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT){TerminateProcess(process.hProcess,124);WaitForSingleObject(process.hProcess,3000);}else GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hThread);CloseHandle(process.hProcess);require(code==0,"Separate-process clipboard paste failed or stalled");
}
int probe(){
    setvbuf(stdout,nullptr,_IONBF,0);
    USEROBJECTFLAGS flags{};DWORD bytes=0;require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),"Refuse clipboard tests on interactive station");
    require(SUCCEEDED(OleInitialize(nullptr)),"OLE init");struct Ole{~Ole(){OleUninitialize();}}ole;
    HWND owner=CreateWindowW(L"STATIC",L"Clipboard fixture",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);require(owner!=nullptr,"Clipboard fixture owner");
    const auto root=std::filesystem::temp_directory_path()/(L"HiveDesk-ClipboardTest-"+std::to_wstring(GetCurrentProcessId()));require(!std::filesystem::exists(root),"Fresh fixture directory");std::filesystem::create_directories(root/L"source"/L"folder"/L"empty");std::filesystem::create_directory(root/L"destination");
    Bytes file_bytes(3*hive::ClipboardChunk+17);for(size_t i=0;i<file_bytes.size();++i)file_bytes[i]=uint8_t((i*71+19)%251);
    const auto file=root/L"source"/L"folder"/L"\u6d4b\u8bd5.bin";{std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(file_bytes.data()),file_bytes.size());}
    {std::ofstream out(root/L"source"/L"zero.txt",std::ios::binary);}
    std::wstring paths=(root/L"source"/L"folder").wstring();paths.push_back(0);paths+=(root/L"source"/L"zero.txt").wstring();paths.push_back(0);paths.push_back(0);
    Bytes drop(sizeof(DROPFILES)+paths.size()*2);auto header=reinterpret_cast<DROPFILES*>(drop.data());header->pFiles=sizeof(DROPFILES);header->fWide=TRUE;memcpy(drop.data()+sizeof(DROPFILES),paths.data(),paths.size()*2);set_native(owner,CF_HDROP,drop);
    auto local=hive::capture_clipboard(owner);require(local->offer.files.size()==4,"Capture nested folder, empty folder and empty file");
    hive::ClipboardOffer decoded;const auto encoded=hive::encode_clipboard_offer(local->offer);require(hive::decode_clipboard_offer(encoded,decoded),"Offer roundtrip");
    for(const auto* bad:{L"..\\outside",L"C:\\outside",L"x:stream",L"CON",L"dir\\..\\x",L"x.",L"x\\",L"missing\\child"}){auto invalid=decoded;invalid.files[0].name=bad;require(!hive::valid_clipboard_offer(invalid),"Reject unsafe file manifest");}
    auto malformed=encoded;malformed.push_back(0);require(!hive::decode_clipboard_offer(malformed,decoded),"Reject trailing metadata");
    auto huge=local->offer;huge.files[2].size=UINT64_MAX;require(!hive::valid_clipboard_offer(huge),"Reject impossible file size");

    hive::Transport left,right;hive::ClipboardTransfer sender(left),receiver(right);std::mutex mutex;hive::Clipboard received,returned;std::atomic<unsigned> reads=0,frames=0,inputs=0;std::atomic<bool> block=false,unblock=false;
    struct Release{std::atomic<bool>& flag;~Release(){flag=true;}}release{unblock};
    auto counted=std::make_shared<hive::ClipboardContent>(*local);counted->read=[&](uint32_t item,uint64_t offset,uint32_t count){++reads;return local->read(item,offset,count);};
    hive::TransportCallbacks a,b;a.clipboard_offer=[&](const auto& peer,const auto& o){auto c=sender.receive(peer,o);std::lock_guard lock(mutex);returned=c;};b.clipboard_offer=[&](const auto& peer,const auto& o){auto c=receiver.receive(peer,o);std::lock_guard lock(mutex);received=c;};
    a.clipboard_packet=[&](const auto& peer,const auto& packet){sender.packet(peer,packet);};b.clipboard_packet=[&](const auto& peer,const auto& packet){receiver.packet(peer,packet);};
    b.clipboard=[&](const auto&,const auto&){block=true;while(!unblock)Sleep(1);};b.frame=[&](const auto&,auto){++frames;};
    b.input_begin=[](const auto&,uint64_t){return true;};b.input_packet=[&](const auto& peer,const hive::InputPacket& p){++inputs;right.send_input_ack(peer,{p.session,p.sequence,p.capture_time_us,0,0,true});};
    std::string error,id;require(right.start({"clip-b","Clipboard B","clipboard-test",48221,false},b,error),error.c_str());require(left.start({"clip-a","Clipboard A","clipboard-test",48222,false},a,error),error.c_str());require(left.connect("127.0.0.1",48221,id,error),error.c_str());
    require(sender.send(id,counted),"Advertise file clipboard");until([&]{std::lock_guard lock(mutex);return bool(received);},"Receive clipboard offer");require(reads==0,"Advertising copied file data eagerly");
    hive::Clipboard remote;{std::lock_guard lock(mutex);remote=received;}
    require(receiver.send("clip-a",remote),"Carry clipboard back to origin");until([&]{std::lock_guard lock(mutex);return bool(returned);},"Receive return offer");{std::lock_guard lock(mutex);require(returned==counted,"Returning clipboard built a proxy cycle");}
    hive::ClipboardDesktop desktop;std::atomic<unsigned> changed=0;require(desktop.start([&](auto){++changed;},error),error.c_str());Sleep(50);changed=0;desktop.apply(remote);
    ComPtr<IDataObject> object;require(SUCCEEDED(OleGetClipboard(&object)),"Get native OLE clipboard");auto descriptors=data(object.Get(),RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));const auto group=reinterpret_cast<const FILEGROUPDESCRIPTORW*>(descriptors.data());require(group->cItems==4,"Native file descriptor count");require(reads==0,"Descriptor read fetched file bytes");
    ComPtr<IDataObjectAsyncCapability> async;require(SUCCEEDED(object.As(&async)),"Native async copy capability");BOOL asynchronous=FALSE;require(SUCCEEDED(async->GetAsyncMode(&asynchronous))&&asynchronous,"File paste must support asynchronous extraction");
    size_t file_index=0;for(;file_index<remote->offer.files.size();++file_index)if(remote->offer.files[file_index].name.ends_with(L".bin"))break;require(file_index<remote->offer.files.size(),"Nested filename");
    require(SUCCEEDED(async->StartOperation(nullptr))&&desktop.busy(),"Active clipboard copy must block idle OTA");
    async->StartOperation(nullptr);async->EndOperation(S_OK,nullptr,DROPEFFECT_COPY);require(desktop.busy(),"Concurrent copy lost its idle guard");
    async->EndOperation(S_OK,nullptr,DROPEFFECT_COPY);require(!desktop.busy(),"Completed clipboard operation must release idle guard");
    auto input=stream(object.Get(),UINT(file_index));STATSTG stat{};require(SUCCEEDED(input->Stat(&stat,STATFLAG_NONAME))&&stat.cbSize.QuadPart==file_bytes.size(),"64-bit stream size");
    Bytes actual(file_bytes.size()+16);ULONG got=0;require(input->Read(actual.data(),ULONG(actual.size()),&got)==S_FALSE&&got==file_bytes.size()&&std::equal(file_bytes.begin(),file_bytes.end(),actual.begin()),"Encrypted multi-chunk file stream or EOF");
    LARGE_INTEGER seek{};seek.QuadPart=-17;require(SUCCEEDED(input->Seek(seek,STREAM_SEEK_END,nullptr)),"Seek from end");ComPtr<IStream> clone;require(SUCCEEDED(input->Clone(&clone)),"Clone virtual stream");require(clone->Read(actual.data(),17,&got)==S_OK&&std::equal(file_bytes.end()-17,file_bytes.end(),actual.begin()),"Cloned stream position");
    // Block the reliable control callback, then prove file, video and input lanes
    // all progress independently. No input event is injected into Windows.
    require(left.begin_input(id,917),"Begin synthetic input");until([&]{return left.input_ready(id,917);},"Input readiness");require(left.send_clipboard(id,L"block"),"Queue blocked control callback");until([&]{return block.load();},"Control callback block");
    seek.QuadPart=0;input->Seek(seek,STREAM_SEEK_SET,nullptr);require(input->Read(actual.data(),4096,&got)==S_OK&&got==4096,"File data stalled behind control");
    hive::EncodedFrame frame;frame.width=64;frame.height=64;frame.sequence=1;frame.keyframe=true;frame.data={1,2,3};require(left.send_frame(id,frame),"Video lane during clipboard copy");hive::InputPacket packet;packet.session=917;packet.sequence=1;packet.capture_time_us=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());packet.event={hive::InputKind::Key,30,0,0};require(left.send_input(id,packet),"Input lane during clipboard copy");until([&]{return frames>0&&inputs>0;},"File transfer blocked video/input");unblock=true;
    // Windows' actual Shell copy engine consumes the virtual files into a
    // generated destination. No Explorer window, desktop input or real files.
    std::puts("CHECK native destination");
    ComPtr<IShellItem> destination;require(SUCCEEDED(SHCreateItemFromParsingName((root/L"destination").c_str(),nullptr,IID_PPV_ARGS(&destination))),"Destination shell item");
    std::puts("CHECK native drop target");
    ComPtr<IDropTarget> target;require(SUCCEEDED(destination->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&target))),"Native folder drop target");
    std::puts("CHECK native DragEnter");
    DWORD effect=DROPEFFECT_COPY;POINTL point{};require(SUCCEEDED(target->DragEnter(object.Get(),MK_LBUTTON|MK_CONTROL,point,&effect))&&(effect&DROPEFFECT_COPY),"Shell accepts virtual clipboard files");
    std::puts("CHECK native Drop");
    require(SUCCEEDED(target->Drop(object.Get(),MK_CONTROL,point,&effect)),"Shell paste virtual clipboard files");
    std::printf("CHECK native copy completion effect=%lu\n",effect);
    until([&]{BOOL active=FALSE;async->InOperation(&active);return !active&&std::filesystem::exists(root/L"destination"/L"zero.txt");},"Native shell copy completion");
    require(std::filesystem::exists(root/L"destination"/L"folder"/L"empty")&&std::filesystem::file_size(root/L"destination"/L"folder"/L"\u6d4b\u8bd5.bin")==file_bytes.size()&&std::filesystem::file_size(root/L"destination"/L"zero.txt")==0,"Shell copy did not materialize folder/files");
    {std::ifstream in(root/L"destination"/L"folder"/L"\u6d4b\u8bd5.bin",std::ios::binary);Bytes copy((std::istreambuf_iterator<char>(in)),{});require(copy==file_bytes,"Native pasted file hash/content mismatch");}
    require(std::filesystem::exists(file),"Copy removed source");Sleep(50);require(changed==0,"Remote clipboard echoed through listener");
    std::filesystem::create_directory(root/L"cross-process");consume_process(L"files",root/L"cross-process");
    {std::ifstream in(root/L"cross-process"/L"folder"/L"\u6d4b\u8bd5.bin",std::ios::binary);Bytes copy((std::istreambuf_iterator<char>(in)),{});require(copy==file_bytes&&std::filesystem::is_directory(root/L"cross-process"/L"folder"/L"empty"),"Cross-process folder paste contents");}
    // Native DIBV5 (including alpha) survives offer + lazy reads byte-for-byte.
    Bytes dib(sizeof(BITMAPV5HEADER)+16*8*4);auto bitmap=reinterpret_cast<BITMAPV5HEADER*>(dib.data());bitmap->bV5Size=sizeof(BITMAPV5HEADER);bitmap->bV5Width=16;bitmap->bV5Height=-8;bitmap->bV5Planes=1;bitmap->bV5BitCount=32;bitmap->bV5Compression=BI_BITFIELDS;bitmap->bV5RedMask=0x00ff0000;bitmap->bV5GreenMask=0x0000ff00;bitmap->bV5BlueMask=0x000000ff;bitmap->bV5AlphaMask=0xff000000;for(size_t i=sizeof(BITMAPV5HEADER);i<dib.size();++i)dib[i]=uint8_t(i);
    set_native(owner,CF_DIBV5,dib);auto picture=hive::capture_clipboard(owner);require(picture->offer.images.size()==1,"Capture native alpha bitmap");{std::lock_guard lock(mutex);received.reset();}require(sender.send(id,picture),"Offer image");until([&]{std::lock_guard lock(mutex);return bool(received);},"Receive image offer");{std::lock_guard lock(mutex);remote=received;}ComPtr<IDataObject> image;image.Attach(hive::clipboard_data_object(remote));require(data(image.Get(),CF_DIBV5)==dib,"Remote image pixels changed");
    desktop.apply(remote);consume_process(L"dib",root/L"pasted.dib");{std::ifstream in(root/L"pasted.dib",std::ios::binary);Bytes copy((std::istreambuf_iterator<char>(in)),{});require(copy==dib,"Cross-process native bitmap pixels changed");}
    ComPtr<IWICImagingFactory> factory;require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"PNG factory");
    ComPtr<IStream> png_stream;require(SUCCEEDED(CreateStreamOnHGlobal(nullptr,TRUE,&png_stream)),"PNG stream");ComPtr<IWICBitmapEncoder> encoder;require(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(png_stream.Get(),WICBitmapEncoderNoCache)),"PNG encoder");
    ComPtr<IWICBitmapFrameEncode> png_frame;require(SUCCEEDED(encoder->CreateNewFrame(&png_frame,nullptr))&&SUCCEEDED(png_frame->Initialize(nullptr))&&SUCCEEDED(png_frame->SetSize(16,8)),"PNG frame");auto pixel_format=GUID_WICPixelFormat32bppBGRA;require(SUCCEEDED(png_frame->SetPixelFormat(&pixel_format))&&pixel_format==GUID_WICPixelFormat32bppBGRA,"PNG lossless format");require(SUCCEEDED(png_frame->WritePixels(8,64,512,dib.data()+sizeof(BITMAPV5HEADER)))&&SUCCEEDED(png_frame->Commit())&&SUCCEEDED(encoder->Commit()),"Encode PNG fixture");
    HGLOBAL png_memory=nullptr;GetHGlobalFromStream(png_stream.Get(),&png_memory);STATSTG png_stat{};png_stream->Stat(&png_stat,STATFLAG_NONAME);auto png_data=static_cast<const uint8_t*>(GlobalLock(png_memory));Bytes png(png_data,png_data+size_t(png_stat.cbSize.QuadPart));GlobalUnlock(png_memory);
    set_native(owner,RegisterClipboardFormatW(L"PNG"),png);require(!desktop.cached()->offer.images.empty(),"Immediate switch used stale clipboard cache");picture=hive::capture_clipboard(owner);require(picture->offer.images.size()==1&&picture->offer.images[0].format==hive::ClipboardFormat::Png,"Capture native PNG");
    {std::lock_guard lock(mutex);received.reset();}require(sender.send(id,picture),"Offer PNG");until([&]{std::lock_guard lock(mutex);return bool(received);},"Receive PNG");{std::lock_guard lock(mutex);remote=received;}image.Attach(hive::clipboard_data_object(remote));require(data(image.Get(),RegisterClipboardFormatW(L"PNG"))==png,"PNG bytes/alpha changed");
    desktop.apply(remote);consume_process(L"png",root/L"pasted.png");{std::ifstream in(root/L"pasted.png",std::ios::binary);Bytes copy((std::istreambuf_iterator<char>(in)),{});require(copy==png,"Cross-process native PNG bytes changed");}
    auto forged=remote->offer;forged.id^=0xdeadbeef12345678ull;bool rejected=false;try{receiver.receive("clip-a",forged)->read(0,0,16);}catch(...){rejected=true;}require(rejected,"Read of an unadvertised clipboard object succeeded");
    auto large=std::make_shared<hive::ClipboardContent>(*hive::text_clipboard(L""));large->offer.files.push_back({L"large.bin",5ull*1024*1024*1024+17,0,false});large->read=[](uint32_t,uint64_t offset,uint32_t count){Bytes result(count);for(uint32_t i=0;i<count;++i)result[i]=uint8_t((offset+i)%251);return result;};
    {std::lock_guard lock(mutex);received.reset();}require(sender.send(id,large),"Offer file beyond 4 GiB");until([&]{std::lock_guard lock(mutex);return bool(received);},"Receive large file metadata");ComPtr<IDataObject> large_object;{std::lock_guard lock(mutex);large_object.Attach(hive::clipboard_data_object(received));}auto large_stream=stream(large_object.Get(),0);require(SUCCEEDED(large_stream->Stat(&stat,STATFLAG_NONAME))&&stat.cbSize.QuadPart==large->offer.files[0].size,"Large file size truncated");seek.QuadPart=4ull*1024*1024*1024+123;require(SUCCEEDED(large_stream->Seek(seek,STREAM_SEEK_SET,nullptr)),"Large file seek");require(large_stream->Read(actual.data(),1024,&got)==S_OK&&got==1024,"Large file read");for(unsigned i=0;i<1024;++i)require(actual[i]==uint8_t((uint64_t(seek.QuadPart)+i)%251),"Large file offset truncated on wire");
    // Range failures and changed sources must fail, never supply another copy.
    bool failed=false;try{remote->read(0,UINT64_MAX,16);}catch(...){failed=true;}require(failed,"Reject overflowed range");
    {std::ofstream out(file,std::ios::binary|std::ios::trunc);out<<"changed";}seek.QuadPart=0;input->Seek(seek,STREAM_SEEK_SET,nullptr);require(FAILED(input->Read(actual.data(),16,&got)),"Changed source supplied mismatching bytes");
    desktop.apply(hive::text_clipboard(L"Unicode \u4e2d\u6587 \U0001f642"));require(desktop.cached()->offer.text==L"Unicode \u4e2d\u6587 \U0001f642","Legacy Unicode handoff");
    const auto old_connection=left.connection_token(id);right.stop();until([&]{return left.connection_token(id)==0;},"Detect clipboard connection teardown");require(right.start({"clip-b","Clipboard B","clipboard-test",48221,false},b,error),error.c_str());require(left.connect("127.0.0.1",48221,id,error),error.c_str());require(left.connection_token(id)!=old_connection,"Reconnect reused connection token");
    {std::lock_guard lock(mutex);received.reset();}require(sender.send(id,picture),"Reoffer PNG after reconnect");until([&]{std::lock_guard lock(mutex);return bool(received);},"Receive reconnected PNG");hive::Clipboard fresh;{std::lock_guard lock(mutex);fresh=received;}require(fresh!=remote&&fresh->read(0,0,16)==Bytes(png.begin(),png.begin()+16),"Reconnected offer retained stale reader");
    sender.stop();receiver.stop();left.stop();right.stop();failed=false;try{remote->read(0,0,16);}catch(...){failed=true;}require(failed,"Disconnected clipboard stayed readable");desktop.stop();object.Reset();DestroyWindow(owner);
    // All fixtures are below the uniquely created directory in TEMP.
    require(root.filename().wstring().starts_with(L"HiveDesk-ClipboardTest-"),"Fixture cleanup boundary");std::filesystem::remove_all(root);
    std::puts("PASS rich clipboard: authenticated independent transfer lane, lazy image/file reads, native OLE/async streams, actual Shell folder paste, Unicode/alpha pixels, multi-chunk exact bytes, seek/clone/EOF, carry-back without proxy cycles, source-change rejection, bounds and input/video independence; isolated clipboard only.");return 0;
}
int wmain(int argc,wchar_t** argv){
    if(argc==4&&wcscmp(argv[1],L"--consumer")==0){try{return consume(argv[2],argv[3]);}catch(const std::exception& e){std::fprintf(stderr,"FAIL clipboard consumer: %s\n",e.what());return 1;}}
    if(argc>1){try{return probe();}catch(const std::exception& e){std::fprintf(stderr,"FAIL clipboard: %s\n",e.what());return 1;}}
    const std::wstring name=L"HiveDeskClipboardTest-"+std::to_wstring(GetCurrentProcessId());HWINSTA original=GetProcessWindowStation(),station=CreateWindowStationW(nullptr,0,WINSTA_ALL_ACCESS,nullptr);if(!station)return 1;HDESK desktop=nullptr;
    if(SetProcessWindowStation(station)){desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);SetProcessWindowStation(original);}if(!desktop){CloseWindowStation(station);return 1;}
    wchar_t executable[32768]{},station_name[256]{};DWORD needed=0;GetModuleFileNameW(nullptr,executable,32768);GetUserObjectInformationW(station,UOI_NAME,station_name,sizeof(station_name),&needed);
    std::wstring command=L"\""+std::wstring(executable)+L"\" --child",location=std::wstring(station_name)+L"\\"+name;STARTUPINFOW startup{sizeof(startup)};startup.lpDesktop=location.data();startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION child{};DWORD code=1;
    if(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)){if(WaitForSingleObject(child.hProcess,60000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&code);else{TerminateProcess(child.hProcess,124);WaitForSingleObject(child.hProcess,5000);code=124;}CloseHandle(child.hThread);CloseHandle(child.hProcess);}CloseDesktop(desktop);CloseWindowStation(station);return int(code);
}
