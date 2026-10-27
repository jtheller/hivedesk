#pragma once
#include "broker_wire.h"
#include "hivedesk/input_channel.h"
#include <array>
#include <algorithm>
#include <system_error>

namespace hive::broker {
constexpr uint32_t InputMaxBytes=128;
constexpr uint64_t InputMaxQueueUs=SessionStallUs;
inline bool send_input_message(HANDLE pipe,Kind kind,const Bytes& payload,const std::atomic<bool>* cancel){
    if(payload.size()>InputMaxBytes)return false;
    std::array<uint8_t,12+InputMaxBytes> message{};
    const uint32_t header[]{Magic,uint32_t(kind),uint32_t(payload.size())};
    std::memcpy(message.data(),header,sizeof(header));
    if(!payload.empty())std::memcpy(message.data()+sizeof(header),payload.data(),payload.size());
    return transfer(pipe,message.data(),sizeof(header)+payload.size(),true,cancel);
}
inline Bytes input_packet_bytes(const InputPacket& packet){
    Bytes bytes;bytes.reserve(48);u64(bytes,packet.session);u64(bytes,packet.sequence);u64(bytes,packet.capture_time_us);
    u32(bytes,uint32_t(packet.event.kind));u32(bytes,uint32_t(packet.event.a));u32(bytes,uint32_t(packet.event.b));u32(bytes,uint32_t(packet.event.c));
    u64(bytes,packet.received_time_us);return bytes;
}
inline bool read_input_packet(const Bytes& bytes,InputPacket& packet){
    Reader reader{bytes};packet.session=reader.u64();packet.sequence=reader.u64();packet.capture_time_us=reader.u64();
    packet.event.kind=InputKind(reader.u32());packet.event.a=int32_t(reader.u32());packet.event.b=int32_t(reader.u32());packet.event.c=int32_t(reader.u32());
    packet.received_time_us=reader.u64();return reader.end()&&packet.session&&packet.sequence&&packet.capture_time_us&&valid_input(packet.event);
}
inline Bytes input_ack_bytes(const InputAck& ack){
    Bytes bytes;bytes.reserve(36);u64(bytes,ack.session);u64(bytes,ack.sequence);u64(bytes,ack.capture_time_us);
    u32(bytes,ack.host_queue_us);u32(bytes,ack.inject_us);u32(bytes,ack.injected?1:0);return bytes;
}
inline bool read_input_ack(const Bytes& bytes,InputAck& ack){
    Reader reader{bytes};ack.session=reader.u64();ack.sequence=reader.u64();ack.capture_time_us=reader.u64();
    ack.host_queue_us=reader.u32();ack.inject_us=reader.u32();const auto injected=reader.u32();ack.injected=injected!=0;
    return reader.end()&&ack.session&&ack.sequence&&ack.capture_time_us&&injected<=1;
}
inline uint32_t input_elapsed_us(uint64_t from,uint64_t to){
    return from&&to>=from?uint32_t(std::min<uint64_t>(UINT32_MAX,to-from)):0;
}
// A desktop can change between attachment and SendInput. Retry one rejected
// Windows operation only, within the original age/authority checks. Accepted
// input is never retried and this cannot extend the session stall watchdog.
template<class Operation,class Fresh,class Pause>
bool retry_desktop_input(Operation&& operation,Fresh&& fresh,Pause&& pause){
    for(unsigned attempt=0;attempt<2;++attempt){
        if(!fresh())return false;
        try{return operation();}
        catch(const std::system_error&){if(attempt||!fresh())throw;pause();}
    }
    return false;
}
struct IgnoreInputError {void operator()(const std::string&)const{}};

// The sole injection owner runs on a windowless, hook-free desktop thread.
// Its pipe, queue and ACKs never share media IPC or a MediaHost lock. Callbacks
// keep injection mockable without any production mode that bypasses identity.
template<class Authorize,class Inject,class Release,class ReportError=IgnoreInputError>
bool serve_input(HANDLE pipe,DWORD expected_pid,const std::atomic<bool>& stopped,
    Authorize&& authorize,Inject&& inject,Release&& release,ReportError report_error={}){
    struct Cleanup {Release& release;~Cleanup(){try{release();}catch(...){}}} cleanup{release};
    Kind kind{};Bytes bytes;ULONG client_pid=0;
    if(!expected_pid||!GetNamedPipeClientProcessId(pipe,&client_pid)||client_pid!=expected_pid||
        !receive(pipe,kind,bytes,&stopped,InputMaxBytes)||kind!=InputStart)return false;
    Reader reader{bytes};const auto session=reader.u64();
    if(!reader.end()||!session||!authorize(session))return false;
    if(!send_input_message(pipe,InputStarted,{},&stopped))return false;
    uint64_t completed=0;
    while(!stopped&&receive(pipe,kind,bytes,&stopped,InputMaxBytes)){
        InputPacket packet;
        if(kind!=Input||!read_input_packet(bytes,packet)||packet.session!=session||packet.sequence<=completed||!authorize(session))return false;
        InputAck ack;ack.session=session;ack.sequence=packet.sequence;ack.capture_time_us=packet.capture_time_us;
        const auto started=input_now_us();ack.host_queue_us=input_elapsed_us(packet.received_time_us,started);
        std::string problem;
        try{
            if(packet.received_time_us&&packet.received_time_us<=started&&started-packet.received_time_us<=InputMaxQueueUs)
                ack.injected=inject(packet);
        }catch(const std::system_error& error){problem="Windows error "+std::to_string(error.code().value())+": "+error.what();}
        catch(const std::exception& error){problem=error.what();}
        catch(...){problem="Unknown desktop input failure";}
        // Only after the injection call returns. This measures Windows input
        // acceptance, never application handling, GPU rendering or scanout.
        ack.inject_us=input_elapsed_us(started,input_now_us());completed=packet.sequence;
        if(!ack.injected){
            try{release();}catch(...){}
            if(problem.empty())problem="Input rejected: session revoked, event expired, or desktop unavailable";
            try{report_error(problem);}catch(...){}
        }
        if(!send_input_message(pipe,InputComplete,input_ack_bytes(ack),&stopped))return false;
        if(!ack.injected){
            // DisconnectNamedPipe discards unread outbound data. Keep the
            // rejection readable until the bridge consumes it and closes its
            // end. Held inputs are already released; queued packets are only
            // drained, never admitted. Session stop still cancels this read.
            while(!stopped&&receive(pipe,kind,bytes,&stopped,InputMaxBytes)){}
            return false;
        }
    }
    return true;
}
}
