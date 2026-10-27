#include "../src/broker_status.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>

using namespace hive;
using namespace std::chrono_literals;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
broker::Bytes report(uint32_t value){broker::Bytes bytes;broker::u32(bytes,value);return bytes;}
uint32_t value(const broker::Bytes& bytes){broker::Reader reader{bytes};return reader.u32();}

void latest_only_check(){
    broker::StatusMailbox mailbox;
    std::promise<void> entered,release;auto arrived=entered.get_future();auto permission=release.get_future().share();
    std::vector<std::pair<broker::Kind,uint32_t>> delivered;
    require(mailbox.publish(broker::Diagnostics,report(1)),"Initial report rejected");
    auto consumer=std::async(std::launch::async,[&]{return mailbox.run([&](broker::Kind kind,const broker::Bytes& bytes){
        delivered.emplace_back(kind,value(bytes));
        if(delivered.size()==1){entered.set_value();permission.wait();}
        if(delivered.size()==3)mailbox.stop();
        return true;
    });});
    const bool blocked=arrived.wait_for(1s)==std::future_status::ready;
    auto publisher=std::async(std::launch::async,[&]{
        for(uint32_t i=2;i<=10000;++i){
            if(!mailbox.publish(broker::Health,report(i))||!mailbox.publish(broker::Diagnostics,report(i)))return false;
        }
        return true;
    });
    const bool independent=publisher.wait_for(500ms)==std::future_status::ready;
    // Release before asserting so a failing test never leaves a blocked future.
    release.set_value();const bool published=publisher.get();
    if(!independent)mailbox.stop();
    const bool done=consumer.wait_for(1s)==std::future_status::ready;
    if(!done)mailbox.stop();const bool drained=consumer.get();
    require(blocked&&independent&&published&&done&&drained,"Blocked status consumer stalled the producer");
    require(delivered.size()==3&&delivered[0]==std::pair{broker::Diagnostics,1u}&&
        delivered[1].second==10000&&delivered[2].second==10000&&delivered[1].first!=delivered[2].first,
        "Status reports became a FIFO or lost one report kind");
    require(!mailbox.publish(broker::Health,report(10001)),"Stopped mailbox admitted a report");
}

void stop_and_bounds_check(){
    broker::StatusMailbox mailbox;
    require(!mailbox.publish(broker::Frame,report(1))&&!mailbox.publish(broker::Diagnostics,broker::Bytes(513)),
        "Status mailbox admitted video or an unbounded payload");
    auto consumer=std::async(std::launch::async,[&]{return mailbox.run([](broker::Kind,const broker::Bytes&){return true;});});
    const bool waited=consumer.wait_for(30ms)==std::future_status::timeout;
    mailbox.stop();const bool woke=consumer.wait_for(500ms)==std::future_status::ready;
    require(waited&&woke&&consumer.get(),"Empty mailbox did not wake on stop");
    broker::StatusMailbox pending;
    pending.publish(broker::Health,report(1));pending.publish(broker::Diagnostics,report(1));pending.stop();
    unsigned calls=0;require(pending.run([&](broker::Kind,const broker::Bytes&){++calls;return true;})&&calls==0,
        "Stop replayed stale pending status");
    broker::StatusMailbox broken;broken.publish(broker::Health,report(1));
    require(!broken.run([](broker::Kind,const broker::Bytes&){return false;})&&!broken.publish(broker::Health,report(2)),
        "Failed writer left the mailbox accepting reports");
}

struct Pipe {
    HANDLE server=INVALID_HANDLE_VALUE,client=INVALID_HANDLE_VALUE;
    Pipe(){
        static unsigned sequence=0;
        const auto name=L"\\\\.\\pipe\\HiveDesk.StatusTest."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++sequence);
        server=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr);
        require(server!=INVALID_HANDLE_VALUE,"Create status test pipe");
        client=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        require(client!=INVALID_HANDLE_VALUE,"Open status test pipe");
        OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        const bool connected=ConnectNamedPipe(server,&operation)||GetLastError()==ERROR_PIPE_CONNECTED;
        CloseHandle(operation.hEvent);require(connected,"Connect status test pipe");
    }
    ~Pipe(){if(client!=INVALID_HANDLE_VALUE)CloseHandle(client);if(server!=INVALID_HANDLE_VALUE)CloseHandle(server);}
};

void pipe_cancellation_check(bool blocked_on_video){
    Pipe pipe;std::atomic<bool> stopped=false;broker::StatusMailbox mailbox;std::mutex writes;
    std::future<bool> video;
    if(blocked_on_video){
        video=std::async(std::launch::async,[&]{std::lock_guard lock(writes);return broker::send(pipe.server,broker::Frame,broker::Bytes(1024*1024,1),&stopped);});
        const bool blocked=video.wait_for(30ms)==std::future_status::timeout;
        if(!blocked){stopped=true;CancelIoEx(pipe.server,nullptr);video.get();require(false,"Video pipe did not block");}
    }
    mailbox.publish(broker::Diagnostics,report(1));
    std::promise<void> entered;auto arrived=entered.get_future();
    auto consumer=std::async(std::launch::async,[&]{return mailbox.run([&](broker::Kind kind,const broker::Bytes& bytes){
        entered.set_value();std::lock_guard lock(writes);
        if(stopped)return false;
        // Force a full pipe deterministically. This tests cancellation of the
        // same broker send used in production, not payload size admission.
        return broker::send(pipe.server,kind,blocked_on_video?bytes:broker::Bytes(1024*1024,1),&stopped);
    });});
    const bool entered_writer=arrived.wait_for(1s)==std::future_status::ready;
    const bool blocked=consumer.wait_for(30ms)==std::future_status::timeout;
    auto publisher=std::async(std::launch::async,[&]{return mailbox.publish(broker::Health,report(2))&&mailbox.publish(broker::Diagnostics,report(2));});
    const bool independent=publisher.wait_for(500ms)==std::future_status::ready;
    stopped=true;mailbox.stop();CancelIoEx(pipe.server,nullptr);
    const bool completed=consumer.wait_for(1s)==std::future_status::ready;
    if(!completed){DisconnectNamedPipe(pipe.server);CancelIoEx(pipe.server,nullptr);}
    const bool cancelled=!consumer.get();const bool published=publisher.get();
    const bool video_cancelled=!video.valid()||!video.get();
    require(entered_writer&&blocked&&independent&&published&&completed&&cancelled&&video_cancelled,
        "Blocked pipe/video mutex stalled publication or teardown");
}
}
int main(){try{
    latest_only_check();stop_and_bounds_check();pipe_cancellation_check(false);pipe_cancellation_check(true);
    std::puts("PASS broker status: bounded latest-only reports, independent publication under blocked writes, failed-writer closure, empty/pending stop and real pipe/video-mutex cancellation. No desktop interaction.");return 0;
}catch(const std::exception& error){std::printf("FAIL broker status: %s\n",error.what());return 1;}}
