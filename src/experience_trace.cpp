#include "hivedesk/experience_trace.h"
#include "hivedesk/config.h"
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <vector>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

namespace hive {
namespace {
std::atomic<bool> enabled{false};
struct Row { uint64_t qpc=0,session=0,sequence=0;ExperienceEvent event{};std::array<uint64_t,10> values{}; };
class Trace {
    static constexpr size_t capacity=8192,limit=64*1024*1024;
    std::array<Row,capacity> rows_{};
    size_t first_=0,count_=0;
    std::mutex mutex_;std::condition_variable changed_;bool stopped_=false;
    std::atomic<uint64_t> lost_{0};std::thread writer_;
    void write() noexcept {
        try {
            std::ofstream file;size_t bytes=0;std::array<Row,512> batch{};
            const auto directory=data_directory();
            std::filesystem::create_directories(directory);
            std::vector<std::filesystem::directory_entry> old;
            for(const auto& entry:std::filesystem::directory_iterator(directory)){
                const auto name=entry.path().filename().wstring();
                if(name.starts_with(L"experience-")&&name.ends_with(L".csv"))old.push_back(entry);
            }
            std::sort(old.begin(),old.end(),[](const auto& a,const auto& b){return a.last_write_time()>b.last_write_time();});
            for(size_t i=4;i<old.size();++i){std::error_code ec;std::filesystem::remove(old[i].path(),ec);}
            const auto path=directory/(L"experience-"+std::to_wstring(GetCurrentProcessId())+L".csv");
            const auto previous=directory/(L"experience-"+std::to_wstring(GetCurrentProcessId())+L".previous.csv");
            auto append=[&](const Row& row){
                file<<GetCurrentProcessId()<<','<<row.qpc<<','<<uint32_t(row.event)<<','<<row.session<<','<<row.sequence;
                for(auto value:row.values)file<<','<<value;
                file<<'\n';
            };
            for(;;){
                size_t count=0;
                {std::unique_lock lock(mutex_);changed_.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopped_||count_>=512;});
                    while(count<batch.size()&&count_){batch[count++]=rows_[first_];first_=(first_+1)%capacity;--count_;}
                    if(!count&&stopped_)break;}
                if(!count)continue;
                if(!file.is_open()||bytes>=limit){
                    if(file.is_open()){file.close();std::error_code ec;std::filesystem::remove(previous,ec);ec.clear();std::filesystem::rename(path,previous,ec);}
                    std::filesystem::create_directories(directory);
                    file.open(path,std::ios::trunc);if(!file)break;
                    file<<"pid,qpc,event,session,sequence,v0,v1,v2,v3,v4,v5,v6,v7,v8,v9\n";
                    LARGE_INTEGER frequency{},qpc{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&qpc);
                    FILETIME utc{};GetSystemTimePreciseAsFileTime(&utc);
                    Row anchor;anchor.event=ExperienceEvent::Clock;anchor.qpc=qpc.QuadPart;
                    anchor.values[0]=frequency.QuadPart;anchor.values[1]=(uint64_t(utc.dwHighDateTime)<<32)|utc.dwLowDateTime;append(anchor);
                }
                for(size_t i=0;i<count;++i)append(batch[i]);
                if(const auto lost=lost_.exchange(0)){Row row;row.event=ExperienceEvent::Lost;row.qpc=batch[count-1].qpc;row.values[0]=lost;append(row);}
                file.flush();if(!file)break;bytes=size_t(file.tellp());
            }
        }catch(...){/* Instrumentation cannot stop a remote session. */}
    }
public:
    Trace():writer_([this]{write();}){}
    ~Trace(){ {std::lock_guard lock(mutex_);stopped_=true;}changed_.notify_all();writer_.join(); }
    void record(Row row) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock||count_==capacity){++lost_;return;}
        rows_[(first_+count_)%capacity]=row;++count_;
        if(count_>=512)changed_.notify_one();
    }
};
Trace& trace(){static Trace value;return value;}
}
void experience_enabled(bool value){if(value)(void)trace();enabled=value;}
bool experience_enabled(){return enabled.load(std::memory_order_relaxed);}
void experience_trace(ExperienceEvent event,uint64_t session,uint64_t sequence,std::array<uint64_t,10> values) noexcept {
    if(!experience_enabled())return;
    LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
    trace().record({uint64_t(qpc.QuadPart),session,sequence,event,values});
}
}
