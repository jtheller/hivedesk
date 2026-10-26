#include "../src/broker_input.h"
#include "../src/input_state.h"
#include <cstdio>
#include <future>
#include <functional>
#include <mutex>
#include <stdexcept>

using namespace hive;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Pipe {
    HANDLE server=INVALID_HANDLE_VALUE,client=INVALID_HANDLE_VALUE;
    Pipe(){
        static unsigned sequence=0;
        const auto name=L"\\\\.\\pipe\\HiveDesk.InputTest."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++sequence);
        server=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr);
        require(server!=INVALID_HANDLE_VALUE,"Create test pipe");
        client=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        require(client!=INVALID_HANDLE_VALUE,"Open test pipe");
        OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        const bool connected=ConnectNamedPipe(server,&operation)||GetLastError()==ERROR_PIPE_CONNECTED;
        CloseHandle(operation.hEvent);require(connected,"Connect test pipe");
    }
    ~Pipe(){if(client!=INVALID_HANDLE_VALUE)CloseHandle(client);if(server!=INVALID_HANDLE_VALUE)CloseHandle(server);}
    void disconnect(){if(client!=INVALID_HANDLE_VALUE){CloseHandle(client);client=INVALID_HANDLE_VALUE;}}
};
struct InputServer {
    Pipe pipe;
    std::atomic<bool> stopped=false;
    std::atomic<uint64_t> authorized;
    std::mutex state_mutex;
    InjectedInputState state;
    std::vector<INPUT> observed;
    std::future<bool> worker;
    std::atomic<unsigned> released=0;
    InputServer(uint64_t session=7,DWORD pid=GetCurrentProcessId(),std::function<void()> before={},std::function<void(const std::string&)> report={}):authorized(session){
        worker=std::async(std::launch::async,[this,pid,before=std::move(before),report=std::move(report)]{
            const bool result=broker::serve_input(pipe.server,pid,stopped,
                [&](uint64_t session){return authorized.load()==session;},
                [&](const InputPacket& packet){
                    if(before)before();
                    const std::lock_guard lock(state_mutex);
                    return state.apply(packet.event,[&](INPUT& input){observed.push_back(input);return true;});
                },[&]{
                    const std::lock_guard lock(state_mutex);
                    state.release([&](INPUT& input){observed.push_back(input);return true;});++released;
                },[&](const std::string& problem){if(report)report(problem);});
            DisconnectNamedPipe(pipe.server);return result;
        });
    }
    ~InputServer(){stop();}
    void stop(){stopped=true;CancelIoEx(pipe.server,nullptr);if(worker.valid())worker.wait();}
    bool begin(uint64_t session=7){
        broker::Bytes bytes;broker::u64(bytes,session);
        if(!broker::send_input_message(pipe.client,broker::InputStart,bytes,&stopped))return false;
        broker::Kind kind{};bytes.clear();return broker::receive(pipe.client,kind,bytes,&stopped,broker::InputMaxBytes)&&kind==broker::InputStarted&&bytes.empty();
    }
    bool send(uint64_t sequence,InputEvent event,uint64_t session=7,uint64_t received=0){
        InputPacket packet{session,sequence,sequence+1000,event,received?received:input_now_us()};
        return broker::send_input_message(pipe.client,broker::Input,broker::input_packet_bytes(packet),&stopped);
    }
    bool ack(InputAck& ack){broker::Kind kind;broker::Bytes bytes;return broker::receive(pipe.client,kind,bytes,&stopped,broker::InputMaxBytes)&&kind==broker::InputComplete&&broker::read_input_ack(bytes,ack);}
    bool held(){const std::lock_guard lock(state_mutex);return !state.empty();}
    size_t injected(){const std::lock_guard lock(state_mutex);return observed.size();}
    bool finished(){return worker.wait_for(std::chrono::seconds(1))==std::future_status::ready;}
};
void state_check(){
    InjectedInputState state;std::vector<INPUT> calls;
    auto send=[&](INPUT& input){calls.push_back(input);return true;};
    require(state.apply({InputKind::Key,0x1d,0,1},send)&&state.apply({InputKind::Button,5,0,0},send),"Hold key/button");
    require(!state.empty()&&calls.size()==2&&calls[0].ki.dwFlags==(KEYEVENTF_SCANCODE|KEYEVENTF_EXTENDEDKEY)&&
        calls[1].mi.mouseData==XBUTTON2,"Scan-code and extended button encoding");
    auto fail=[](INPUT&){return false;};
    require(!state.apply({InputKind::Key,0x1d,1,1},fail)&&!state.empty(),"Failed key-up lost held state");
    require(!state.release([&](INPUT& input){if(input.type==INPUT_KEYBOARD)return false;return send(input);})&&!state.empty(),"Partial release forgot failed key");
    const auto before=calls.size();
    require(state.release(send)&&state.empty()&&calls.size()==before+1&&calls.back().ki.dwFlags==(KEYEVENTF_SCANCODE|KEYEVENTF_EXTENDEDKEY|KEYEVENTF_KEYUP),"Failed key release did not retry");
    require(state.apply({InputKind::Key,0x1d,1,1},send)&&state.apply({InputKind::Button,5,1,0},send)&&calls.size()==before+1,"Duplicate releases re-injected input");
    require(!state.apply({InputKind::Key,0,0,0},send)&&!state.apply({InputKind::Button,6,0,0},send),"Invalid input reached fake sender");
}
void wire_check(){
    InputPacket packet{7,12,987654,{InputKind::Relative,-8,9,0},input_now_us()},decoded;
    const auto bytes=broker::input_packet_bytes(packet);
    require(broker::read_input_packet(bytes,decoded)&&decoded.session==7&&decoded.sequence==12&&decoded.capture_time_us==987654&&
        decoded.event.a==-8&&decoded.event.b==9&&decoded.received_time_us==packet.received_time_us,"Input packet wire roundtrip");
    for(size_t size=0;size<bytes.size();++size)require(!broker::read_input_packet(broker::Bytes(bytes.begin(),bytes.begin()+size),decoded),"Truncated input packet accepted");
    InputAck ack{7,12,987654,123,456,true},observed;
    const auto encoded=broker::input_ack_bytes(ack);
    require(broker::read_input_ack(encoded,observed)&&observed.sequence==12&&observed.host_queue_us==123&&observed.inject_us==456&&observed.injected,"Input completion ACK wire roundtrip");
    for(size_t size=0;size<encoded.size();++size)require(!broker::read_input_ack(broker::Bytes(encoded.begin(),encoded.begin()+size),observed),"Truncated input ACK accepted");
    {Pipe pipe;uint32_t header[]{broker::Magic,broker::Input,broker::InputMaxBytes+1};
        require(broker::transfer(pipe.client,header,sizeof(header),true,nullptr),"Write overlong input header");
        broker::Kind kind;broker::Bytes payload;
        require(!broker::receive(pipe.server,kind,payload,nullptr,broker::InputMaxBytes),"Input channel accepted a media-sized payload");}
}
void blocked_media_check(){
    Pipe media;std::atomic<bool> stopped=false;
    auto blocked=std::async(std::launch::async,[&]{return broker::send(media.server,broker::Frame,broker::Bytes(1024*1024,1),&stopped);});
    const bool waiting=blocked.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
    bool input_ok=false,held=false,cleared=false;
    {
        InputServer input;InputAck ack;
        input_ok=input.begin()&&input.send(1,{InputKind::Key,0x1e,0,0})&&input.ack(ack)&&ack.injected&&ack.sequence==1&&ack.capture_time_us==1001;
        held=input.held();
        input_ok=input_ok&&input.send(3,{InputKind::Relative,15,-7,0})&&input.ack(ack)&&ack.injected&&ack.sequence==3;
        input_ok=input_ok&&input.send(4,{InputKind::ReleaseAll})&&input.ack(ack)&&ack.injected;
        cleared=!input.held();input.stop();
    }
    const bool still_waiting=blocked.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout;
    stopped=true;CancelIoEx(media.server,nullptr);const bool cancelled=!blocked.get();
    require(waiting&&still_waiting&&cancelled&&input_ok&&held&&cleared,"Video IPC stall blocked independent input / completion ACK");
}
void completion_check(){
    std::promise<void> entered,finish;auto arrived=entered.get_future();auto permission=finish.get_future().share();
    InputServer input(7,GetCurrentProcessId(),[&]{entered.set_value();permission.wait();});
    const bool begun=input.begin()&&input.send(1,{InputKind::Key,0x1e,0,0});
    const bool reached=arrived.wait_for(std::chrono::seconds(1))==std::future_status::ready;
    InputAck ack;auto response=std::async(std::launch::async,[&]{return input.ack(ack);});
    const bool held=response.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
    finish.set_value();const bool completed=response.get();input.stop();
    require(begun&&reached&&held&&completed&&ack.injected&&ack.inject_us>=20000&&!input.held(),"ACK preceded injection completion or shutdown did not release");
}
void binding_and_release_check(){
    {InputServer input;require(!input.begin(8)&&input.finished()&&input.injected()==0,"Old session bound to input pipe");}
    {InputServer input(7,GetCurrentProcessId()+1);require(!input.begin()&&input.finished()&&input.injected()==0,"Wrong process bound to input pipe");}
    {InputServer input;InputAck ack;require(input.begin()&&input.send(1,{InputKind::Key,0x1e,0,0},8)&&!input.ack(ack)&&
        input.finished()&&input.injected()==0,"An old-session packet crossed a new input binding");}
    {InputServer input;InputAck ack;require(input.begin()&&input.send(2,{InputKind::Button,1,0,0})&&input.ack(ack)&&input.held(),"Hold before sequence check");
        input.send(2,{InputKind::Relative,1,2,0});require(!input.ack(ack)&&input.finished()&&!input.held()&&input.injected()==2,"Duplicate sequence injected / failed to release");}
    {InputServer input;InputAck ack;require(input.begin()&&input.send(1,{InputKind::Key,0x1e,0,0})&&input.ack(ack)&&input.held(),"Hold before revocation");
        input.authorized=0;input.send(2,{InputKind::Key,0x30,0,0});require(!input.ack(ack)&&input.finished()&&!input.held()&&input.injected()==2,"Revoked session injected / failed to release");}
    {InputServer input;InputAck ack;require(input.begin()&&input.send(1,{InputKind::Button,2,0,0})&&input.ack(ack)&&input.held(),"Hold before pipe close");
        input.pipe.disconnect();require(input.finished()&&!input.held()&&input.released==1,"Input pipe close did not release held button");}
    {InputServer input;InputAck ack;require(input.begin()&&input.send(1,{InputKind::Key,0x1e,0,0})&&input.ack(ack)&&input.held(),"Hold before expiry");
        require(input.send(2,{InputKind::Key,0x30,0,0},7,input_now_us()-broker::InputMaxQueueUs-1000)&&input.ack(ack)&&!ack.injected&&
            ack.host_queue_us>broker::InputMaxQueueUs&&!input.held()&&input.injected()==2,"Expired queued input replayed / failed to release");
        input.send(3,{InputKind::Key,0x30,0,0});
        input.pipe.disconnect();require(input.finished()&&input.injected()==2,"Failed input lane admitted queued input after rejection");}
    for(unsigned iteration=0;iteration<12;++iteration){
        InputServer input(100+iteration);InputAck ack;
        require(input.begin(100+iteration)&&input.send(1,{InputKind::Key,0x1e,0,0},100+iteration)&&input.ack(ack)&&ack.injected,"Restart input session");
        auto racing_send=std::async(std::launch::async,[&]{return input.send(2,{InputKind::Key,0x30,0,0},100+iteration);});
        input.stop();racing_send.get();require(!input.held()&&input.released==1,"Concurrent send/stop or restart retained held input");
    }
}
void desktop_error_and_retry_check(){
    unsigned calls=0,pauses=0;bool fresh=true;
    const auto denied=[]()->bool{throw std::system_error(ERROR_ACCESS_DENIED,std::system_category(),"Attach input desktop");};
    require(broker::retry_desktop_input([&]{if(++calls==1)return denied();return true;},[&]{return fresh;},[&]{++pauses;}),"Transient desktop rejection did not recover");
    require(calls==2&&pauses==1,"Desktop retry duplicated accepted input");
    calls=pauses=0;bool rejected=false;
    try{broker::retry_desktop_input([&]()->bool{++calls;return denied();},[&]{return fresh;},[&]{++pauses;});}catch(const std::system_error& error){rejected=error.code().value()==ERROR_ACCESS_DENIED;}
    require(rejected&&calls==2&&pauses==1,"Persistent denial lost error or retried without bound");
    calls=pauses=0;
    require(!broker::retry_desktop_input([&]()->bool{++calls;return denied();},[&]{return fresh;},[&]{++pauses;fresh=false;}),"Expired/revoked packet retried after desktop switch");
    require(calls==1&&pauses==1,"Expired input operation ran again");
    fresh=true;calls=pauses=0;
    require(broker::retry_desktop_input([&]{++calls;return true;},[&]{return fresh;},[&]{++pauses;})&&calls==1&&pauses==0,"Normal input incurred retry delay");
    Pipe pipe;std::atomic<bool> stopped=false;
    auto worker=std::async(std::launch::async,[&]{return broker::serve_input(pipe.server,GetCurrentProcessId(),stopped,
        [](uint64_t session){return session==7;},[&](const InputPacket&)->bool{return denied();},[]{},
        [&](const std::string& problem){require(problem.find("Windows error 5")!=std::string::npos,"Desktop error number was swallowed");
            require(broker::send_input_message(pipe.server,broker::Notice,broker::Bytes(problem.begin(),problem.end()),&stopped),"Independent input error notice failed");});});
    struct Stop {std::atomic<bool>& stopped;HANDLE pipe;~Stop(){stopped=true;CancelIoEx(pipe,nullptr);}} stop{stopped,pipe.server};
    broker::Bytes bytes;broker::u64(bytes,7);broker::Kind kind;
    require(broker::send_input_message(pipe.client,broker::InputStart,bytes,&stopped)&&broker::receive(pipe.client,kind,bytes,&stopped,broker::InputMaxBytes)&&kind==broker::InputStarted,"Error test startup failed");
    InputPacket packet{7,1,100,{InputKind::Key,0x1e,0,0},input_now_us()};
    require(broker::send_input_message(pipe.client,broker::Input,broker::input_packet_bytes(packet),&stopped),"Error test send failed");
    require(broker::receive(pipe.client,kind,bytes,&stopped,broker::InputMaxBytes)&&kind==broker::Notice,"Error report missing before rejection ACK");
    InputAck ack;require(broker::receive(pipe.client,kind,bytes,&stopped,broker::InputMaxBytes)&&kind==broker::InputComplete&&broker::read_input_ack(bytes,ack)&&!ack.injected,"Failure notice hid input rejection ACK");
    pipe.disconnect();require(worker.wait_for(std::chrono::seconds(2))==std::future_status::ready&&!worker.get(),"Failed input owner did not close");
}

}
int main(){try{
    state_check();wire_check();blocked_media_check();completion_check();binding_and_release_check();desktop_error_and_retry_check();
    std::puts("PASS host input: real independent pipe under blocked video, process/session/sequence binding, post-injection ACK, expiry, release retries, revoke/close, desktop retries/error notices and restarts. Fake sender only; no desktop input injected.");return 0;
}catch(const std::exception& error){std::printf("FAIL host input: %s\n",error.what());return 1;}}
