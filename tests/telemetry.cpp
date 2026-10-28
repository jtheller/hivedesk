#include "hivedesk/telemetry.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
std::string read(const std::filesystem::path& file){std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
template<class F>void wait(F condition){for(int i=0;i<100;++i){if(condition())return;std::this_thread::sleep_for(std::chrono::milliseconds(30));}throw std::runtime_error("Telemetry output did not reach expected state");}
}
int main(){
    const auto directory=std::filesystem::temp_directory_path()/("HiveDeskTelemetry-"+std::to_string(GetCurrentProcessId()));
    try {
        require(!std::filesystem::exists(directory),"Refusing to reuse a test directory");std::filesystem::create_directory(directory);
        struct Cleanup {std::filesystem::path directory;~Cleanup(){std::error_code ignored;for(const auto* name:{L"diagnostics.json",L"diagnostics.tmp",L"diagnostics.jsonl",L"diagnostics.previous.jsonl"})std::filesystem::remove(directory/name,ignored);std::filesystem::remove(directory,ignored);}} cleanup{directory};
        {std::ofstream file(directory/L"diagnostics.jsonl",std::ios::binary);file<<std::string(2*1024*1024,'x');}
        const auto latest=directory/L"diagnostics.json";
        {
            hive::Telemetry monitor(directory);monitor.enabled(true);monitor.select(hive::Telemetry::Side::View,"peer",9,"Peer \"quoted\"\n");
            monitor.health(hive::Telemetry::Side::View,"peer",{9,144000,40000,hive::PressureEncode});
            hive::SourceDiagnostics source{9,40000,22500,100,0,1000,200,2500,20000,10000,true};
            source.rate_details=true;source.automatic_cbr=true;source.target_bitrate_bps=120000000;source.actual_bitrate_bps=115500000;source.average_qp=19;source.quality_limited=true;source.rate_reason=2;
            source.receiver_service_us=1200;source.receiver_queue_us=300;source.receiver_recover=true;source.feedback_sequence=57;source.source_discarded=6;source.sequence_gaps=2;source.age_expired=3;source.queue_overflows=4;
            monitor.source(hive::Telemetry::Side::View,"peer",source);
            hive::ViewerDiagnostics stats;stats.session=9;stats.received_millihz=40000;stats.presented_millihz=39900;
            stats.age_expired=3;stats.sequence_gaps=2;stats.queue_overflows=4;stats.transit_extra_us=3300;
            stats.nvdec_map_us=1500;stats.convert_us=700;stats.draw_us=90;stats.present_gap_p95_us=12000;stats.present_gap_max_us=35000;
            monitor.viewer("peer",stats,2560,1440);
            monitor.select(hive::Telemetry::Side::Host,"outbound",90,"Receiver");
            hive::VideoSendDiagnostics send;send.available=true;send.connection=4;send.submitted_packets=11;send.completed_packets=10;send.completed_bytes=1000000;
            send.failed_packets=1;send.queue_expired=2;send.samples=11;send.sample_age_us=700;send.encrypt_p95_us=200;send.send_p95_us=1900;send.send_max_us=25000;send.ready_gap_p95_us=80;send.inflight=2;
            send.write_limit=32;send.owned_bytes=800000;send.write_budget_bytes=1250000;
            send.tcp_available=true;send.tcp_sample_age_us=12000;send.tcp_rtt_us=105000;send.tcp_min_rtt_us=100000;
            send.tcp_ideal_bytes=1250000;send.tcp_cwnd_bytes=1500000;send.tcp_send_window_bytes=2000000;send.tcp_inflight_bytes=700000;
            monitor.video_send("outbound",90,send);
            auto obsolete=send;obsolete.completed_packets=999;
            monitor.video_send("outbound",89,obsolete);monitor.video_send("different",90,obsolete);
            hive::InputDiagnostics input;input.available=input.dedicated=input.ready=true;input.session=9;input.connection=7;
            input.enqueued=80;input.coalesced=20;input.sent=60;input.acknowledged=58;input.inflight=2;input.samples=58;input.sample_age_us=500;
            input.capture_to_send_p99_us=450;input.ack_roundtrip_p50_us=1800;input.ack_roundtrip_p95_us=3000;input.ack_roundtrip_p99_us=4200;
            input.host_queue_p95_us=130;input.inject_p95_us=90;monitor.input("peer",9,input);
            auto wrong_input=input;wrong_input.acknowledged=999;monitor.input("peer",8,wrong_input);monitor.input("different",9,wrong_input);
            wrong_input.session=8;monitor.input("peer",9,wrong_input);
            // The writer may publish source data before the later viewer and
            // sender updates. Assert one snapshot containing all three stages.
            std::string value;
            wait([&]{value=read(latest);return value.find("\"nvenc_ms\":20.000")!=std::string::npos&&
                value.find("\"present_gap_max_ms\":35.000")!=std::string::npos&&
                value.find("\"completed_packets\":10,")!=std::string::npos&&value.find("\"acknowledged\":58,")!=std::string::npos;});
            require(value.find("Peer \\\"quoted\\\"\\u000a")!=std::string::npos,"JSON string escaping");
            require(value.find("\"width\":2560")!=std::string::npos&&value.find("\"cap_fps\":40.000")!=std::string::npos,"Missing resolution or live cap");
            require(value.find("\"automatic_cbr\":true,\"target_mbps\":120.000,\"actual_mbps\":115.500,\"average_qp\":19")!=std::string::npos,"Missing automatic bitrate or measured quality");
            require(value.find("\"quality_limited\":true,\"reason\":2,\"receiver_service_ms\":1.200,\"receiver_queue_ms\":0.300,\"receiver_recovering\":true")!=std::string::npos,"Missing automatic-rate decision inputs");
            require(value.find("\"feedback_sequence\":57,\"source_discarded\":6,\"sequence_gaps\":2,\"age_expired\":3,\"queue_overflows\":4")!=std::string::npos,"Missing recovery cause counters");
            require(value.find("\"service_sample_age_ms\":null,\"receive_sample_age_ms\":null,\"transit_excess_ms\":3.300")!=std::string::npos,"Unavailable samples appeared fresh");
            require(value.find("\"nvdec_map_ms\":1.500,\"convert_ms\":0.700,\"draw_ms\":0.090,\"present_gap_p95_ms\":12.000,\"present_gap_max_ms\":35.000")!=std::string::npos,"Missing viewer stalls/cadence metrics");
            require(value.find("\"completed_packets\":10,\"completed_bytes\":1000000,\"failed_packets\":1,\"queue_expired\":2")!=std::string::npos&&value.find("\"completed_packets\":999")==std::string::npos,"Video send counters or session isolation failed");
            require(value.find("\"inflight\":2,\"send_p95_ms\":1.900,\"send_max_ms\":25.000")!=std::string::npos&&value.find("\"ready_gap_p95_ms\":0.080")!=std::string::npos,"Missing bounded in-flight writes, wire completion and ready dispatch costs");
            require(value.find("\"write_limit\":32,\"owned_bytes\":800000,\"write_budget_bytes\":1250000")!=std::string::npos,"Missing adaptive byte ownership");
            require(value.find("\"tcp_available\":true,\"tcp_rtt_ms\":105.000,\"tcp_min_rtt_ms\":100.000")!=std::string::npos&&
                value.find("\"tcp_ideal_bytes\":1250000,\"tcp_cwnd_bytes\":1500000,\"tcp_send_window_bytes\":2000000,\"tcp_inflight_bytes\":700000,\"tcp_sample_age_ms\":12.000")!=std::string::npos,"TCP observation units or windows missing");
            require(value.find("\"dedicated\":true,\"ready\":true,\"session\":9")!=std::string::npos&&value.find("\"acknowledged\":999")==std::string::npos,"Input diagnostics session isolation failed");
            require(value.find("\"capture_to_send_p99_ms\":0.450")!=std::string::npos&&value.find("\"ack_roundtrip_p50_ms\":1.800,\"ack_roundtrip_p95_ms\":3.000,\"ack_roundtrip_p99_ms\":4.200")!=std::string::npos,"Input stage units/percentiles missing");
            require(std::filesystem::exists(directory/L"diagnostics.previous.jsonl"),"History did not rotate");
            monitor.select(hive::Telemetry::Side::View,"second",10,"Second");
            monitor.source(hive::Telemetry::Side::View,"peer",{9,999000});
            wait([&]{const auto s=read(latest);return s.find("\"session\":10")!=std::string::npos&&s.find("\"source\":null")!=std::string::npos;});
            monitor.disconnected("second");wait([&]{return read(latest).find("\"connected\":false")!=std::string::npos;});
            monitor.enabled(false);wait([&]{return read(latest).find("\"status\":\"disabled\",\"view\":null,\"host\":null")!=std::string::npos;});
        }
        require(read(latest).find("\"status\":\"stopped\"")!=std::string::npos,"Shutdown left live-looking statistics");
        require(std::filesystem::file_size(directory/L"diagnostics.jsonl")<2*1024*1024,"History exceeded bound");
        std::cout<<"PASS telemetry: asynchronous snapshot, JSON escaping, session isolation, disconnect state, disabled/stopped state and bounded history rotation.\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL telemetry: "<<e.what()<<'\n';return 1;}
}
