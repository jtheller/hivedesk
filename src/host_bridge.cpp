#include "hivedesk/host_bridge.h"
#include "hivedesk/config.h"
#include "broker_wire.h"
#include "broker_input.h"
#include "rate_wire.h"
#include "broker_readiness.h"
#include <aclapi.h>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
namespace hive {
namespace {
bool trusted_server(HANDLE pipe){
    PSID owner=nullptr;PSECURITY_DESCRIPTOR security=nullptr;BYTE system[SECURITY_MAX_SID_SIZE]{};DWORD size=sizeof(system),pid=0;
    bool ok=CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&size)&&GetSecurityInfo(pipe,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&security)==ERROR_SUCCESS&&EqualSid(owner,system);
    if(security)LocalFree(security);if(!ok||!GetNamedPipeServerProcessId(pipe,&pid))return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return false;
    wchar_t path[32768]{};size=32768;ok=QueryFullProcessImageNameW(process,0,path,&size)!=FALSE;CloseHandle(process);
    return ok&&CompareStringOrdinal(path,-1,(broker::module_directory()/L"HiveDeskHost.exe").c_str(),-1,TRUE)==CSTR_EQUAL;
}
}
struct HostBridge::Impl {
    HANDLE pipe=INVALID_HANDLE_VALUE;std::mutex writes;std::thread reader;std::atomic<bool> stopped{true};
    broker::ReadySignal downstream;
    std::mutex activation_mutex;std::condition_variable activation_changed;bool activated=false;
    std::atomic<HANDLE> input_pipe{INVALID_HANDLE_VALUE};std::mutex input_writes,input_lifecycle;std::thread input_reader;
    std::atomic<bool> input_stopped{true};std::atomic<uint64_t> input_session{0},legacy_sequence{0};
    bool send(broker::Kind kind,const broker::Bytes& bytes={}){std::lock_guard lock(writes);return !stopped&&broker::send(pipe,kind,bytes,&stopped);}
};
HostBridge::HostBridge():impl_(std::make_unique<Impl>()){}
HostBridge::~HostBridge(){stop();}
bool HostBridge::start(const StreamSettings& settings,std::function<void(EncodedFrame)> output,std::string& error,std::function<bool()> ready,std::function<void(const StreamHealth&)> health,std::function<void(const SourceDiagnostics&)> diagnostics,std::function<void(const InputAck&)> input_ack){
    stop();DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);const auto name=broker::pipe_name(session);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    do {
        impl_->pipe=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(impl_->pipe!=INVALID_HANDLE_VALUE)break;
        WaitNamedPipeW(name.c_str(),100);Sleep(50);
    }while(std::chrono::steady_clock::now()<deadline);
    if(impl_->pipe==INVALID_HANDLE_VALUE){error="Desktop service unavailable. Install or repair HiveDesk on the controlled computer.";return false;}
    if(!trusted_server(impl_->pipe)){CloseHandle(impl_->pipe);impl_->pipe=INVALID_HANDLE_VALUE;error="Desktop service identity check failed.";return false;}
    impl_->stopped=false;auto promise=std::make_shared<std::promise<std::string>>();auto initialized=promise->get_future();
    impl_->reader=std::thread([this,promise,output=std::move(output),ready=std::move(ready),health=std::move(health),diagnostics=std::move(diagnostics)]{
        bool announced=false;broker::Kind kind;broker::Bytes bytes;
        try {
            while(!impl_->stopped&&broker::receive(impl_->pipe,kind,bytes,&impl_->stopped)){
                broker::Reader r{bytes};
                if(kind==broker::Started){if(!bytes.empty()||announced)break;announced=true;promise->set_value({});}
                else if(kind==broker::Activated){if(!bytes.empty())break;{std::lock_guard lock(impl_->activation_mutex);impl_->activated=true;}impl_->activation_changed.notify_all();}
                else if(kind==broker::Failure){std::string message(bytes.begin(),bytes.end());if(!announced){announced=true;promise->set_value(message);}else log("Desktop service: "+message);break;}
                else if(kind==broker::Diagnostics){SourceDiagnostics d;d.session=r.u64();d.encoded_millihz=r.u32();d.encode_us=r.u32();d.capture_wait_us=r.u32();d.send_wait_us_per_second=r.u32();d.feedback_age_us=r.u32();d.network_extra_us=r.u32();d.color_us=r.u32();d.nvenc_us=r.u32();d.encode_age_us=r.u32();d.encode_details=true;if(!r.end()&&!rate_wire::read_diagnostics(d,[&]{return r.u32();},[&]{return r.u64();}))break;if(!r.end()||!valid_diagnostics(d))break;if(diagnostics)diagnostics(d);}
                else if(kind==broker::Notice){if(bytes.size()>1024)break;log("Desktop service: "+std::string(bytes.begin(),bytes.end()));}
                else if(kind==broker::Frame){
                    EncodedFrame frame;if(!broker::read_frame(bytes,frame))break;
                    output(std::move(frame));
                    if(!impl_->downstream.wait(impl_->stopped,ready))break;
                    if(!impl_->send(broker::Ready))break;
                }else if(kind==broker::Health){StreamHealth value;value.session=r.u64();value.target_millihz=r.u32();value.rate_millihz=r.u32();value.pressure=r.u32();if(!r.end())break;if(health)health(value);}
                else break;
            }
        }catch(const std::exception& e){if(!announced){announced=true;promise->set_value(e.what());}else log(std::string("Desktop service connection: ")+e.what());}
        if(!announced)promise->set_value("Desktop service closed before capture started.");
        impl_->stopped=true;
        // The input owner notices this cancellation even if media failed while
        // Start was still establishing its independent pipe. Its ACK reader
        // closes that pipe and the broker releases all held input.
        impl_->input_session=0;impl_->input_stopped=true;
    });
    broker::Bytes bytes;broker::u32(bytes,settings.fps);broker::u32(bytes,settings.bitrate_mbps);broker::u32(bytes,settings.receiver_millihz);broker::u64(bytes,settings.session);broker::u32(bytes,settings.warm?1:0);
    if(!impl_->send(broker::Start,bytes)||initialized.wait_for(std::chrono::seconds(8))!=std::future_status::ready){error="Desktop service capture startup timed out.";stop();return false;}
    error=initialized.get();if(!error.empty()){stop();return false;}
    const auto input_name=broker::input_pipe_name(session);
    const auto input_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    HANDLE input_pipe=INVALID_HANDLE_VALUE;
    do{
        input_pipe=CreateFileW(input_name.c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(input_pipe!=INVALID_HANDLE_VALUE)break;
        WaitNamedPipeW(input_name.c_str(),50);
    }while(std::chrono::steady_clock::now()<input_deadline);
    if(input_pipe==INVALID_HANDLE_VALUE||!trusted_server(input_pipe)){
        if(input_pipe!=INVALID_HANDLE_VALUE)CloseHandle(input_pipe);
        error="Desktop service input channel unavailable. Install the same HiveDesk version on this computer.";stop();return false;
    }
    impl_->input_pipe=input_pipe;impl_->input_stopped=false;impl_->input_session=settings.session?settings.session:UINT64_MAX;impl_->legacy_sequence=0;
    auto input_promise=std::make_shared<std::promise<bool>>();auto input_initialized=input_promise->get_future();
    impl_->input_reader=std::thread([this,input_promise,input_ack=std::move(input_ack)]{
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        bool announced=false;uint64_t completed=0;broker::Kind kind;broker::Bytes payload;
        try{
            while(!impl_->input_stopped&&broker::receive(impl_->input_pipe.load(),kind,payload,&impl_->input_stopped,broker::InputMaxBytes)){
                if(!announced){
                    if(kind!=broker::InputStarted||!payload.empty())break;
                    announced=true;input_promise->set_value(true);continue;
                }
                if(kind==broker::Notice){
                    if(payload.empty())break;
                    log("Desktop input: "+std::string(payload.begin(),payload.end()));continue;
                }
                InputAck ack;
                if(kind!=broker::InputComplete||!broker::read_input_ack(payload,ack)||ack.session!=impl_->input_session.load()||ack.sequence<=completed)break;
                completed=ack.sequence;
                if(input_ack)input_ack(ack);
                if(!ack.injected)break;
            }
        }catch(...){/* Closing the input gate makes callback failures fail closed. */}
        impl_->input_stopped=true;impl_->input_session=0;
        {
            const std::lock_guard writes(impl_->input_writes);
            const auto pipe=impl_->input_pipe.exchange(INVALID_HANDLE_VALUE);
            if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);
        }
        if(!announced)input_promise->set_value(false);
    });
    broker::Bytes input_begin;broker::u64(input_begin,impl_->input_session.load());
    bool input_begin_sent=false;
    {
        const std::lock_guard writes(impl_->input_writes);
        if(!impl_->stopped&&!impl_->input_stopped)input_begin_sent=broker::send_input_message(impl_->input_pipe.load(),broker::InputStart,input_begin,&impl_->input_stopped);
    }
    if(!input_begin_sent||
        input_initialized.wait_for(std::chrono::seconds(2))!=std::future_status::ready||!input_initialized.get()||impl_->input_stopped){
        error="Desktop service input channel startup failed.";stop();return false;
    }
    return true;
}
void HostBridge::stop(){
    revoke_input();
    impl_->stopped=true;
    impl_->downstream.notify();
    if(impl_->pipe!=INVALID_HANDLE_VALUE)CancelIoEx(impl_->pipe,nullptr);
    if(impl_->reader.joinable()){CancelSynchronousIo(impl_->reader.native_handle());impl_->reader.join();}
    std::lock_guard lock(impl_->writes);if(impl_->pipe!=INVALID_HANDLE_VALUE){CloseHandle(impl_->pipe);impl_->pipe=INVALID_HANDLE_VALUE;}
}
bool HostBridge::activate(){
    {std::lock_guard lock(impl_->activation_mutex);impl_->activated=false;}
    if(!impl_->send(broker::Activate))return false;
    std::unique_lock lock(impl_->activation_mutex);
    return impl_->activation_changed.wait_for(lock,std::chrono::seconds(2),[&]{return impl_->activated||impl_->stopped;})&&impl_->activated;
}
void HostBridge::notify_ready(){impl_->downstream.notify();}
void HostBridge::request_keyframe(){impl_->send(broker::Keyframe);}
void HostBridge::feedback(const StreamFeedback& report){broker::Bytes b;broker::u64(b,report.session);broker::u64(b,report.capture_time_us);broker::u32(b,report.arrival_age_us);broker::u32(b,report.service_us);broker::u32(b,report.queue_us);broker::u32(b,report.recover?1:0);if(report.rate_details)rate_wire::write_feedback(report,[&](uint32_t n){broker::u32(b,n);},[&](uint64_t n){broker::u64(b,n);});impl_->send(broker::Feedback,b);}
void HostBridge::revoke_input(){
    impl_->input_session=0;impl_->input_stopped=true;
    const std::lock_guard lifecycle(impl_->input_lifecycle);
    {
        const std::lock_guard writes(impl_->input_writes);
        const auto pipe=impl_->input_pipe.load();if(pipe!=INVALID_HANDLE_VALUE)CancelIoEx(pipe,nullptr);
    }
    if(impl_->input_reader.joinable())impl_->input_reader.join();
    const std::lock_guard writes(impl_->input_writes);
    const auto pipe=impl_->input_pipe.exchange(INVALID_HANDLE_VALUE);
    if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);
}
bool HostBridge::input(const InputPacket& packet){
    if(!packet.session||!packet.sequence||!packet.capture_time_us||!broker::valid_input(packet.event))return false;
    const std::lock_guard lock(impl_->input_writes);
    if(impl_->stopped||impl_->input_stopped||impl_->input_session.load()!=packet.session)return false;
    return broker::send_input_message(impl_->input_pipe.load(),broker::Input,broker::input_packet_bytes(packet),&impl_->input_stopped);
}
bool HostBridge::input(const InputEvent& event){
    if(!broker::valid_input(event))return false;
    const std::lock_guard lock(impl_->input_writes);
    if(impl_->stopped||impl_->input_stopped)return false;
    InputPacket packet;packet.session=impl_->input_session.load();packet.sequence=impl_->legacy_sequence.fetch_add(1)+1;
    packet.capture_time_us=packet.received_time_us=input_now_us();packet.event=event;
    return broker::send_input_message(impl_->input_pipe.load(),broker::Input,broker::input_packet_bytes(packet),&impl_->input_stopped);
}
}
