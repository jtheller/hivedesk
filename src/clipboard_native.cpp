#include "hivedesk/clipboard.h"
#include "hivedesk/config.h"
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace hive { namespace {
using Bytes=std::vector<uint8_t>;
UINT descriptor_format(){static auto f=RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);return f;}
UINT contents_format(){static auto f=RegisterClipboardFormatW(CFSTR_FILECONTENTS);return f;}
UINT png_format(){static auto f=RegisterClipboardFormatW(L"PNG");return f;}
UINT effect_format(){static auto f=RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);return f;}
UINT native_format(ClipboardFormat format){return format==ClipboardFormat::Dib?CF_DIB:format==ClipboardFormat::DibV5?CF_DIBV5:png_format();}
struct Handle {HANDLE h=INVALID_HANDLE_VALUE;~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
uint64_t time_value(FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
FILETIME file_time(uint64_t value){return {DWORD(value),DWORD(value>>32)};}
HGLOBAL global_copy(const void* source,size_t size){auto h=GlobalAlloc(GMEM_MOVEABLE,std::max<size_t>(size,1));if(!h)throw std::bad_alloc();auto p=GlobalLock(h);if(!p){GlobalFree(h);throw std::bad_alloc();}if(size)memcpy(p,source,size);GlobalUnlock(h);return h;}
Bytes clipboard_bytes(UINT format,uint64_t limit){const auto h=GetClipboardData(format);if(!h)return {};const auto size=GlobalSize(h);if(!size||size>limit)throw std::runtime_error("Clipboard format is too large");const auto p=static_cast<const uint8_t*>(GlobalLock(h));if(!p)throw std::runtime_error("Clipboard format unavailable");struct Unlock{HANDLE h;~Unlock(){GlobalUnlock(h);}}unlock{h};return Bytes(p,p+size);}
bool bitmap_bytes(const Bytes& b,ClipboardFormat format){
    if(format==ClipboardFormat::Png){constexpr uint8_t signature[]={137,80,78,71,13,10,26,10};return b.size()>=33&&!memcmp(b.data(),signature,8);}
    if(b.size()<sizeof(BITMAPINFOHEADER))return false;BITMAPINFOHEADER h{};memcpy(&h,b.data(),sizeof(h));
    if(h.biSize<40||h.biSize>b.size()||h.biWidth<=0||h.biWidth>32768||!h.biHeight||h.biHeight==LONG_MIN||abs(h.biHeight)>32768||h.biPlanes!=1)return false;
    if(h.biBitCount!=1&&h.biBitCount!=4&&h.biBitCount!=8&&h.biBitCount!=16&&h.biBitCount!=24&&h.biBitCount!=32)return false;
    if(h.biCompression!=BI_RGB&&h.biCompression!=BI_BITFIELDS)return false;
    if(h.biClrUsed>256)return false;const uint64_t palette=h.biClrUsed?h.biClrUsed:h.biBitCount<=8?(1ull<<h.biBitCount):0;
    const uint64_t offset=h.biSize+palette*4+(h.biSize==40&&h.biCompression==BI_BITFIELDS?12:0);
    const uint64_t stride=((uint64_t(h.biWidth)*h.biBitCount+31)/32)*4;
    return offset<=b.size()&&stride*abs(h.biHeight)<=b.size()-offset;
}
struct LocalFile {std::filesystem::path path;BY_HANDLE_FILE_INFORMATION identity{};};
BY_HANDLE_FILE_INFORMATION file_info(HANDLE h){BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Clipboard file unavailable or reparse point");return info;}
bool same_file(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&time_value(a.ftLastWriteTime)==time_value(b.ftLastWriteTime);}

class FileStream final : public IStream {
    std::atomic<ULONG> refs{1};Clipboard content;uint32_t item;uint64_t size,position=0;std::mutex mutex;IUnknown* marshaler=nullptr;
public:
    FileStream(Clipboard c,uint32_t index,uint64_t bytes):content(std::move(c)),item(index),size(bytes){CoCreateFreeThreadedMarshaler(static_cast<IStream*>(this),&marshaler);}
    ~FileStream(){if(marshaler)marshaler->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_ISequentialStream||iid==IID_IStream){*out=static_cast<IStream*>(this);AddRef();return S_OK;}if(iid==IID_IMarshal&&marshaler)return marshaler->QueryInterface(iid,out);return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Read(void* buffer,ULONG count,ULONG* read)override{
        if(read)*read=0;if(count&&!buffer)return STG_E_INVALIDPOINTER;std::lock_guard lock(mutex);ULONG done=0;
        try{while(done<count&&position<size){const auto length=uint32_t(std::min<uint64_t>({count-done,size-position,ClipboardChunk}));auto b=content->read(item,position,length);if(b.size()!=length)return STG_E_READFAULT;memcpy(static_cast<uint8_t*>(buffer)+done,b.data(),length);position+=length;done+=length;if(read)*read=done;}return done==count?S_OK:S_FALSE;}catch(...){return STG_E_READFAULT;}
    }
    HRESULT STDMETHODCALLTYPE Write(const void*,ULONG,ULONG*)override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER* result)override{
        std::lock_guard lock(mutex);uint64_t base=origin==STREAM_SEEK_SET?0:origin==STREAM_SEEK_CUR?position:size;if(origin>STREAM_SEEK_END)return STG_E_INVALIDFUNCTION;
        if(move.QuadPart<0){const auto amount=uint64_t(-(move.QuadPart+1))+1;if(amount>base)return STG_E_INVALIDFUNCTION;position=base-amount;}else{if(uint64_t(move.QuadPart)>uint64_t(INT64_MAX)-base)return STG_E_INVALIDFUNCTION;position=base+uint64_t(move.QuadPart);}if(result)result->QuadPart=position;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER)override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target,ULARGE_INTEGER count,ULARGE_INTEGER* read,ULARGE_INTEGER* written)override{
        if(read)read->QuadPart=0;if(written)written->QuadPart=0;if(!target)return E_POINTER;Bytes buffer(ClipboardChunk);uint64_t done=0;
        while(done<count.QuadPart){ULONG got=0,sent=0;auto hr=Read(buffer.data(),ULONG(std::min<uint64_t>(buffer.size(),count.QuadPart-done)),&got);if(read)read->QuadPart+=got;if(FAILED(hr))return hr;if(!got)return S_FALSE;hr=target->Write(buffer.data(),got,&sent);if(written)written->QuadPart+=sent;if(FAILED(hr)||sent!=got)return STG_E_WRITEFAULT;done+=got;}return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD)override{return S_OK;}HRESULT STDMETHODCALLTYPE Revert()override{return STG_E_REVERTED;}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD)override{return STG_E_INVALIDFUNCTION;}HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD)override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* s,DWORD)override{if(!s)return E_POINTER;*s={};s->type=STGTY_STREAM;s->cbSize.QuadPart=size;s->grfMode=STGM_READ;return S_OK;}
    HRESULT STDMETHODCALLTYPE Clone(IStream** out)override{if(!out)return E_POINTER;*out=nullptr;try{std::lock_guard lock(mutex);auto c=new FileStream(content,item,size);c->position=position;*out=c;return S_OK;}catch(...){return E_OUTOFMEMORY;}}
};

class DataObject final : public IDataObject, public IDataObjectAsyncCapability {
    std::atomic<ULONG> refs{1};Clipboard content;std::shared_ptr<std::atomic<unsigned>> activity;std::atomic<BOOL> asynchronous{VARIANT_TRUE};std::atomic<unsigned> operations{0};
    std::vector<FORMATETC> formats()const{std::vector<FORMATETC> v;auto add=[&](UINT f,DWORD medium,LONG index=-1){v.push_back({CLIPFORMAT(f),nullptr,DVASPECT_CONTENT,index,medium});};
        if(!content->offer.text.empty()||(content->offer.images.empty()&&content->offer.files.empty()))add(CF_UNICODETEXT,TYMED_HGLOBAL);
        for(const auto& i:content->offer.images)add(native_format(i.format),TYMED_HGLOBAL);
        if(!content->offer.files.empty()){add(descriptor_format(),TYMED_HGLOBAL);add(effect_format(),TYMED_HGLOBAL);add(contents_format(),TYMED_ISTREAM);}return v;}
public:
    // OLE clipboard wrappers require the standard IDataObject marshaler.
    // Aggregating FTM here breaks Shell asynchronous paste. Only IStream is FTM.
    explicit DataObject(Clipboard c,std::shared_ptr<std::atomic<unsigned>> a={}):content(std::move(c)),activity(std::move(a)){}
    ~DataObject(){if(activity)activity->fetch_sub(operations.load());}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_IDataObject)*out=static_cast<IDataObject*>(this);else if(iid==__uuidof(IDataObjectAsyncCapability))*out=static_cast<IDataObjectAsyncCapability*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f)override{
        if(!f)return E_POINTER;if(f->dwAspect!=DVASPECT_CONTENT)return DV_E_DVASPECT;if(f->ptd)return DV_E_DVTARGETDEVICE;
        DWORD medium=0;
        if(f->cfFormat==CF_UNICODETEXT&&(!content->offer.text.empty()||(content->offer.images.empty()&&content->offer.files.empty())))medium=TYMED_HGLOBAL;
        for(const auto& image:content->offer.images)if(f->cfFormat==native_format(image.format))medium=TYMED_HGLOBAL;
        if(!content->offer.files.empty()){if(f->cfFormat==descriptor_format()||f->cfFormat==effect_format())medium=TYMED_HGLOBAL;else if(f->cfFormat==contents_format())medium=TYMED_ISTREAM;}
        if(!medium)return DV_E_FORMATETC;if(!(f->tymed&medium))return DV_E_TYMED;
        if(f->cfFormat==contents_format()){if(f->lindex < -1 || (f->lindex>=0&&(size_t(f->lindex)>=content->offer.files.size()||content->offer.files[f->lindex].directory)))return DV_E_LINDEX;}else if(f->lindex!=-1)return DV_E_LINDEX;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f,STGMEDIUM* medium)override{
        struct Reading {std::shared_ptr<std::atomic<unsigned>> counter;Reading(decltype(counter) c):counter(std::move(c)){if(counter)++*counter;}~Reading(){if(counter)--*counter;}}reading{activity};
        if(!medium)return E_POINTER;*medium={};const auto valid=QueryGetData(f);if(FAILED(valid))return valid;
        try{if(f->cfFormat==contents_format()){if(f->lindex<0)return DV_E_LINDEX;medium->tymed=TYMED_ISTREAM;medium->pstm=new FileStream(content,uint32_t(content->offer.images.size()+f->lindex),content->offer.files[f->lindex].size);return S_OK;}
            HGLOBAL h=nullptr;
            if(f->cfFormat==CF_UNICODETEXT)h=global_copy(content->offer.text.c_str(),(content->offer.text.size()+1)*2);
            else if(f->cfFormat==effect_format()){DWORD effect=DROPEFFECT_COPY;h=global_copy(&effect,sizeof(effect));}
            else if(f->cfFormat==descriptor_format()){
                Bytes b(offsetof(FILEGROUPDESCRIPTORW,fgd)+sizeof(FILEDESCRIPTORW)*content->offer.files.size());auto group=reinterpret_cast<FILEGROUPDESCRIPTORW*>(b.data());group->cItems=UINT(content->offer.files.size());
                for(size_t i=0;i<content->offer.files.size();++i){auto& d=group->fgd[i];const auto& file=content->offer.files[i];d.dwFlags=DWORD(FD_ATTRIBUTES|FD_FILESIZE|FD_WRITESTIME|FD_PROGRESSUI|FD_UNICODE);d.dwFileAttributes=file.directory?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL;d.nFileSizeHigh=DWORD(file.size>>32);d.nFileSizeLow=DWORD(file.size);d.ftLastWriteTime=file_time(file.modified);wcscpy_s(d.cFileName,file.name.c_str());}h=global_copy(b.data(),b.size());
            }else{for(uint32_t i=0;i<content->offer.images.size();++i)if(native_format(content->offer.images[i].format)==f->cfFormat){Bytes b;const auto size=content->offer.images[i].size;b.reserve(size);while(b.size()<size){auto next=content->read(i,b.size(),uint32_t(std::min<uint64_t>(ClipboardChunk,size-b.size())));if(next.empty()||next.size()>size-b.size())return STG_E_READFAULT;b.insert(b.end(),next.begin(),next.end());}if(!bitmap_bytes(b,content->offer.images[i].format))return DV_E_FORMATETC;h=global_copy(b.data(),b.size());break;}}
            if(!h)return DV_E_FORMATETC;medium->tymed=TYMED_HGLOBAL;medium->hGlobal=h;return S_OK;
        }catch(...){return STG_E_READFAULT;}
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,STGMEDIUM*)override{return DATA_E_FORMATETC;}
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,FORMATETC* out)override{if(!out)return E_POINTER;out->ptd=nullptr;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* f,STGMEDIUM* medium,BOOL release)override{
        // Acknowledge shell bookkeeping, but never honor MOVE/delete requests.
        if(!f||!medium)return E_POINTER;const auto performed=RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT),paste=RegisterClipboardFormatW(CFSTR_PASTESUCCEEDED);
        if((f->cfFormat!=performed&&f->cfFormat!=paste)||medium->tymed!=TYMED_HGLOBAL)return E_NOTIMPL;if(release)ReleaseStgMedium(medium);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,IEnumFORMATETC** out)override{if(direction!=DATADIR_GET)return E_NOTIMPL;try{auto f=formats();return SHCreateStdEnumFmtEtc(UINT(f.size()),f.data(),out);}catch(...){return E_OUTOFMEMORY;}}
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*)override{return OLE_E_ADVISENOTSUPPORTED;}HRESULT STDMETHODCALLTYPE DUnadvise(DWORD)override{return OLE_E_ADVISENOTSUPPORTED;}HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**)override{return OLE_E_ADVISENOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL b)override{asynchronous=b?VARIANT_TRUE:FALSE;return S_OK;}HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* b)override{if(!b)return E_POINTER;*b=asynchronous;return S_OK;}
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*)override{++operations;if(activity)++*activity;return S_OK;}HRESULT STDMETHODCALLTYPE InOperation(BOOL* b)override{if(!b)return E_POINTER;*b=operations?VARIANT_TRUE:FALSE;return S_OK;}HRESULT STDMETHODCALLTYPE EndOperation(HRESULT,IBindCtx*,DWORD)override{auto count=operations.load();while(count&&!operations.compare_exchange_weak(count,count-1)){}if(count&&activity)--*activity;return S_OK;}
};
}
IDataObject* clipboard_data_object(Clipboard c){if(!c||!valid_clipboard_offer(c->offer))throw std::runtime_error("Invalid native clipboard offer");return new DataObject(std::move(c));}
Clipboard capture_clipboard(HWND owner){
    if(!OpenClipboard(owner))throw std::runtime_error("Clipboard busy");
    struct Close{bool open=true;~Close(){if(open)CloseClipboard();}}close;
    auto content=std::make_shared<ClipboardContent>(*text_clipboard(L""));auto blobs=std::make_shared<std::vector<Bytes>>();std::vector<std::filesystem::path> paths;
    if(IsClipboardFormatAvailable(CF_UNICODETEXT)){auto b=clipboard_bytes(CF_UNICODETEXT,1024*1024+2);if(!b.empty()){const auto p=reinterpret_cast<const wchar_t*>(b.data());const auto count=b.size()/2;size_t n=0;while(n<count&&p[n])++n;if(n==count)throw std::runtime_error("Clipboard text is not terminated");content->offer.text.assign(p,n);}}
    for(auto format:{ClipboardFormat::DibV5,ClipboardFormat::Dib,ClipboardFormat::Png}){
        if(format==ClipboardFormat::Dib&&!content->offer.images.empty())continue;if(!IsClipboardFormatAvailable(native_format(format)))continue;auto b=clipboard_bytes(native_format(format),ClipboardImageLimit);if(b.empty()||!bitmap_bytes(b,format))continue;
        content->offer.images.push_back({format,b.size()});blobs->push_back(std::move(b));
    }
    if(IsClipboardFormatAvailable(CF_HDROP)){
        const auto drop=static_cast<HDROP>(GetClipboardData(CF_HDROP));if(!drop)throw std::runtime_error("Clipboard file list unavailable");const auto count=DragQueryFileW(drop,UINT_MAX,nullptr,0);if(count>4096)throw std::runtime_error("Clipboard file count exceeds 4096");
        for(UINT i=0;i<count;++i){const auto n=DragQueryFileW(drop,i,nullptr,0);if(!n||n>32767)throw std::runtime_error("Invalid clipboard path");std::wstring path(n+1,L'\0');if(DragQueryFileW(drop,i,path.data(),n+1)!=n)throw std::runtime_error("Invalid clipboard path");path.resize(n);paths.emplace_back(path);}
    }
    CloseClipboard();close.open=false;
    auto files=std::make_shared<std::vector<LocalFile>>();
    auto add=[&](const std::filesystem::path& path,const std::wstring& name){
        if(content->offer.files.size()>=4096)throw std::runtime_error("Clipboard file count exceeds 4096");
        Handle file{CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
        if(file.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Clipboard source unavailable");const auto info=file_info(file.h);const bool dir=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        content->offer.files.push_back({name,dir?0:(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,time_value(info.ftLastWriteTime),dir});files->push_back({path,info});
    };
    for(const auto& path:paths){const auto name=path.filename().wstring();add(path,name);if(content->offer.files.back().directory){for(const auto& entry:std::filesystem::recursive_directory_iterator(path))add(entry.path(),name+L"\\"+entry.path().lexically_relative(path).wstring());}}
    if(!valid_clipboard_offer(content->offer))throw std::runtime_error("Unsupported clipboard names, format or size");
    content->read=[blobs,files](uint32_t item,uint64_t offset,uint32_t count){
        if(count>ClipboardChunk)throw std::runtime_error("Clipboard chunk too large");if(item<blobs->size()){const auto& b=(*blobs)[item];if(offset>b.size()||count>b.size()-offset)throw std::runtime_error("Clipboard image range");return Bytes(b.begin()+size_t(offset),b.begin()+size_t(offset)+count);}
        item-=uint32_t(blobs->size());if(item>=files->size())throw std::runtime_error("Clipboard file index");const auto& source=(*files)[item];
        Handle file{CreateFileW(source.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
        if(file.h==INVALID_HANDLE_VALUE||!same_file(file_info(file.h),source.identity))throw std::runtime_error("Clipboard source changed or is busy");
        const auto size=(uint64_t(source.identity.nFileSizeHigh)<<32)|source.identity.nFileSizeLow;if(offset>size||count>size-offset)throw std::runtime_error("Clipboard file range");
        LARGE_INTEGER position{};position.QuadPart=offset;if(!SetFilePointerEx(file.h,position,nullptr,FILE_BEGIN))throw std::runtime_error("Clipboard seek failed");Bytes b(count);DWORD read=0;if(!ReadFile(file.h,b.data(),count,&read,nullptr)||read!=count)throw std::runtime_error("Clipboard file read failed");return b;
    };return content;
}

struct ClipboardDesktop::Impl {
    std::thread thread;mutable std::mutex mutex;std::condition_variable ready;HWND window=nullptr;bool started=false,stopped=false;std::string error;
    Clipboard cache=text_clipboard(L"");std::function<void(Clipboard)> changed;IDataObject* object=nullptr;std::atomic<DWORD> sequence{0};bool initial=true;
    struct Job {Clipboard content;HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);std::exception_ptr failure;~Job(){if(done)CloseHandle(done);}};std::map<uint64_t,std::shared_ptr<Job>> jobs;uint64_t next_job=0;std::shared_ptr<std::atomic<unsigned>> activity=std::make_shared<std::atomic<unsigned>>(0);
    void capture(){
        if(object&&OleIsCurrentClipboard(object)==S_OK){sequence=GetClipboardSequenceNumber();ready.notify_all();return;}
        const auto before=GetClipboardSequenceNumber();if(before==sequence)return;
        try{auto c=capture_clipboard(window);if(before!=GetClipboardSequenceNumber()){SetTimer(window,1,50,nullptr);return;}
            {std::lock_guard lock(mutex);cache=c;}sequence=before;ready.notify_all();if(changed&&!initial)changed(c);initial=false;
        }catch(const std::exception& e){
            if(std::string_view(e.what())=="Clipboard busy"){SetTimer(window,1,100,nullptr);return;}
            log(std::string("Clipboard capture failed: ")+(dynamic_cast<const std::filesystem::filesystem_error*>(&e)?"Source filesystem unavailable":e.what()));auto empty=text_clipboard(L"");
            {std::lock_guard lock(mutex);cache=empty;}sequence=before;ready.notify_all();if(changed&&!initial)changed(empty);initial=false;
        }
    }
    static LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM w,LPARAM l){
        auto self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(hwnd,message,w,l);
        if(message==WM_CLIPBOARDUPDATE||message==WM_APP+1){SetTimer(hwnd,1,1,nullptr);return 0;}
        if(message==WM_TIMER){KillTimer(hwnd,1);self->capture();return 0;}
        if(message==WM_APP+2){std::shared_ptr<Job> job;{std::lock_guard lock(self->mutex);auto it=self->jobs.find(w);if(it!=self->jobs.end()){job=it->second;self->jobs.erase(it);}}
            if(job)try{auto object=new DataObject(job->content,self->activity);const auto hr=OleSetClipboard(object);if(FAILED(hr)){object->Release();throw std::runtime_error("Clipboard busy; handoff deferred");}if(self->object)self->object->Release();self->object=object;
                {std::lock_guard lock(self->mutex);self->cache=job->content;}self->sequence=GetClipboardSequenceNumber();self->ready.notify_all();self->initial=false;SetEvent(job->done);
            }catch(...){job->failure=std::current_exception();SetEvent(job->done);}return 0;}
        if(message==WM_CLOSE){DestroyWindow(hwnd);return 0;}if(message==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(hwnd,message,w,l);
    }
};
ClipboardDesktop::ClipboardDesktop():impl_(std::make_unique<Impl>()){}
ClipboardDesktop::~ClipboardDesktop(){stop();}
bool ClipboardDesktop::start(std::function<void(Clipboard)> changed,std::string& error){
    auto& p=*impl_;p.changed=std::move(changed);p.thread=std::thread([&p]{SetThreadDescription(GetCurrentThread(),L"HiveDesk clipboard STA");const auto hr=OleInitialize(nullptr);
        WNDCLASSW cls{};cls.lpfnWndProc=Impl::proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"HiveDesk.Clipboard";RegisterClassW(&cls);
        HWND window=SUCCEEDED(hr)?CreateWindowW(cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,&p):nullptr;
        const bool listener=window&&AddClipboardFormatListener(window);
        {std::lock_guard lock(p.mutex);p.window=window;p.started=true;if(!listener)p.error="Could not initialize native clipboard listener";}p.ready.notify_all();
        if(listener){SetTimer(window,1,1,nullptr);MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}RemoveClipboardFormatListener(window);}
        if(p.object){if(OleIsCurrentClipboard(p.object)==S_OK){if(p.cache->offer.images.empty()&&p.cache->offer.files.empty())OleFlushClipboard();else OleSetClipboard(nullptr);}p.object->Release();p.object=nullptr;}if(SUCCEEDED(hr))OleUninitialize();
    });std::unique_lock lock(p.mutex);p.ready.wait(lock,[&]{return p.started;});error=p.error;return error.empty();
}
void ClipboardDesktop::stop(){auto& p=*impl_;{std::lock_guard lock(p.mutex);if(p.stopped)return;p.stopped=true;if(p.window)PostMessageW(p.window,WM_CLOSE,0,0);}p.ready.notify_all();if(p.thread.joinable())p.thread.join();}
bool ClipboardDesktop::busy()const{std::lock_guard lock(impl_->mutex);return *impl_->activity!=0||!impl_->jobs.empty();}
Clipboard ClipboardDesktop::cached()const{
    auto& p=*impl_;std::unique_lock lock(p.mutex);
    if(p.window&&!p.stopped&&p.sequence!=GetClipboardSequenceNumber()){
        PostMessageW(p.window,WM_APP+1,0,0);
        if(!p.ready.wait_for(lock,std::chrono::seconds(2),[&]{return p.stopped||p.sequence==GetClipboardSequenceNumber();}))throw std::runtime_error("Clipboard capture is still pending; stale handoff refused");
    }return p.cache;
}
void ClipboardDesktop::refresh(){std::lock_guard lock(impl_->mutex);if(impl_->window&&!impl_->stopped)PostMessageW(impl_->window,WM_APP+1,0,0);}
void ClipboardDesktop::apply(Clipboard c){
    if(!c||!valid_clipboard_offer(c->offer))throw std::runtime_error("Invalid clipboard content");auto job=std::make_shared<Impl::Job>();job->content=std::move(c);if(!job->done)throw std::runtime_error("Clipboard handoff event unavailable");uint64_t token;
    {std::lock_guard lock(impl_->mutex);if(!impl_->window||impl_->stopped)throw std::runtime_error("Clipboard service stopped");token=++impl_->next_job;impl_->jobs[token]=job;if(!PostMessageW(impl_->window,WM_APP+2,WPARAM(token),0)){impl_->jobs.erase(token);throw std::runtime_error("Clipboard handoff failed");}}
    DWORD index=0;const auto waited=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,2000,1,&job->done,&index);
    if(waited==CO_E_NOTINITIALIZED)WaitForSingleObject(job->done,2000);
    if(WaitForSingleObject(job->done,0)!=WAIT_OBJECT_0){std::lock_guard lock(impl_->mutex);impl_->jobs.erase(token);throw std::runtime_error("Clipboard handoff timed out");}
    if(job->failure)std::rethrow_exception(job->failure);
}
}
