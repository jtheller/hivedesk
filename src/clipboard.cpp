#include "hivedesk/clipboard.h"
#include "hivedesk/transport.h"
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace hive { namespace {
using Bytes=std::vector<uint8_t>;
uint64_t identity(){uint64_t id=0;if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&id),sizeof(id),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0||!id)throw std::runtime_error("Clipboard identity unavailable");return id;}
void put(Bytes& b,uint64_t n,unsigned size){for(unsigned i=size;i;--i)b.push_back(uint8_t(n>>((i-1)*8)));}
struct Read {const Bytes& b;size_t at=0;bool ok=true;uint64_t get(unsigned size){uint64_t n=0;while(size--){if(at==b.size()){ok=false;return 0;}n=(n<<8)|b[at++];}return n;}
    std::wstring text(size_t limit){const auto size=get(4);if(!ok||size>limit||size>(b.size()-at)/2){ok=false;return {};}std::wstring s;for(size_t i=0;i<size;++i)s.push_back(wchar_t(get(2)));return s;}};
void text(Bytes& b,const std::wstring& s){put(b,s.size(),4);for(auto c:s)put(b,c,2);}
bool unicode(const std::wstring& s){for(size_t i=0;i<s.size();++i){auto c=uint16_t(s[i]);if(!c)return false;if(c>=0xd800&&c<=0xdbff){if(++i==s.size()||uint16_t(s[i])<0xdc00||uint16_t(s[i])>0xdfff)return false;}else if(c>=0xdc00&&c<=0xdfff)return false;}return true;}
bool filename(const std::wstring& path){
    if(path.empty()||path.size()>=MAX_PATH||!unicode(path))return false;
    size_t start=0;while(start<path.size()){
        const auto end=path.find(L'\\',start);const auto part=path.substr(start,end==std::wstring::npos?end:end-start);
        if(part.empty()||part==L"."||part==L".."||part.back()==L'.'||part.back()==L' ')return false;
        for(auto c:part)if(c<32||std::wstring_view(L"<>:\"/|?*").find(c)!=std::wstring_view::npos)return false;
        auto stem=part.substr(0,part.find(L'.'));for(auto& c:stem)c=wchar_t(towupper(c));
        if(stem==L"CON"||stem==L"PRN"||stem==L"AUX"||stem==L"NUL"||(stem.size()==4&&(stem.starts_with(L"COM")||stem.starts_with(L"LPT"))&&stem[3]>=L'0'&&stem[3]<=L'9'))return false;
        if(end==std::wstring::npos)return true;start=end+1;if(start==path.size())return false;
    }return false;
}
uint64_t item_size(const ClipboardOffer& o,uint32_t item){if(item<o.images.size())return o.images[item].size;item-=uint32_t(o.images.size());if(item>=o.files.size()||o.files[item].directory)throw std::runtime_error("Invalid clipboard item");return o.files[item].size;}
}
Clipboard text_clipboard(const std::wstring& value){auto c=std::make_shared<ClipboardContent>();c->offer.id=identity();c->offer.text=value;return c;}
bool valid_clipboard_offer(const ClipboardOffer& o){
    if(!o.id||o.text.size()>512*1024||!unicode(o.text)||o.images.size()>3||o.files.size()>4096)return false;
    std::set<uint32_t> formats;uint64_t total=0;
    for(const auto& image:o.images){const auto format=uint32_t(image.format);if(format<1||format>3||!formats.insert(format).second||!image.size||image.size>ClipboardImageLimit)return false;total+=image.size;}
    if(total>ClipboardImageLimit)return false;
    std::map<std::wstring,bool> paths;
    for(const auto& file:o.files){if(!filename(file.name)||(file.directory&&file.size)||file.size>INT64_MAX)return false;auto name=file.name;for(auto& c:name)c=wchar_t(towlower(c));if(!paths.emplace(name,file.directory).second)return false;}
    for(const auto& [name,dir]:paths){size_t at=0;while((at=name.find(L'\\',at))!=std::wstring::npos){auto parent=paths.find(name.substr(0,at));if(parent==paths.end()||!parent->second)return false;++at;}}
    return true;
}
Bytes encode_clipboard_offer(const ClipboardOffer& o){
    if(!valid_clipboard_offer(o))return {};Bytes b;put(b,o.id,8);text(b,o.text);put(b,o.images.size(),4);
    for(const auto& i:o.images){put(b,uint32_t(i.format),4);put(b,i.size,8);}put(b,o.files.size(),4);
    for(const auto& f:o.files){text(b,f.name);put(b,f.size,8);put(b,f.modified,8);put(b,f.directory,1);}return b;
}
bool decode_clipboard_offer(const Bytes& b,ClipboardOffer& out){
    if(b.size()>4*1024*1024)return false;Read r{b};ClipboardOffer o;o.id=r.get(8);o.text=r.text(512*1024);
    auto count=r.get(4);if(!r.ok||count>3)return false;for(size_t i=0;i<count;++i)o.images.push_back({ClipboardFormat(r.get(4)),r.get(8)});
    count=r.get(4);if(!r.ok||count>4096)return false;for(size_t i=0;i<count;++i){ClipboardFile f;f.name=r.text(MAX_PATH-1);f.size=r.get(8);f.modified=r.get(8);auto dir=r.get(1);if(dir>1)return false;f.directory=dir!=0;o.files.push_back(std::move(f));}
    if(!r.ok||r.at!=b.size()||!valid_clipboard_offer(o))return false;out=std::move(o);return true;
}

struct ClipboardTransfer::Impl : std::enable_shared_from_this<Impl> {
    Transport& transport;std::atomic<uint64_t> last_activity{0};std::atomic<unsigned> active_reads{0};std::mutex mutex;std::condition_variable wake;bool stopped=false;
    struct Grant {Clipboard content;uint64_t connection;};
    // Keep a small history so an Explorer operation already in progress can
    // finish after a subsequent copy. No file bytes are retained in these grants.
    std::map<std::string,std::deque<Grant>> grants;
    std::map<uint64_t,std::weak_ptr<const ClipboardContent>> known;
    struct Pending {std::string peer;uint64_t connection=0;uint32_t count=0;bool done=false,ok=false;Bytes bytes;};
    std::map<uint64_t,std::shared_ptr<Pending>> pending;
    std::deque<std::function<void()>> jobs;std::thread worker;
    explicit Impl(Transport& t):transport(t){}
    void start(){worker=std::thread([this]{SetThreadDescription(GetCurrentThread(),L"HiveDesk clipboard file reads");for(;;){std::function<void()> job;{std::unique_lock lock(mutex);wake.wait(lock,[&]{return stopped||!jobs.empty();});if(stopped)return;job=std::move(jobs.front());jobs.pop_front();}try{job();}catch(...) {}}});}
    void stop(){ {std::lock_guard lock(mutex);stopped=true;jobs.clear();}wake.notify_all();if(worker.joinable())worker.join();std::lock_guard lock(mutex);grants.clear();known.clear();}
    Bytes fetch(const std::string& peer,uint64_t connection,uint64_t offer,uint32_t item,uint64_t offset,uint32_t count){
        last_activity=GetTickCount64();auto p=std::make_shared<Pending>();p->peer=peer;p->connection=connection;p->count=count;const auto request=identity();
        {std::lock_guard lock(mutex);if(stopped||pending.size()>=32||transport.connection_token(peer)!=connection)throw std::runtime_error("Clipboard source disconnected");pending.emplace(request,p);}
        struct Remove{Impl& self;uint64_t request;~Remove(){std::lock_guard lock(self.mutex);self.pending.erase(request);}}remove{*this,request};
        Bytes b{1};put(b,request,8);put(b,offer,8);put(b,item,4);put(b,offset,8);put(b,count,4);
        if(!transport.send_clipboard_packet(peer,b))throw std::runtime_error("Clipboard transfer channel unavailable");
        std::unique_lock lock(mutex);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(!p->done&&!stopped&&transport.connection_token(peer)==connection){if(std::chrono::steady_clock::now()>=deadline)break;wake.wait_for(lock,std::chrono::milliseconds(100));}
        if(!p->done||!p->ok||stopped||transport.connection_token(peer)!=connection)throw std::runtime_error("Clipboard transfer canceled, expired or source changed");return std::move(p->bytes);
    }
};
ClipboardTransfer::ClipboardTransfer(Transport& t):impl_(std::make_shared<Impl>(t)){impl_->start();}
ClipboardTransfer::~ClipboardTransfer(){stop();}
void ClipboardTransfer::stop(){impl_->stop();}
bool ClipboardTransfer::busy()const{std::lock_guard lock(impl_->mutex);return !impl_->pending.empty()||!impl_->jobs.empty()||impl_->active_reads|| (impl_->last_activity&&GetTickCount64()-impl_->last_activity<5000);}
bool ClipboardTransfer::send(const std::string& peer,Clipboard c){
    if(!c||!valid_clipboard_offer(c->offer))return false;
    bool rich=false;for(const auto& p:impl_->transport.peers())if(p.id==peer){rich=(p.features&FeatureRichClipboard)!=0;break;}
    if(!rich)return impl_->transport.send_clipboard(peer,c->offer.text);
    {std::lock_guard lock(impl_->mutex);if(impl_->stopped)return false;auto& list=impl_->grants[peer];
        std::erase_if(list,[&](const auto& g){return g.content->offer.id==c->offer.id;});list.push_back({c,impl_->transport.connection_token(peer)});while(list.size()>8)list.pop_front();
        std::erase_if(impl_->known,[](const auto& entry){return entry.second.expired();});impl_->known[c->offer.id]=c;}
    return impl_->transport.send_clipboard_offer(peer,c->offer);
}
Clipboard ClipboardTransfer::receive(const std::string& peer,const ClipboardOffer& offer){
    if(!valid_clipboard_offer(offer))throw std::runtime_error("Invalid clipboard offer");
    {std::lock_guard lock(impl_->mutex);auto it=impl_->known.find(offer.id);if(it!=impl_->known.end())if(auto c=it->second.lock()){
        if(encode_clipboard_offer(c->offer)!=encode_clipboard_offer(offer))throw std::runtime_error("Clipboard identity mismatch");
        if(c->upstream.empty()||c->upstream!=peer||c->connection==impl_->transport.connection_token(peer))return c;}}
    const auto connection=impl_->transport.connection_token(peer);auto c=std::make_shared<ClipboardContent>();c->offer=offer;c->upstream=peer;c->connection=connection;
    c->read=[weak=std::weak_ptr<Impl>(impl_),peer,connection,offer](uint32_t item,uint64_t offset,uint32_t count){
        const auto size=item_size(offer,item);if(count>ClipboardChunk||offset>size||count>size-offset)throw std::runtime_error("Invalid clipboard range");
        auto self=weak.lock();if(!self)throw std::runtime_error("Clipboard service stopped");return self->fetch(peer,connection,offer.id,item,offset,count);
    };
    {std::lock_guard lock(impl_->mutex);std::erase_if(impl_->known,[](const auto& entry){return entry.second.expired();});impl_->known[offer.id]=c;}return c;
}
void ClipboardTransfer::packet(const std::string& peer,const Bytes& b){
    impl_->last_activity=GetTickCount64();Read r{b};const auto kind=r.get(1),request=r.get(8);if(!request||!r.ok)return;
    if(kind==2){const auto ok=r.get(1);std::lock_guard lock(impl_->mutex);auto it=impl_->pending.find(request);if(it==impl_->pending.end())return;auto& p=*it->second;
        if(p.peer!=peer||p.connection!=impl_->transport.connection_token(peer)||!r.ok||ok>1)return;
        p.ok=ok&&b.size()-r.at==p.count;p.bytes.assign(b.begin()+r.at,b.end());p.done=true;impl_->wake.notify_all();return;}
    if(kind!=1)return;const auto offer=r.get(8);const auto item=uint32_t(r.get(4));const auto offset=r.get(8);const auto count=uint32_t(r.get(4));if(!r.ok||r.at!=b.size()||count>ClipboardChunk)return;
    std::lock_guard lock(impl_->mutex);if(impl_->stopped||impl_->jobs.size()>=32)return;Clipboard source;const auto token=impl_->transport.connection_token(peer);
    auto found=impl_->grants.find(peer);if(found!=impl_->grants.end())for(const auto& grant:found->second)if(grant.connection==token&&grant.content->offer.id==offer){source=grant.content;break;}
    impl_->jobs.push_back([self=impl_.get(),peer,token,source,request,item,offset,count]{
        struct Active {Impl& p;Active(Impl& s):p(s){++p.active_reads;}~Active(){p.last_activity=GetTickCount64();--p.active_reads;}}active{*self};
        Bytes reply{2};put(reply,request,8);reply.push_back(0);
        try{if(!source||!source->read)throw std::runtime_error("Clipboard grant expired");const auto size=item_size(source->offer,item);
            if(offset>size||count>size-offset)throw std::runtime_error("Clipboard range rejected");auto data=source->read(item,offset,count);if(data.size()!=count)throw std::runtime_error("Clipboard source truncated");reply[9]=1;reply.insert(reply.end(),data.begin(),data.end());}catch(...){}
        if(self->transport.connection_token(peer)==token)self->transport.send_clipboard_packet(peer,reply);
    });impl_->wake.notify_all();
}
}
