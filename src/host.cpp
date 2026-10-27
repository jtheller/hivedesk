#include "broker_wire.h"
#include "broker_input.h"
#include "broker_status.h"
#include "rate_wire.h"
#include "hivedesk/media.h"
#include "hivedesk/config.h"
#include "hivedesk/shell.h"
#include "hivedesk/desktop_session.h"
#include "hivedesk/gpu_policy_restart.h"
#include <sddl.h>
#include <wtsapi32.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace {
using namespace hive;
std::atomic<bool> parent_gone{false};std::atomic<HANDLE> active_pipe{INVALID_HANDLE_VALUE};
struct ParentWatch {HANDLE process,thread;};
DWORD WINAPI watch_parent(void* argument){
    auto* watch=static_cast<ParentWatch*>(argument);WaitForSingleObject(watch->process,INFINITE);CloseHandle(watch->process);parent_gone=true;
    for(;;){const auto pipe=active_pipe.load();if(pipe!=INVALID_HANDLE_VALUE)CancelIoEx(pipe,nullptr);CancelSynchronousIo(watch->thread);Sleep(50);}
}
bool local_system(){HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;BYTE user[512]{},system[SECURITY_MAX_SID_SIZE]{};DWORD bytes=0,size=sizeof(system);bool ok=GetTokenInformation(token,TokenUser,user,sizeof(user),&bytes)&&CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&size)&&EqualSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid,system);CloseHandle(token);return ok;}
std::wstring user_sid(DWORD session){
    HANDLE token{};if(!WTSQueryUserToken(session,&token))throw std::runtime_error("No signed-in console user");
    BYTE user[4096]{};DWORD bytes=0;LPWSTR sid=nullptr;
    bool ok=GetTokenInformation(token,TokenUser,user,sizeof(user),&bytes)&&ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user)->User.Sid,&sid);CloseHandle(token);
    if(!ok)throw std::runtime_error("Read session identity");std::wstring result=sid;LocalFree(sid);return result;
}
bool trusted_client(HANDLE pipe,DWORD session){
    ULONG pid=0,client_session=0;if(!GetNamedPipeClientProcessId(pipe,&pid)||!GetNamedPipeClientSessionId(pipe,&client_session)||client_session!=session)return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return false;
    wchar_t path[32768]{};DWORD size=32768;const bool ok=QueryFullProcessImageNameW(process,0,path,&size)!=FALSE;CloseHandle(process);
    return ok&&CompareStringOrdinal(path,-1,(broker::module_directory()/L"HiveDesk.exe").c_str(),-1,TRUE)==CSTR_EQUAL;
}
struct Session {
    HANDLE pipe;MediaHost media;std::mutex write_mutex,media_mutex;
    std::atomic<bool> stopping{false},credit{true};bool active=false;StreamSettings settings{};std::wstring captured_desktop;
    HANDLE input_pipe=INVALID_HANDLE_VALUE;std::thread input_reader;
    std::atomic<bool> input_stopping{false};std::atomic<uint64_t> input_session{0};
    broker::StatusMailbox status_mailbox;std::thread status_writer;
    Session(HANDLE connection,DWORD session,SECURITY_ATTRIBUTES* security):pipe(connection){
        input_pipe=CreateNamedPipeW(broker::input_pipe_name(session).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,security);
        if(input_pipe==INVALID_HANDLE_VALUE)throw std::runtime_error("Create dedicated input pipe: "+std::to_string(GetLastError()));
        ULONG client_pid=0;
        if(!GetNamedPipeClientProcessId(pipe,&client_pid)){CloseHandle(input_pipe);input_pipe=INVALID_HANDLE_VALUE;throw std::runtime_error("Read desktop client identity");}
        input_reader=std::thread([this,client_pid]{
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
            OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(!operation.hEvent)return;
            bool connected=ConnectNamedPipe(input_pipe,&operation)!=FALSE;
            if(!connected){const auto error=GetLastError();connected=error==ERROR_PIPE_CONNECTED;
                if(error==ERROR_IO_PENDING){
                    while(WaitForSingleObject(operation.hEvent,100)==WAIT_TIMEOUT&&!input_stopping&&!parent_gone){}
                    if(input_stopping||parent_gone)CancelIoEx(input_pipe,&operation);
                    DWORD count=0;connected=GetOverlappedResult(input_pipe,&operation,&count,TRUE)!=FALSE;
                }
            }
            CloseHandle(operation.hEvent);
            if(connected&&!input_stopping&&!parent_gone){
                InputDesktop desktop(DesktopUse::Input);bool first_input=true;
                broker::serve_input(input_pipe,client_pid,input_stopping,
                    [this](uint64_t session){return !input_stopping&&!parent_gone&&input_session.load()==session;},
                    [&](const InputPacket& packet){
                        const auto fresh=[&]{const auto now=input_now_us();return !input_stopping&&!parent_gone&&input_session.load()==packet.session&&
                            packet.received_time_us&&now>=packet.received_time_us&&now-packet.received_time_us<=broker::InputMaxQueueUs;};
                        return broker::retry_desktop_input([&]{
                            if(desktop.attach())first_input=true;
                            if(first_input){inject_input({InputKind::ReleaseAll});first_input=false;}
                            // Attachment/release can stall. Recheck authority
                            // and original age immediately before injection.
                            if(!fresh())return false;
                            inject_input(packet.event);return true;
                        },fresh,[]{Sleep(1);});
                    },[&]{
                        try{desktop.attach();}catch(...){}
                        release_injected_input();
                    },[&](const std::string& error){
                        auto message=error+" [desktop="+narrow(desktop_name(GetThreadDesktop(GetCurrentThreadId())))+"]";
                        if(message.size()>broker::InputMaxBytes)message.resize(broker::InputMaxBytes);
                        // Use the independent input pipe, so a blocked video
                        // write cannot hide the rejection. No key/PIN content.
                        broker::send_input_message(input_pipe,broker::Notice,broker::Bytes(message.begin(),message.end()),&input_stopping);
                    });
            }
            // Closing the dedicated pipe makes an input failure visible to the
            // tray process without waiting for media work or its large writes.
            DisconnectNamedPipe(input_pipe);
        });
    }
    ~Session(){stop_status();stop_input();media.stop();}
    void stop_status(){
        // Cancel before joining: the status writer can be waiting for either
        // the pipe itself or a video write that owns the shared write mutex.
        stopping=true;status_mailbox.stop();CancelIoEx(pipe,nullptr);
        if(status_writer.joinable())status_writer.join();
    }
    void stop_input(){
        input_session=0;input_stopping=true;
        if(input_pipe!=INVALID_HANDLE_VALUE)CancelIoEx(input_pipe,nullptr);
        if(input_reader.joinable())input_reader.join();
        if(input_pipe!=INVALID_HANDLE_VALUE){CloseHandle(input_pipe);input_pipe=INVALID_HANDLE_VALUE;}
    }
    bool send(broker::Kind kind,const broker::Bytes& data={}){std::lock_guard lock(write_mutex);return !stopping&&broker::send(pipe,kind,data,&stopping);}
    void notice(const std::string& text){send(broker::Notice,broker::Bytes(text.begin(),text.end()));}
    bool start_media(std::string& error){
        captured_desktop=input_desktop_name();credit=true;
        return media.start(settings,[this](EncodedFrame frame){
            const auto b=broker::write_frame(frame);
            credit=false;if(!send(broker::Frame,b))throw std::runtime_error("Local viewer pipe closed");
        },error,[this]{return !stopping&&credit.load();},[this](const StreamHealth& h){broker::Bytes b;broker::u64(b,h.session);broker::u32(b,h.target_millihz);broker::u32(b,h.rate_millihz);broker::u32(b,h.pressure);status_mailbox.publish(broker::Health,std::move(b));},[this](const SourceDiagnostics& d){broker::Bytes b;broker::u64(b,d.session);broker::u32(b,d.encoded_millihz);broker::u32(b,d.encode_us);broker::u32(b,d.capture_wait_us);broker::u32(b,d.send_wait_us_per_second);broker::u32(b,d.feedback_age_us);broker::u32(b,d.network_extra_us);broker::u32(b,d.color_us);broker::u32(b,d.nvenc_us);broker::u32(b,d.encode_age_us);if(d.rate_details)rate_wire::write_diagnostics(d,[&](uint32_t n){broker::u32(b,n);},[&](uint64_t n){broker::u64(b,n);});status_mailbox.publish(broker::Diagnostics,std::move(b));});
    }
    void run(){
        status_writer=std::thread([this]{
            bool delivered=false;
            try{delivered=status_mailbox.run([this](broker::Kind kind,const broker::Bytes& bytes){return send(kind,bytes);});}
            catch(...){status_mailbox.stop();}
            if(!delivered){stopping=true;CancelIoEx(pipe,nullptr);}
        });
        std::thread monitor([this]{
            std::string previous_error;
            while(!stopping){
                Sleep(100);if(stopping)break;
                std::lock_guard lock(media_mutex);
                const auto desktop=input_desktop_name();
                if(active&&(captured_desktop!=desktop||!media.running())){
                    const auto previous_desktop=captured_desktop;
                    media.stop();if(stopping)break;std::string error;start_media(error);
                    if(!error.empty()&&error!=previous_error)notice("Capture restart: "+error);
                    if(error.empty())notice("Capture resumed: "+narrow(previous_desktop)+" -> "+narrow(captured_desktop)+"; preserving stream frame sequence.");previous_error=error;
                    if(!error.empty())Sleep(150);
                }
            }
        });
        broker::Kind kind;broker::Bytes b;
        while(!parent_gone&&!stopping&&broker::receive(pipe,kind,b,&stopping)){
            broker::Reader r{b};bool valid=true;
            if(kind==broker::Start){
                std::lock_guard lock(media_mutex);if(active)break;
                settings.fps=r.u32();settings.bitrate_mbps=r.u32();settings.receiver_millihz=r.u32();settings.session=r.u64();
                if(!r.end()||(settings.session?(settings.receiver_millihz<1000||settings.receiver_millihz>2000000):(!settings.bitrate_mbps||settings.bitrate_mbps>4000||settings.fps>2000)))break;
                // Publish input authority before capture initialization. Later
                // desktop restarts and feedback cannot obstruct this owner.
                input_session=settings.session?settings.session:UINT64_MAX;
                std::string error;active=start_media(error);
                if(!active){send(broker::Failure,broker::Bytes(error.begin(),error.end()));break;}
                if(!send(broker::Started))break;
            }else if(kind==broker::Ready){if(!b.empty()||!active)break;credit=true;media.notify_ready();}
            else if(kind==broker::Keyframe){if(!b.empty()||!active)break;media.request_keyframe();}
            else if(kind==broker::Feedback){
                StreamFeedback f;f.session=r.u64();f.capture_time_us=r.u64();f.arrival_age_us=r.u32();f.service_us=r.u32();f.queue_us=r.u32();const auto recover=r.u32();f.recover=recover!=0;
                if(!r.end()&&!rate_wire::read_feedback(f,[&]{return r.u32();},[&]{return r.u64();}))break;
                if(!active||!r.end()||recover>1||!valid_feedback(f))break;media.feedback(f);
            }else valid=false;
            if(!valid)break;
        }
        stop_status();stop_input();monitor.join();media.stop();
    }
};
}
int wmain(int argc,wchar_t** argv){
    if(!local_system())return ERROR_ACCESS_DENIED;
    if(argc!=3||wcscmp(argv[1],L"--parent"))return ERROR_INVALID_PARAMETER;
    const auto parent=wcstoul(argv[2],nullptr,10);if(!parent)return ERROR_INVALID_PARAMETER;
    auto* watch=new ParentWatch{OpenProcess(SYNCHRONIZE,FALSE,parent),OpenThread(THREAD_TERMINATE,FALSE,GetCurrentThreadId())};
    if(!watch->process||!watch->thread)return ERROR_ACCESS_DENIED;
    HANDLE watcher=CreateThread(nullptr,0,watch_parent,watch,0,nullptr);if(!watcher)return int(GetLastError());CloseHandle(watcher);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);if(!session)return ERROR_CTX_WINSTATION_NOT_FOUND;
    try {
        const auto console_sid=user_sid(session);
        HANDLE restart=hive::gpu_policy_restart::create_broker_event(console_sid);
        if(!restart)return ERROR_ACCESS_DENIED;
        struct CloseEvent {HANDLE value;~CloseEvent(){CloseHandle(value);}}close_restart{restart};
        const auto sddl=L"O:SYG:SYD:P(A;;GA;;;SY)(A;;GRGW;;;"+console_sid+L")";
        PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return int(GetLastError());
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
        bool restart_requested=false;
        while(!parent_gone){
            // Consume requests strictly between sessions, never from the
            // capture/input threads or while a controller owns this pipe.
            if(WaitForSingleObject(restart,0)==WAIT_OBJECT_0){restart_requested=true;break;}
            HANDLE pipe=CreateNamedPipeW(broker::pipe_name(session).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,256*1024,64*1024,0,&security);
            if(pipe==INVALID_HANDLE_VALUE){LocalFree(descriptor);return int(GetLastError());}
            active_pipe=pipe;
            OVERLAPPED connect{};connect.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);bool connected=false,restarting=false;
            if(connect.hEvent&&!parent_gone){
                connected=ConnectNamedPipe(pipe,&connect)!=FALSE;
                if(!connected){const auto error=GetLastError();connected=error==ERROR_PIPE_CONNECTED;
                    if(error==ERROR_IO_PENDING){
                        HANDLE events[]={restart,connect.hEvent};
                        DWORD ready=WAIT_TIMEOUT;
                        while(!parent_gone&&(ready=WaitForMultipleObjects(2,events,FALSE,100))==WAIT_TIMEOUT){}
                        restarting=ready==WAIT_OBJECT_0;
                        if(parent_gone||restarting||ready==WAIT_FAILED)CancelIoEx(pipe,&connect);
                        DWORD bytes=0;connected=GetOverlappedResult(pipe,&connect,&bytes,TRUE)!=FALSE;
                    }
                }
            }
            if(connect.hEvent)CloseHandle(connect.hEvent);
            if(!restarting)restarting=WaitForSingleObject(restart,0)==WAIT_OBJECT_0;
            if(!parent_gone&&!restarting&&connected&&trusted_client(pipe,session)){
                Session connection{pipe,session,&security};connection.run();
            }
            active_pipe=INVALID_HANDLE_VALUE;DisconnectNamedPipe(pipe);CloseHandle(pipe);
            if(restarting){restart_requested=true;break;}
        }
        LocalFree(descriptor);return restart_requested?int(hive::gpu_policy_restart::BrokerRestartExitCode):0;
    }catch(...){return 1;}
}
