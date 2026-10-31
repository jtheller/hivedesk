#include "../src/broker_wire.h"
#include "../src/broker_readiness.h"
#include "../src/rate_wire.h"
#include <cstdio>
#include <future>
#include <array>
#include <stdexcept>
#include <thread>
using namespace hive;
namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void frame_composition_check(){
    EncodedFrame frame;frame.width=2560;frame.height=1440;frame.sequence=91;frame.capture_time_us=1000000;
    frame.source_ready_us=1010000;frame.session=13;frame.keyframe=true;frame.data={0,0,0,1,99};
    for(const auto composition:{CursorComposition::Unknown,CursorComposition::Separate,CursorComposition::NoSeparate}){
        frame.cursor_composition=composition;const auto bytes=broker::write_frame(frame);EncodedFrame decoded;
        require(broker::read_frame(bytes,decoded)&&decoded.width==frame.width&&decoded.height==frame.height&&
            decoded.sequence==frame.sequence&&decoded.capture_time_us==frame.capture_time_us&&decoded.source_ready_us==frame.source_ready_us&&
            decoded.session==frame.session&&decoded.keyframe&&decoded.cursor_composition==composition&&decoded.data==frame.data,
            "Broker lost image-associated cursor ownership or frame provenance");
        for(size_t size=0;size<=48;++size){
            const broker::Bytes truncated(bytes.begin(),bytes.begin()+size);
            require(!broker::read_frame(truncated,decoded),"Broker accepted truncated frame metadata or empty image");
        }
    }
    auto bytes=broker::write_frame(frame);EncodedFrame decoded;
    for(const auto invalid:{3u,255u,256u,0x10001u,UINT32_MAX}){
        for(unsigned i=0;i<4;++i)bytes[44+i]=uint8_t(invalid>>(i*8));
        require(!broker::read_frame(bytes,decoded),"Broker accepted invalid or narrowing-wrapped cursor ownership");
    }
    frame.cursor_composition=CursorComposition::Separate;frame.source_ready_us=0;
    require(broker::read_frame(broker::write_frame(frame),decoded)&&!decoded.source_ready_us&&decoded.cursor_composition==CursorComposition::Separate,
        "Broker cursor ownership incorrectly requires source timing");
}
void rate_extension_check(){
    StreamFeedback feedback{7,900,10,1200,5,true};feedback.rate_details=true;feedback.feedback_sequence=80;
    feedback.received_bytes=0x123456789ull;feedback.received_frames=310;feedback.received_kbps=100000;feedback.received_millihz=144000;
    feedback.receive_sample_age_us=80;feedback.service_sample_age_us=90;feedback.transit_extra_us=100;
    feedback.sequence_gaps=2;feedback.age_expired=3;feedback.queue_overflows=4;
    broker::Bytes data;rate_wire::write_feedback(feedback,[&](uint32_t n){broker::u32(data,n);},[&](uint64_t n){broker::u64(data,n);});
    broker::Reader r{data};StreamFeedback decoded;decoded.session=7;
    require(rate_wire::read_feedback(decoded,[&]{return r.u32();},[&]{return r.u64();})&&r.end()&&valid_feedback(decoded)&&
        decoded.received_bytes==feedback.received_bytes&&decoded.received_frames==310&&decoded.feedback_sequence==80&&
        decoded.received_kbps==100000&&decoded.received_millihz==144000&&decoded.receive_sample_age_us==80&&
        decoded.service_sample_age_us==90&&decoded.transit_extra_us==100&&decoded.sequence_gaps==2&&decoded.age_expired==3&&decoded.queue_overflows==4,
        "Broker automatic-rate feedback extension roundtrip");
    for(size_t size=0;size<data.size();++size){broker::Bytes short_data(data.begin(),data.begin()+size);broker::Reader truncated{short_data};StreamFeedback f;
        require(!rate_wire::read_feedback(f,[&]{return truncated.u32();},[&]{return truncated.u64();})||!truncated.end(),"Broker truncated feedback extension accepted");}
    SourceDiagnostics source;source.session=7;source.rate_details=true;source.automatic_cbr=true;source.quality_limited=true;source.encode_details=true;source.receiver_recover=true;
    source.target_bitrate_bps=80000000;source.actual_bitrate_bps=70000000;source.average_qp=21;source.rate_reason=2;source.receiver_service_us=1200;source.receiver_queue_us=5;
    source.feedback_sequence=80;source.source_discarded=9;source.sequence_gaps=2;source.age_expired=3;source.queue_overflows=4;
    data.clear();rate_wire::write_diagnostics(source,[&](uint32_t n){broker::u32(data,n);},[&](uint64_t n){broker::u64(data,n);});
    broker::Reader sr{data};SourceDiagnostics observed;observed.session=7;
    require(rate_wire::read_diagnostics(observed,[&]{return sr.u32();},[&]{return sr.u64();})&&sr.end()&&valid_diagnostics(observed)&&
        observed.automatic_cbr&&observed.quality_limited&&observed.encode_details&&observed.receiver_recover&&
        observed.target_bitrate_bps==80000000&&observed.actual_bitrate_bps==70000000&&observed.average_qp==21&&observed.rate_reason==2&&
        observed.receiver_service_us==1200&&observed.receiver_queue_us==5&&observed.feedback_sequence==80&&observed.source_discarded==9&&
        observed.sequence_gaps==2&&observed.age_expired==3&&observed.queue_overflows==4,"Broker automatic-rate source extension roundtrip");
    for(size_t size=0;size<data.size();++size){broker::Bytes short_data(data.begin(),data.begin()+size);broker::Reader truncated{short_data};SourceDiagnostics d;
        require(!rate_wire::read_diagnostics(d,[&]{return truncated.u32();},[&]{return truncated.u64();})||!truncated.end(),"Broker truncated diagnostic extension accepted");}
    data[4]=16;broker::Reader flags{data};
    require(!rate_wire::read_diagnostics(observed,[&]{return flags.u32();},[&]{return flags.u64();}),"Unknown diagnostic flags accepted");
    source.backend=EncoderBackend::Intel;
    data.clear();rate_wire::write_diagnostics(source,[&](uint32_t n){broker::u32(data,n);},[&](uint64_t n){broker::u64(data,n);},true);
    broker::Reader codec{data};
    require(rate_wire::read_diagnostics(observed,[&]{return codec.u32();},[&]{return codec.u64();})&&codec.end()&&observed.backend==EncoderBackend::Intel,"Broker codec identity roundtrip");
    broker::Reader legacy{data};require(!rate_wire::read_diagnostics(observed,[&]{return legacy.u32();},[&]{return legacy.u64();},false),"Unnegotiated codec extension accepted");
    for(size_t size=0;size<data.size();++size){broker::Bytes short_data(data.begin(),data.begin()+size);broker::Reader truncated{short_data};SourceDiagnostics d;
        require(!rate_wire::read_diagnostics(d,[&]{return truncated.u32();},[&]{return truncated.u64();})||!truncated.end(),"Truncated codec identity accepted");}
}
struct Pipe {
    HANDLE server=INVALID_HANDLE_VALUE,client=INVALID_HANDLE_VALUE;
    Pipe(){
        static unsigned id=0;const auto name=L"\\\\.\\pipe\\HiveDesk.Test."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++id);
        server=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr);
        require(server!=INVALID_HANDLE_VALUE,"Create test pipe");
        client=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        require(client!=INVALID_HANDLE_VALUE,"Connect test pipe");
        OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        const bool connected=ConnectNamedPipe(server,&operation)||GetLastError()==ERROR_PIPE_CONNECTED;CloseHandle(operation.hEvent);require(connected,"Accept test client");
    }
    ~Pipe(){if(client!=INVALID_HANDLE_VALUE)CloseHandle(client);if(server!=INVALID_HANDLE_VALUE)CloseHandle(server);}
};
void readiness_check(){
    // A credit represents one free downstream slot. Sending a frame must not
    // return that credit while the consumer is still busy, even after spurious
    // notifications; cancellation must not require the consumer to free it.
    {
        Pipe p;broker::ReadySignal signal;std::atomic<bool> stopped=false,ready=false;
        std::promise<void> waiting;auto entered=waiting.get_future();std::atomic<bool> announced=false;
        auto bridge=std::async(std::launch::async,[&]{
            broker::Kind kind;broker::Bytes frame;
            if(!broker::receive(p.client,kind,frame,&stopped)||kind!=broker::Frame)return false;
            return signal.wait(stopped,[&]{if(!announced.exchange(true))waiting.set_value();return ready.load();})&&broker::send(p.client,broker::Ready,{},&stopped);
        });
        require(broker::send(p.server,broker::Frame,{1,2,3},&stopped),"Send gated frame");
        const bool reached=entered.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        signal.notify();
        const bool held=bridge.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
        ready=true;signal.notify();
        const bool released=bridge.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        if(!released){stopped=true;signal.notify();CancelIoEx(p.client,nullptr);}
        require(reached&&held&&released&&bridge.get(),"Frame credit was lost or returned before downstream readiness");
        broker::Kind kind;broker::Bytes credit;
        require(broker::receive(p.server,kind,credit,&stopped)&&kind==broker::Ready&&credit.empty(),"Missing frame credit after downstream wake");
    }
    {
        broker::ReadySignal signal;std::atomic<bool> stopped=false;
        std::promise<void> waiting;auto entered=waiting.get_future();std::atomic<bool> announced=false;
        auto waiter=std::async(std::launch::async,[&]{return signal.wait(stopped,[&]{if(!announced.exchange(true))waiting.set_value();return false;});});
        const bool reached=entered.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        stopped=true;signal.notify();
        require(reached&&waiter.wait_for(std::chrono::seconds(1))==std::future_status::ready&&!waiter.get(),"Stop did not release downstream credit wait");
    }
    // Force the difficult ordering: the predicate has sampled false, then the
    // producer publishes true and notifies before the waiter can enroll. An
    // unpaired notification would be lost and each round would hit the fallback.
    const auto begin=std::chrono::steady_clock::now();
    for(int round=0;round<8;++round){
        broker::ReadySignal signal;std::atomic<bool> stopped=false,ready=false,first=true;
        std::promise<void> sampled,release,notifying;
        auto sampled_event=sampled.get_future(),release_event=release.get_future(),notifying_event=notifying.get_future();
        auto waiter=std::async(std::launch::async,[&]{return signal.wait(stopped,[&]{
            const bool available=ready.load();if(first.exchange(false)){sampled.set_value();release_event.wait();}return available;
        });});
        const bool reached=sampled_event.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        ready=true;
        auto notifier=std::async(std::launch::async,[&]{notifying.set_value();signal.notify();});
        notifying_event.wait();release.set_value();notifier.get();
        require(reached&&waiter.wait_for(std::chrono::seconds(1))==std::future_status::ready&&waiter.get(),"Readiness publication / wait race");
    }
    require(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(700),"Readiness wakes fell back to polling");
}
}
int main(){try{
    broker::Bytes data;broker::u32(data,0xfedcba98);broker::u64(data,0xfedcba9876543210ull);broker::Reader r{data};
    require(r.u32()==0xfedcba98&&r.u64()==0xfedcba9876543210ull&&r.end(),"Integer wire roundtrip");r.u32();require(!r.good&&!r.end(),"Truncated fields accepted");
    require(!broker::valid_input({InputKind::Key,0,0,0})&&!broker::valid_input({InputKind::Key,256,0,0})&&!broker::valid_input({InputKind::Button,6,0,0})&&!broker::valid_input({InputKind::Absolute,-1,1,0})&&!broker::valid_input({InputKind::Wheel,120,2,0})&&!broker::valid_input({InputKind(99),0,0,0}),"Invalid input accepted");
    {Pipe p;require(broker::send(p.client,broker::Input,data),"Write message");broker::Bytes received;broker::Kind kind;require(broker::receive(p.server,kind,received)&&kind==broker::Input&&received==data,"Message roundtrip");}
    for(const auto header: {std::array<uint32_t,3>{0,broker::Input,0},std::array<uint32_t,3>{0x48425631,broker::Frame,0},std::array<uint32_t,3>{broker::Magic,99,0},std::array<uint32_t,3>{broker::Magic,broker::Frame,broker::MaxBytes+1}}){
        Pipe p;auto h=header;require(broker::transfer(p.client,h.data(),sizeof(h),true,nullptr),"Write malformed header");broker::Bytes received;broker::Kind kind;require(!broker::receive(p.server,kind,received),"Malformed header accepted");
    }
    {Pipe p;std::atomic<bool> cancel=false;auto read=std::async(std::launch::async,[&]{broker::Bytes received;broker::Kind kind;return broker::receive(p.server,kind,received,&cancel);});Sleep(20);cancel=true;require(read.wait_for(std::chrono::seconds(2))==std::future_status::ready&&!read.get(),"Idle read cancellation");}
    {Pipe p;std::atomic<bool> cancel=false;broker::Bytes large(1024*1024,42);auto write=std::async(std::launch::async,[&]{return broker::send(p.server,broker::Frame,large,&cancel);});Sleep(20);cancel=true;require(write.wait_for(std::chrono::seconds(2))==std::future_status::ready&&!write.get(),"Blocked frame cancellation");}
    {Pipe p;broker::Kind kind;broker::Bytes payload;
        require(broker::send(p.client,broker::Activate)&&broker::receive(p.server,kind,payload)&&kind==broker::Activate&&payload.empty(),"Warm activation rejected by broker wire");
        require(broker::send(p.server,broker::Activated)&&broker::receive(p.client,kind,payload)&&kind==broker::Activated&&payload.empty(),"Warm activation acknowledgement rejected by bridge wire");
        for(const uint32_t enabled:{1u,0u,1u}){
            broker::Bytes preview;broker::u32(preview,enabled);
            require(broker::send(p.client,broker::Preview,preview)&&broker::receive(p.server,kind,payload)&&kind==broker::Preview&&payload==preview,
                "Live preview resume/pause rejected by actual broker pipe framing");
        }
    }
    readiness_check();rate_extension_check();frame_composition_check();
    std::puts("PASS broker: framing, cursor ownership/provenance, mixed-version rejection, invalid input/headers, I/O cancellation, bounded frame credit, readiness wake races, and blocked-credit cancellation.");return 0;
}catch(const std::exception& e){std::printf("FAIL broker: %s\n",e.what());return 1;}}
