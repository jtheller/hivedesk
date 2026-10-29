#include "hivedesk/telemetry.h"
#include "hivedesk/experience_trace.h"
#include "hivedesk/config.h"
#include "hivedesk/auto_rate.h"
#include <windows.h>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
namespace hive {
namespace {
std::string quoted(const std::string& text){
    std::ostringstream out;out<<'"';
    for(unsigned char ch:text){
        if(ch=='"'||ch=='\\')out<<'\\'<<char(ch);
        else if(ch<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<int(ch)<<std::dec;
        else out<<char(ch);
    }
    out<<'"';return out.str();
}
std::string age(ULONGLONG now,ULONGLONG at){return at?std::to_string(now>=at?now-at:0):"null";}
}
struct Telemetry::Impl {
    struct Endpoint {
        bool active=false,connected=true;std::string peer,name;uint64_t session=0;uint32_t width=0,height=0;
        StreamHealth health{};SourceDiagnostics source{};ViewerDiagnostics viewer{};
        VideoSendDiagnostics send{};
        InputDiagnostics input{};
        ULONGLONG health_at=0,source_at=0,viewer_at=0,send_at=0,input_at=0;
        bool matches(const std::string& id,uint64_t stamp)const{return active&&peer==id&&session==stamp;}
    };
    std::filesystem::path directory;
    std::mutex mutex;std::condition_variable changed;
    bool enabled=false,stopping=false,dirty=true,error_reported=false;
    Endpoint view,host;
    std::thread worker;
    explicit Impl(std::filesystem::path path):directory(std::move(path)),worker([this]{run();}){}
    ~Impl(){ {std::lock_guard lock(mutex);stopping=true;}changed.notify_one();worker.join();}
    Endpoint& endpoint(Side side){return side==Side::View?view:host;}
    static void render(std::ostringstream& out,const Endpoint& e,ULONGLONG now){
        if(!e.active){out<<"null";return;}
        out<<"{\"peer\":"<<quoted(e.peer)<<",\"name\":"<<quoted(e.name)<<",\"session\":"<<e.session
           <<",\"connected\":"<<(e.connected?"true":"false")<<",\"width\":"<<e.width<<",\"height\":"<<e.height<<",\"health_age_ms\":"<<age(now,e.health_at)
           <<",\"health\":";
        if(!e.health_at)out<<"null";
        else out<<"{\"target_fps\":"<<e.health.target_millihz/1000.0<<",\"cap_fps\":"<<e.health.rate_millihz/1000.0
                <<",\"pressure\":"<<e.health.pressure<<",\"encode_limit\":"<<((e.health.pressure&PressureEncode)?"true":"false")
                <<",\"decode_limit\":"<<((e.health.pressure&PressureDecode)?"true":"false")
                <<",\"send_limit\":"<<((e.health.pressure&PressureNetwork)?"true":"false")<<'}';
        out<<",\"source_age_ms\":"<<age(now,e.source_at)<<",\"source\":";
        if(!e.source_at)out<<"null";
        else {
            const auto& d=e.source;out<<"{\"fps\":"<<d.encoded_millihz/1000.0<<",\"encode_ms\":"<<d.encode_us/1000.0
                <<",\"capture_wait_ms\":"<<d.capture_wait_us/1000.0<<",\"send_wait_ms_per_s\":"<<d.send_wait_us_per_second/1000.0
                <<",\"transit_excess_ms\":"<<d.network_extra_us/1000.0<<",\"feedback_age_ms\":";
            if(d.feedback_age_us==UINT32_MAX)out<<"null";else out<<d.feedback_age_us/1000.0;
            out<<",\"encode_details\":"<<(d.encode_details?"true":"false")<<",\"color_ms\":";
            if(d.encode_details)out<<d.color_us/1000.0;else out<<"null";
            out<<",\"nvenc_ms\":";if(d.encode_details)out<<d.nvenc_us/1000.0;else out<<"null";
            out<<",\"encode_sample_age_ms\":";if(d.encode_details&&d.encode_age_us!=UINT32_MAX)out<<d.encode_age_us/1000.0;else out<<"null";
            out<<",\"rate_details\":"<<(d.rate_details?"true":"false")<<",\"rate\":";
            if(!d.rate_details)out<<"null";
            else {
                out<<"{\"automatic_cbr\":"<<(d.automatic_cbr?"true":"false")<<",\"target_mbps\":"<<d.target_bitrate_bps/1000000.0
                   <<",\"actual_mbps\":"<<d.actual_bitrate_bps/1000000.0<<",\"average_qp\":";
                if(d.average_qp==UINT32_MAX)out<<"null";else out<<d.average_qp;
                out<<",\"quality_limited\":"<<(d.quality_limited?"true":"false")<<",\"reason\":"<<d.rate_reason
                   <<",\"receiver_service_ms\":"<<d.receiver_service_us/1000.0<<",\"receiver_queue_ms\":"<<d.receiver_queue_us/1000.0
                   <<",\"receiver_recovering\":"<<(d.receiver_recover?"true":"false")<<",\"feedback_sequence\":"<<d.feedback_sequence
                   <<",\"source_discarded\":"<<d.source_discarded<<",\"sequence_gaps\":"<<d.sequence_gaps
                   <<",\"age_expired\":"<<d.age_expired<<",\"queue_overflows\":"<<d.queue_overflows
                   <<",\"reason_name\":"<<quoted(d.rate_reason<=4?auto_rate_reason_name(AutoRateReason(d.rate_reason)):"unknown")<<'}';
            }
            out<<'}';
        }
        out<<",\"viewer_age_ms\":"<<age(now,e.viewer_at)<<",\"viewer\":";
        if(!e.viewer_at)out<<"null";
        else {
            const auto& v=e.viewer;out<<"{\"rx_fps\":"<<v.received_millihz/1000.0<<",\"decode_fps\":"<<v.decoded_millihz/1000.0
                <<",\"present_fps\":"<<v.presented_millihz/1000.0<<",\"mbps\":"<<v.receive_kbps/1000.0
                <<",\"decode_ms\":"<<v.decode_us/1000.0<<",\"render_ms\":"<<v.render_us/1000.0<<",\"present_ms\":"<<v.present_us/1000.0
                <<",\"service_p90_ms\":"<<v.service_p90_us/1000.0<<",\"queue_ms\":"<<v.queue_us/1000.0
                <<",\"queue_frames\":"<<v.queue_depth<<",\"discarded\":"<<v.discarded<<",\"skipped\":"<<v.skipped
                <<",\"recovering\":"<<(v.recovering?"true":"false")<<",\"service_sample_age_ms\":";
            if(v.service_sample_age_us==UINT32_MAX)out<<"null";else out<<v.service_sample_age_us/1000.0;
            out<<",\"receive_sample_age_ms\":";if(v.receive_sample_age_us==UINT32_MAX)out<<"null";else out<<v.receive_sample_age_us/1000.0;
            out<<",\"transit_excess_ms\":"<<v.transit_extra_us/1000.0<<",\"wire_assembly_ms\":"<<v.wire_assembly_us/1000.0<<",\"sequence_gaps\":"<<v.sequence_gaps
               <<",\"age_expired\":"<<v.age_expired<<",\"queue_overflows\":"<<v.queue_overflows
               <<",\"nvdec_map_ms\":"<<v.nvdec_map_us/1000.0<<",\"convert_ms\":"<<v.convert_us/1000.0<<",\"draw_ms\":"<<v.draw_us/1000.0
               <<",\"present_gap_p95_ms\":"<<v.present_gap_p95_us/1000.0<<",\"present_gap_max_ms\":"<<v.present_gap_max_us/1000.0<<",\"presented_frames\":"<<v.presented_frames
               <<",\"decoder\":\""<<(v.software_decode?"software-hevc444":"nvdec")<<"\"}";
        }
        out<<",\"video_send_age_ms\":"<<age(now,e.send_at)<<",\"video_send\":";
        if(!e.send_at||!e.send.available)out<<"null";
        else {
            const auto& s=e.send;
            out<<"{\"connection\":"<<s.connection<<",\"submitted_packets\":"<<s.submitted_packets
               <<",\"completed_packets\":"<<s.completed_packets<<",\"completed_bytes\":"<<s.completed_bytes
               <<",\"failed_packets\":"<<s.failed_packets<<",\"queue_expired\":"<<s.queue_expired
               <<",\"samples\":"<<s.samples<<",\"sample_age_ms\":";
            if(s.sample_age_us==UINT32_MAX)out<<"null";else out<<s.sample_age_us/1000.0;
            out<<",\"encrypt_p95_ms\":"<<s.encrypt_p95_us/1000.0<<",\"encrypt_max_ms\":"<<s.encrypt_max_us/1000.0
               <<",\"write_limit\":"<<s.write_limit<<",\"owned_bytes\":"<<s.owned_bytes<<",\"write_budget_bytes\":"<<s.write_budget_bytes
               <<",\"tcp_available\":"<<(s.tcp_available?"true":"false")<<",\"tcp_rtt_ms\":"<<s.tcp_rtt_us/1000.0<<",\"tcp_min_rtt_ms\":"<<s.tcp_min_rtt_us/1000.0
               <<",\"tcp_ideal_bytes\":"<<s.tcp_ideal_bytes<<",\"tcp_cwnd_bytes\":"<<s.tcp_cwnd_bytes<<",\"tcp_send_window_bytes\":"<<s.tcp_send_window_bytes
               <<",\"tcp_inflight_bytes\":"<<s.tcp_inflight_bytes<<",\"tcp_sample_age_ms\":";if(s.tcp_sample_age_us==UINT32_MAX)out<<"null";else out<<s.tcp_sample_age_us/1000.0;
            out<<",\"inflight\":"<<s.inflight<<",\"send_p95_ms\":"<<s.send_p95_us/1000.0<<",\"send_max_ms\":"<<s.send_max_us/1000.0
               <<",\"queue_p95_ms\":"<<s.queue_p95_us/1000.0<<",\"queue_max_ms\":"<<s.queue_max_us/1000.0
               <<",\"submit_gap_p95_ms\":"<<s.submit_gap_p95_us/1000.0<<",\"submit_gap_max_ms\":"<<s.submit_gap_max_us/1000.0
               <<",\"ready_gap_p95_ms\":"<<s.ready_gap_p95_us/1000.0<<",\"ready_gap_max_ms\":"<<s.ready_gap_max_us/1000.0;
            out<<",\"packet_transport\":"<<s.packet_transport<<",\"packet_rtt_ms\":"<<s.packet_rtt_us/1000.0<<",\"packet_min_rtt_ms\":"<<s.packet_min_rtt_us/1000.0
               <<",\"packet_window\":"<<s.packet_window<<",\"packet_inflight\":"<<s.packet_inflight<<",\"packet_owned_bytes\":"<<s.packet_owned_bytes
               <<",\"packet_sent\":"<<s.packet_sent<<",\"packet_retransmitted\":"<<s.packet_retransmitted<<",\"packet_received\":"<<s.packet_received
               <<",\"packet_invalid\":"<<s.packet_invalid<<",\"packet_expired\":"<<s.packet_expired
               <<",\"packet_send_expired\":"<<s.packet_send_expired<<",\"packet_receive_expired\":"<<s.packet_receive_expired<<'}';
        }
        out<<",\"input_age_ms\":"<<age(now,e.input_at)<<",\"input\":";
        if(!e.input_at||!e.input.available)out<<"null";
        else {
            const auto& d=e.input;
            out<<"{\"dedicated\":"<<(d.dedicated?"true":"false")<<",\"ready\":"<<(d.ready?"true":"false")
               <<",\"session\":"<<d.session<<",\"connection\":"<<d.connection<<",\"enqueued\":"<<d.enqueued
               <<",\"coalesced\":"<<d.coalesced<<",\"sent\":"<<d.sent<<",\"acknowledged\":"<<d.acknowledged
               <<",\"credit_limit\":"<<d.credit_limit<<",\"credit_stalls\":"<<d.credit_stalls<<",\"credit_wait_total_ms\":"<<d.credit_wait_total_us/1000.0<<",\"credit_wait_max_ms\":"<<d.credit_wait_max_us/1000.0
               <<",\"failed\":"<<d.failed<<",\"queued\":"<<d.queued<<",\"inflight\":"<<d.inflight
               <<",\"samples\":"<<d.samples<<",\"sample_age_ms\":";
            if(d.sample_age_us==UINT32_MAX)out<<"null";else out<<d.sample_age_us/1000.0;
            out<<",\"capture_to_send_p50_ms\":"<<d.capture_to_send_p50_us/1000.0
               <<",\"capture_to_send_p95_ms\":"<<d.capture_to_send_p95_us/1000.0
               <<",\"capture_to_send_p99_ms\":"<<d.capture_to_send_p99_us/1000.0
               <<",\"capture_to_send_max_ms\":"<<d.capture_to_send_max_us/1000.0
               <<",\"ack_roundtrip_p50_ms\":"<<d.ack_roundtrip_p50_us/1000.0
               <<",\"ack_roundtrip_p95_ms\":"<<d.ack_roundtrip_p95_us/1000.0
               <<",\"ack_roundtrip_p99_ms\":"<<d.ack_roundtrip_p99_us/1000.0
               <<",\"ack_roundtrip_max_ms\":"<<d.ack_roundtrip_max_us/1000.0
               <<",\"host_queue_p95_ms\":"<<d.host_queue_p95_us/1000.0<<",\"host_queue_max_ms\":"<<d.host_queue_max_us/1000.0
               <<",\"inject_p95_ms\":"<<d.inject_p95_us/1000.0<<",\"inject_max_ms\":"<<d.inject_max_us/1000.0<<'}';
        }
        out<<'}';
    }
    std::string snapshot()const {
        SYSTEMTIME utc{};GetSystemTime(&utc);char time[40]{};
        std::snprintf(time,sizeof(time),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds);
        const auto now=GetTickCount64();std::ostringstream out;out.imbue(std::locale::classic());out<<std::fixed<<std::setprecision(3);
        out<<"{\"schema\":1,\"utc\":"<<quoted(time)<<",\"tick_ms\":"<<now<<",\"pid\":"<<GetCurrentProcessId()
           <<",\"status\":\""<<(stopping?"stopped":!enabled?"disabled":view.active||host.active?"streaming":"idle")<<"\",\"view\":";
        if(enabled&&!stopping)render(out,view,now);else out<<"null";
        out<<",\"host\":";if(enabled&&!stopping)render(out,host,now);else out<<"null";out<<"}\n";return out.str();
    }
    void write(const std::string& line){
        std::filesystem::create_directories(directory);
        const auto temporary=directory/L"diagnostics.tmp",latest=directory/L"diagnostics.json";
        {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<line;file.close();if(!file)throw std::runtime_error("write latest statistics");}
        if(!MoveFileExW(temporary.c_str(),latest.c_str(),MOVEFILE_REPLACE_EXISTING))throw std::runtime_error("replace latest statistics");
        const auto history=directory/L"diagnostics.jsonl",previous=directory/L"diagnostics.previous.jsonl";
        if(std::filesystem::exists(history)&&std::filesystem::file_size(history)+line.size()>2*1024*1024)
            if(!MoveFileExW(history.c_str(),previous.c_str(),MOVEFILE_REPLACE_EXISTING))throw std::runtime_error("rotate statistics history");
        std::ofstream file(history,std::ios::binary|std::ios::app);file<<line;file.close();if(!file)throw std::runtime_error("append statistics history");
    }
    void run(){
        for(;;){
            std::unique_lock lock(mutex);
            if(!dirty&&!stopping){if(enabled)changed.wait_for(lock,std::chrono::seconds(1),[this]{return dirty||stopping;});else changed.wait(lock,[this]{return dirty||stopping;});}
            const auto line=snapshot();const bool done=stopping;dirty=false;lock.unlock();
            try {write(line);error_reported=false;}
            catch(const std::exception& e){if(!error_reported){log(std::string("Local diagnostics: ")+e.what());error_reported=true;}}
            if(done)return;
        }
    }
};
Telemetry::Telemetry(std::filesystem::path directory):impl_(std::make_unique<Impl>(std::move(directory))){}
Telemetry::~Telemetry()=default;
void Telemetry::enabled(bool value){experience_enabled(value);std::lock_guard lock(impl_->mutex);if(impl_->enabled==value)return;impl_->enabled=value;impl_->dirty=true;impl_->changed.notify_one();}
void Telemetry::select(Side side,const std::string& peer,uint64_t session,const std::string& name){std::lock_guard lock(impl_->mutex);auto& e=impl_->endpoint(side);e={};e.active=true;e.peer=peer;e.name=name;e.session=session;if(impl_->enabled){impl_->dirty=true;impl_->changed.notify_one();}}
void Telemetry::clear(Side side){std::lock_guard lock(impl_->mutex);auto& e=impl_->endpoint(side);if(!e.active)return;e={};if(impl_->enabled){impl_->dirty=true;impl_->changed.notify_one();}}
void Telemetry::disconnected(const std::string& peer){std::lock_guard lock(impl_->mutex);for(auto* e:{&impl_->view,&impl_->host})if(e->active&&e->peer==peer)e->connected=false;if(impl_->enabled){impl_->dirty=true;impl_->changed.notify_one();}}
void Telemetry::health(Side side,const std::string& peer,const StreamHealth& h){std::lock_guard lock(impl_->mutex);auto& e=impl_->endpoint(side);if(e.matches(peer,h.session)){e.health=h;e.health_at=GetTickCount64();}}
void Telemetry::source(Side side,const std::string& peer,const SourceDiagnostics& d){std::lock_guard lock(impl_->mutex);auto& e=impl_->endpoint(side);if(e.matches(peer,d.session)){e.source=d;e.source_at=GetTickCount64();}}
void Telemetry::viewer(const std::string& peer,const ViewerDiagnostics& d,uint32_t width,uint32_t height){std::lock_guard lock(impl_->mutex);auto& e=impl_->view;if(e.matches(peer,d.session)){e.viewer=d;e.viewer_at=GetTickCount64();e.width=width;e.height=height;}}
void Telemetry::video_send(const std::string& peer,uint64_t session,const VideoSendDiagnostics& d){std::lock_guard lock(impl_->mutex);auto& e=impl_->host;if(e.matches(peer,session)){e.send=d;e.send_at=GetTickCount64();}}
void Telemetry::input(const std::string& peer,uint64_t session,const InputDiagnostics& d){std::lock_guard lock(impl_->mutex);auto& e=impl_->view;if(e.matches(peer,session)&&(!d.available||d.session==session)){e.input=d;e.input_at=GetTickCount64();}}
void Telemetry::size(Side side,const std::string& peer,uint64_t session,uint32_t width,uint32_t height){std::lock_guard lock(impl_->mutex);auto& e=impl_->endpoint(side);if(e.matches(peer,session)){e.width=width;e.height=height;}}
}
