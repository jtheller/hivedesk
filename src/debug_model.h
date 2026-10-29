#pragma once
#include "debug_surface.h"
#include "hivedesk/types.h"
#include "hivedesk/input_diagnostics.h"
#include "hivedesk/auto_rate.h"
#include <sstream>
#include <iomanip>
namespace hive {
struct DebugOverlayState {
    uint32_t width=0,height=0;
    StreamHealth health;SourceDiagnostics source;ViewerDiagnostics viewer;InputDiagnostics input;
    uint64_t now=0,source_time=0,health_time=0;
    std::wstring viewer_version,host_version;
};
inline DebugRows build_debug_rows(const DebugOverlayState& state,std::array<DiagnosticPulse,6>& debug_pulses) {
        using T=DiagnosticTone;const auto now=state.now;const auto& v=state.viewer;const auto& d=state.source;const auto& input=state.input;
        const auto& stream_health=state.health;const auto source_stats_time=state.source_time,health_time=state.health_time;
        const auto remote_width=state.width,remote_height=state.height;
        auto wide=[](const char* text){return std::wstring(text,text+std::char_traits<char>::length(text));};
        const auto budget=diagnostic_budget(stream_health.target_millihz);
        const bool source_fresh=source_stats_time&&now-source_stats_time<500;
        const auto encode_age=d.encode_age_us==UINT32_MAX?UINT64_MAX:uint64_t(d.encode_age_us)+(now-source_stats_time)*1000;
        const bool encode_fresh=source_fresh&&encode_age<500000,view_fresh=v.service_sample_age_us<500000;
        const bool input_fresh=input.samples&&input.sample_age_us<2000000;
        auto time=[&](uint32_t n,bool fresh=true){return diagnostic_threshold(n,budget*3/4,budget,fresh);};
        auto num=[](double n){std::wostringstream text;text<<std::fixed<<std::setprecision(1)<<n;return text.str();};
        auto metric=[&](const wchar_t* label,double n,T tone=T::Normal){return DebugSpan{std::wstring(label)+num(n),tone};};
        auto ms=[&](const wchar_t* label,uint32_t n,bool fresh=true){return metric(label,n/1000.0,time(n,fresh));};
        auto count=[&](const wchar_t* label,uint64_t n,size_t index){return DebugSpan{std::wstring(label)+std::to_wstring(n),debug_pulses[index].update(now,n)?T::Warning:T::Normal};};
        auto input_ms=[&](const wchar_t* label,uint32_t n,uint32_t warning,uint32_t critical){return DebugSpan{std::wstring(label)+(input_fresh?num(n/1000.0):L"--"),diagnostic_threshold(n,warning,critical,input_fresh)};};
        DebugRows rows{
            {{L"HiveDesk debug  "+std::to_wstring(remote_width)+L" x "+std::to_wstring(remote_height)+L"  HEVC 4:4:4"+(v.software_decode?L"  CPU":L"  NVDEC")}},
            {{L"Viewer (this PC): "+(state.viewer_version.empty()?std::wstring(L"Unknown"):state.viewer_version),state.viewer_version.empty()?T::Muted:T::Normal}},
            {{L"Host (remote): "+(state.host_version.empty()?std::wstring(L"Unknown"):state.host_version),state.host_version.empty()?T::Muted:state.host_version!=state.viewer_version?T::Warning:T::Normal}},
            {metric(L"Target ",stream_health.target_millihz/1000.0),metric(L"   Cap ",stream_health.rate_millihz/1000.0,health_time&&now-health_time<2000?(stream_health.pressure?T::Warning:T::Normal):T::Muted),{L" fps"}},
            {{L"Limit: "},{stream_health.pressure&PressureEncode?L"encode ":L"",T::Warning},{stream_health.pressure&PressureDecode?L"decode/render ":L"",T::Warning},{stream_health.pressure&PressureNetwork?L"network ":L"",T::Warning},{stream_health.pressure?L"":L"none"}},
            {metric(L"FPS  RX ",v.received_millihz/1000.0,v.receive_sample_age_us<500000?T::Normal:T::Muted),metric(L"   Decode ",v.decoded_millihz/1000.0,view_fresh?T::Normal:T::Muted),metric(L"   Present ",v.presented_millihz/1000.0,view_fresh?T::Normal:T::Muted)},
            {ms(L"Decode ",v.decode_us,view_fresh),ms(L"   Render ",v.render_us,view_fresh),ms(L"   Present ",v.present_us,view_fresh),{L" ms"}},
            {ms(L"Map ",v.nvdec_map_us,view_fresh),ms(L"   Convert ",v.convert_us,view_fresh),ms(L"   Draw ",v.draw_us,view_fresh),{L" ms"}},
            {ms(L"Service p90 ",v.service_p90_us,view_fresh),ms(L"   Queue ",v.queue_us),{L" ms / "+std::to_wstring(v.queue_depth)+L" frames",diagnostic_threshold(v.queue_depth,2,4)}},
            {metric(L"RX ",v.receive_kbps/1000.0),{L" Mbps"},count(L"  Drop ",v.discarded,0),count(L"  Skip ",v.skipped,1),{v.recovering?L"  IDR WAIT":L"  IDR ok",v.recovering?T::Critical:T::Normal}},
            {count(L"Recovery: gap ",v.sequence_gaps,2),count(L"   age ",v.age_expired,3),count(L"   overflow ",v.queue_overflows,4)},
            {metric(L"Source ",d.encoded_millihz/1000.0,source_fresh?T::Normal:T::Muted),{L" fps"},ms(L"   Encode ",d.encode_us,encode_fresh),{L" ms"}},
            {ms(L"Color/sync ",d.color_us,encode_fresh),ms(L"   NVENC latency ",d.nvenc_us,encode_fresh),{L" ms"}},
            {metric(L"Capture wait ",d.capture_wait_us/1000.0,source_fresh?T::Normal:T::Muted),{L" ms"},metric(L"   Send wait ",d.send_wait_us_per_second/1000.0,diagnostic_threshold(d.send_wait_us_per_second,100000,400000,source_fresh)),{L" ms/s"}},
            {ms(L"Transit excess ",d.network_extra_us,source_fresh),{L" ms"},{L"   Feedback "+(d.feedback_age_us==UINT32_MAX?std::wstring(L"--"):num(d.feedback_age_us/1000.0))+L" ms",diagnostic_threshold(d.feedback_age_us,300000,1000000,source_fresh)}},
            {metric(d.automatic_cbr?L"Auto CBR ":L"CQP baseline ",d.target_bitrate_bps/1000000.0,source_fresh?T::Normal:T::Muted),metric(L" / actual ",d.actual_bitrate_bps/1000000.0,source_fresh?T::Normal:T::Muted),{L" Mbps"}},
            {{L"QP "+(d.average_qp==UINT32_MAX?std::wstring(L"--"):std::to_wstring(d.average_qp)),diagnostic_threshold(d.average_qp,24,30,source_fresh&&d.average_qp!=UINT32_MAX)},{d.quality_limited?L"  quality floor":L"",T::Warning},{L"  "+wide(d.rate_reason<=4?auto_rate_reason_name(AutoRateReason(d.rate_reason)):"unknown"),source_fresh?T::Normal:T::Muted},count(L"  Drop ",d.source_discarded,5)},
            {{L"Source age "+(source_stats_time?std::to_wstring(now-source_stats_time):L"--")+L" ms",diagnostic_threshold(now-source_stats_time,500,2000,source_stats_time!=0)},{L"   Encode age "+(encode_age==UINT64_MAX?std::wstring(L"--"):num(encode_age/1000.0))+L" ms",encode_fresh?T::Normal:T::Muted}},
            {metric(L"Present gap p95 ",v.present_gap_p95_us/1000.0,view_fresh?T::Normal:T::Muted),metric(L" / max ",v.present_gap_max_us/1000.0,view_fresh?T::Normal:T::Muted),{L" ms"}},
            {input_ms(L"Input ACK p50 ",input.ack_roundtrip_p50_us,8000,20000),input_ms(L" / p99 ",input.ack_roundtrip_p99_us,8000,20000),{L" ms"}},
            {input_ms(L"Dispatch ",input.capture_to_send_p99_us,2000,8000),input_ms(L"   Host ",input.host_queue_p95_us,2000,8000),input_ms(L"   Inject ",input.inject_p95_us,2000,8000),{L" ms (p99/95/95)"}},
            {{L"Input queue "+std::to_wstring(input.queued),diagnostic_threshold(input.queued,8,32)},{L"   In flight "+std::to_wstring(input.inflight)+L"/"+std::to_wstring(input.credit_limit),diagnostic_threshold(input.inflight,input.credit_limit*3/4,input.credit_limit)},{input.ready?L"  ready":L"  waiting",input.ready?T::Normal:T::Muted}},
            {{L"Live tick ~16ms | FPS / Mbps rolling 250ms",T::Muted}},
            {{L"Amber: slow   Red: over budget   Gray: idle / stale",T::Muted}},
            {{L"Latency is not capacity. Present is not scanout.",T::Muted}}
        };
        return rows;
}
}
