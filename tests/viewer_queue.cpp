#include "../src/viewer_queue.h"
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
hive::EncodedFrame picture(uint64_t sequence,uint64_t capture,bool keyframe=false,uint64_t session=7) {
    hive::EncodedFrame frame;frame.sequence=sequence;frame.capture_time_us=capture;
    frame.keyframe=keyframe;frame.session=session;frame.data={1};return frame;
}
hive::EncodedFrame completed_picture(uint64_t sequence,uint64_t capture,uint64_t ready,bool keyframe=false,uint64_t session=7) {
    auto frame=picture(sequence,capture,keyframe,session);frame.source_ready_us=ready;return frame;
}
constexpr uint64_t Source=9000000000000ull,Local=2000000;
void service_samples_expire() {
    hive::ViewerServiceWindow service;
    for(uint64_t i=0;i<64;++i)service.add(Local+i*1000,30000);
    require(service.p90(Local+63000)==30000,"Fresh expensive service samples missing");
    require(service.p90(Local+1063001)==0&&service.sample_age(Local+1063001)==UINT32_MAX,"Idle decoder cost remained a capacity limit");
    for(uint64_t i=0;i<8;++i)service.add(Local+1100000+i*1000,1000);
    require(service.p90(Local+1107000)==1000&&service.sample_age(Local+1107000)==0,"Fresh cheap service retained expired costs");
    require(service.p90(Local+2100001)==0,"Insufficient current decoder samples appeared authoritative");
}
void present_gap_samples() {
    hive::ViewerPresentWindow intervals;
    intervals.presented(Local);
    require(intervals.p95(Local)==0&&intervals.maximum(Local)==0,"First presentation invented a gap");
    for(uint64_t i=1;i<=20;++i)intervals.presented(Local+i*10000);
    intervals.presented(Local+250000);
    require(intervals.p95(Local+250000)==10000&&intervals.maximum(Local+250000)==50000,"Presentation gap distribution lost a lone stall");
    intervals.presented(Local+300000);
    require(intervals.p95(Local+300000)==50000,"Repeated presentation stalls missing from p95");
    require(intervals.p95(Local+1300001)==0&&intervals.maximum(Local+1300001)==0,"Old presentation stalls never expired");
    intervals.clear();intervals.presented(Local+2000000);intervals.presented(Local+2010000);
    require(intervals.maximum(Local+2010000)==10000,"Session reset retained a previous presentation timestamp");
}
void normal_and_idle() {
    hive::ViewerQueue queue;
    for(uint64_t i=0;i<1000;++i){
        require(queue.push(picture(i,Source+i*10000,i==0),Local+i*10000),"Healthy frame rejected");
        auto active=queue.pop(Local+i*10000);require(bool(active),"Healthy frame missing");
        require(queue.presentable(*active,Local+i*10000+1000),"Healthy frame suppressed");
    }
    // Static desktop / idle time is not a backlog: both clocks advance together.
    require(queue.push(picture(1000,Source+1000000000),Local+1000000000),"Idle resume rejected");
    auto active=queue.pop(Local+1000000000);
    require(active&&queue.presentable(*active,Local+1000001000),"Idle resume considered stale");
    require(!queue.recovering()&&queue.discarded()==0,"Healthy stream requested recovery");
}
void decoded_expiry_static_refresh_and_epochs() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Static decoded baseline rejected");
    auto decoded=queue.pop(Local);require(bool(decoded),"Static decode missing");
    constexpr auto expired=Local+hive::ViewerQueue::MaxAgeUs+1;
    require(!queue.output_fresh(*decoded,expired)&&queue.empty()&&queue.recovering(),"Expired static output failed to request a new desktop picture");
    require(!queue.mark_output_ready(*decoded,expired+100000)&&queue.age_expired()==1,"Expired static output cleared refresh or counted repeated polling");
    require(!queue.recovery_expired(expired+hive::ViewerQueue::RecoveryTimeoutUs-1),"Display-only recovery deadline fired early");
    require(queue.recovery_expired(expired+hive::ViewerQueue::RecoveryTimeoutUs),"Repeated display expiry deferred the two-second barrier");
    const auto at=expired+hive::ViewerQueue::RecoveryTimeoutUs+1000;
    require(queue.push(picture(1,Source+at-Local,true),at),"Static refresh IDR rejected");
    require(!queue.recovering()&&queue.recovery_pending(),"Queued refresh IDR did not clear request or cleared readiness deadline too early");
    require(!queue.output_fresh(*decoded,at)&&queue.size()==1&&!queue.recovering(),"Old decoded output invalidated a newer IDR epoch");
    auto current=queue.pop(at);
    require(current&&queue.mark_output_ready(*current,at+1000),"Static refresh IDR failed to become ready");
    require(!queue.recovery_pending()&&!queue.recovery_expired(at+5000000),"Fresh static output retained old recovery deadline");
}
void older_presenter_cannot_rearm_newer_ready_output() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Presenter race baseline rejected");
    auto old=queue.pop(Local);require(old&&queue.mark_output_ready(*old,Local+1000),"Old presenter picture not ready");
    require(queue.push(picture(1,Source+20000),Local+20000),"New mapper picture rejected");
    auto newer=queue.pop(Local+20000);
    require(newer&&queue.mark_output_ready(*newer,Local+21000),"New mapper picture not ready");
    require(!queue.output_fresh(*old,Local+51000),"Expired old presenter remained displayable");
    require(queue.output_fresh(*newer,Local+52000)&&!queue.recovering()&&!queue.recovery_pending()&&queue.age_expired()==0,
        "Old presentation completion re-armed refresh after newer ready pixels");
    // The watermark only suppresses an older retry. It must not give the
    // latest ready output unlimited lifetime on a static desktop.
    require(!queue.output_fresh(*newer,Local+70001)&&queue.recovering()&&queue.age_expired()==1,
        "Newest ready picture escaped static-output expiry");
    require(queue.recovery_expired(Local+70001+hive::ViewerQueue::RecoveryTimeoutUs),"Newest output expiry lost the bounded refresh deadline");

    // Neither the prior epoch's watermark nor a prior session's larger
    // sequence number may suppress the next independent picture's expiry.
    require(queue.push(picture(2,Source+80000,true),Local+80000),"Replacement epoch IDR rejected");
    auto idr=queue.pop(Local+80000);require(bool(idr),"Replacement epoch IDR missing");
    require(!queue.output_fresh(*old,Local+80000)&&!queue.recovering(),"Old epoch re-armed a replacement IDR request");
    require(!queue.output_fresh(*idr,Local+130001)&&queue.recovering(),"New epoch inherited old output watermark");
    queue.reset();
    require(queue.push(picture(0,Source+200000,true,8),Local+200000),"New session after ready watermark rejected");
    auto reopened=queue.pop(Local+200000);require(bool(reopened),"New session picture missing");
    require(!queue.output_fresh(*reopened,Local+250001)&&queue.recovering(),"New session inherited a larger ready sequence watermark");
}
void epochs_and_reference_gaps() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Initial IDR rejected");
    auto old=queue.pop(Local);require(bool(old),"Initial IDR missing");
    for(uint64_t i=1;i<=4;++i)require(queue.push(picture(i,Source+i*1000),Local+i*1000),"Bounded frame rejected too soon");
    require(!queue.push(picture(5,Source+5000),Local+5000),"Queue exceeded compressed-frame limit");
    require(queue.size()==0&&queue.recovering(),"Overflow retained dependent pictures");
    require(queue.queue_overflows()==1&&queue.age_expired()==0,"Overflow reason misclassified");
    require(queue.push(picture(6,Source+6000,true),Local+6000),"Fresh overflow recovery IDR rejected");
    require(!queue.presentable(*old,Local+6000),"Old in-flight output survived queue epoch change");
    auto fresh=queue.pop(Local+6000);require(fresh&&queue.presentable(*fresh,Local+6000),"New epoch failed presentation");
    require(!queue.push(picture(8,Source+8000),Local+8000),"Missing reference did not require IDR");
    require(queue.recovering(),"Reference gap recovery not reported");
    require(queue.sequence_gaps()==1&&queue.queue_overflows()==1,"Sequence gap reason misclassified");
    require(queue.push(picture(10,Source+10000,true),Local+10000),"Independent IDR rejected after sequence gap");
    auto idr=queue.pop(Local+10000);require(idr&&queue.presentable(*idr,Local+10000),"IDR after gap failed");
    require(!queue.push(picture(9,Source+9000),Local+11000),"Old duplicate accepted");
    require(!queue.recovering(),"Harmless old duplicate invalidated current chain");
    // A newer IDR may arrive while the previous decode is still blocked. Its
    // completion must not clear that already available recovery picture.
    require(queue.push(picture(11,Source+11000),Local+11000),"Pre-stall frame rejected");
    auto stalled=queue.pop(Local+11000);require(bool(stalled),"Pre-stall frame missing");
    require(queue.push(picture(12,Source+80000,true),Local+80000),"IDR during in-flight stall rejected");
    require(!queue.presentable(*stalled,Local+80000),"Stalled predecessor remained presentable");
    require(queue.size()==1&&!queue.recovering(),"Late decode completion discarded newer recovery IDR");
    auto latest=queue.pop(Local+80000);require(latest&&queue.presentable(*latest,Local+80000),"Newer IDR could not recover stalled decode");
}
void session_and_legacy() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true,11),Local),"First session rejected");
    auto previous=queue.pop(Local);require(bool(previous),"First session frame missing");
    require(!queue.push(picture(0,1,true,12),Local+1000),"Unopened session accepted");
    queue.reset();
    require(queue.push(picture(0,1,true,12),Local+2000),"Reopened session retained old clock baseline");
    require(!queue.presentable(*previous,Local+2000),"Previous session in-flight output remained eligible");
    require(!queue.push(picture(1,Source+3000,true,11),Local+3000),"Late previous-session packet accepted");
    auto current=queue.pop(Local+2000);require(current&&queue.presentable(*current,Local+2000),"Current session failed after old packet");
    queue.reset();require(queue.push(picture(0,0,true,0),Local),"Legacy no-timestamp IDR rejected");
    auto legacy=queue.pop(Local);require(legacy&&queue.presentable(*legacy,Local+1000),"Legacy presentation failed");
    require(!queue.presentable(*legacy,Local+50001),"Legacy local queue age not enforced");
}

void intact_references_and_latest_presentation(){
    hive::ViewerQueue q;
    require(q.push(picture(0,Source,true),Local),"Initial IDR rejected");
    auto initial=q.pop(Local);require(initial&&q.mark_output_ready(*initial,Local),"Initial ready missing");
    // Simulated 180 ms transport jitter: valid P references are not corruption.
    // Catch up by decoding each; only the latest completed picture is displayed.
    for(uint64_t i=1;i<=18;++i){
        const auto arrival=Local+190000+i*1000;
        require(q.push(picture(i,Source+i*10000),arrival),"Jitter destroyed an intact reference chain");
        auto p=q.pop(arrival);require(p&&q.admit(*p,arrival+1000),"Bounded reference catch-up refused");
        require(p->epoch==initial->epoch,"Jitter changed the decoder epoch");
        require(q.mark_output_ready(*p,arrival+2000),"Latest complete jittered picture was suppressed");
    }
    require(!q.recovering()&&!q.discarded()&&!q.age_expired(),"Jitter requested an unnecessary IDR");
    // A local worker stalls beyond display budget. Decode the reference, but
    // never present it late; a following P picture completes recovery.
    require(q.push(picture(19,Source+300000),Local+300000),"Catch-up reference rejected");
    auto late=q.pop(Local+380000);require(late&&q.admit(*late,Local+381000),"Late intact reference not decoded");
    require(!q.output_fresh(*late,Local+382000),"Local stale output reached presentation");
    require(q.push(picture(20,Source+390000),Local+390000),"P after decode-only reference rejected");
    auto fresh=q.pop(Local+390000);
    require(fresh&&fresh->epoch==late->epoch&&q.mark_output_ready(*fresh,Local+391000),"Fresh P could not restore presentation");
    require(!q.recovery_pending()&&!q.recovering(),"Fresh P left recovery latched");
}
void severe_backlog_and_queue_budget(){
    hive::ViewerQueue q;
    require(q.push(picture(0,Source,true),Local),"Baseline rejected");q.pop(Local);
    require(!q.push(picture(1,Source+10000),Local+1010001),"Over-one-second TCP tail accepted");
    require(q.recovering()&&q.age_expired()==1,"Severe backlog omitted reference recovery");
    require(!q.push(picture(2,Source+20000,true),Local+1020001),"Old IDR reset transit baseline");
    require(q.push(picture(3,Source+1100000,true),Local+1100000),"Fresh IDR after severe backlog refused");
    auto ready=q.pop(Local+1100000);require(ready&&q.mark_output_ready(*ready,Local+1101000),"Fresh recovery failed");
    require(!q.recovery_pending(),"Recovery deadline was not cleared by actual ready output");
    q.reset();require(q.push(picture(0,Source,true),Local),"Worker baseline rejected");
    require(!q.pop(Local+hive::ViewerQueue::MaxReferenceAgeUs+1),"Unbounded worker stall accepted");
    require(q.recovering()&&q.feedback_queue_age(Local+1000002)==1000001,"Severe queued work lost capacity evidence");
    require(q.feedback_queue_age(Local+1250001)==0,"Old overload evidence did not expire");
    q.reset();require(q.push(picture(0,Source,true),Local),"Admission baseline rejected");
    auto p=q.pop(Local);require(p&&q.admit(*p,Local+80000),"Decoder-credit wait reset valid chain");
    require(q.feedback_queue_age(Local+81000)==80000,"Decoder-credit wait not reported");
    require(!q.admit(*p,Local+1000001),"Unbounded decoder-credit wait accepted");
    require(!q.admit(*p,Local+1300000)&&q.feedback_queue_age(Local+1300000)==0,"Invalidated reservation renewed stale pressure");
}
void source_timing_and_session_integrity(){
    hive::ViewerQueue q;
    require(q.push(completed_picture(0,Source,Source+4000,true),Local+4000),"Timed baseline rejected");q.pop(Local+4000);
    require(q.push(completed_picture(1,Source+10000,Source+90000),Local+90000),"Intrinsic source work became network delay");
    auto p=q.pop(Local+90000);require(p&&q.mark_output_ready(*p,Local+91000)&&q.transit_extra_us()==0,"Timed work failed");
    require(!q.push(completed_picture(2,Source+100000,Source+99999),Local+100000),"Negative source work accepted");
    require(!q.push(picture(3,Source+110000,true),Local+110000),"Missing provenance changed clock basis");
    require(!q.push(completed_picture(4,Source,Source+4000,true),Local+120000),"Backward completion accepted");
    require(!q.push(completed_picture(5,Source+130000,Source+130000+hive::MaxSourceWorkUs+1,true),Local+1130001),"Unbounded GPU work accepted");
    require(q.push(completed_picture(6,Source+1200000,Source+1204000,true),Local+1204000),"Malformed timing poisoned later recovery");
    p=q.pop(Local+1204000);require(p&&q.mark_output_ready(*p,Local+1205000),"Recovery timing failed");
    require(!q.push(completed_picture(7,Source+1210000,Source+1214000,false,99),Local+1214000),"Wrong session accepted");
    require(!q.push(completed_picture(6,Source+1210000,Source+1214000,true),Local+1214000),"Duplicate IDR accepted");
}
void slow_assembly_watchdog_and_clocks(){
    auto wire=[](uint64_t seq,uint64_t source,uint64_t first,uint64_t last,bool key){
        auto f=completed_picture(seq,source,source+1000,key);f.receive_started_us=first;f.receive_completed_us=last;return f;
    };
    hive::ViewerQueue q;
    require(q.push(wire(0,Source,Local,Local+10000000,true),Local+10000000),"Large initial packet cannot complete");
    auto first=q.pop(Local+10000000);require(first&&q.mark_output_ready(*first,Local+10001000),"Initial packet cannot become ready");
    require(q.push(wire(1,Source+11000000,Local+11000000,Local+11600000,false),Local+11600000),"Slow P serialization invalidated chain");
    auto p=q.pop(Local+11600000);require(p&&q.mark_output_ready(*p,Local+11601000),"Slow P not renderable");
    require(!q.output_fresh(*p,Local+11650001),"Wire timing excused local display stall");
    q.network_progress(7,100,Local+12200000);q.network_progress(99,200,Local+12500000);
    require(q.recovery_expired(Local+13650001),"Wrong session renewed deadline");
    q.network_progress(7,200,Local+13000000);require(!q.recovery_expired(Local+14000000),"Actual byte progress ignored");
    q.network_progress(7,200,Local+14500000);require(q.recovery_expired(Local+15000000),"Repeated counter invented progress");
    q.network_progress(7,100,Local+15500000);require(q.recovery_expired(Local+15500000),"Counter reset invented progress");
    q.network_progress(7,300,Local+41000000);require(q.recovery_expired(Local+41650001),"Trickling bytes bypassed finite deadline");
    q.reset();require(!q.push(wire(0,Source,Local+1,Local,true),Local),"Reversed receive timing accepted");
    q.reset();require(!q.push(wire(0,Source,Local,Local+100,true),Local),"Future receive timing accepted");
    q.reset();require(!q.push(wire(0,Source,0,Local,true),Local),"Partial receive timing accepted");
    hive::ViewerTransitClock clock;
    for(uint64_t i=0;i<360000;++i){clock.observe(Source+i*10000,Local+i*10001);require(clock.extra_age(Source+i*10000,Local+i*10001)<10000,"Clock drift accumulated as backlog");}
    // Replay falls progressively behind; no receive timestamp can excuse it.
    q.reset();bool rejected=false;
    for(uint64_t i=0;i<300;++i){const auto at=Local+i*14000;
        if(!q.push(picture(i,Source+i*10000,i%10==0),at)){rejected=true;continue;}
        require(!rejected,"Old IDR silently rebased delayed replay");auto frame=q.pop(at);require(frame&&q.mark_output_ready(*frame,at),"Accepted replay fixture failed");}
    require(rejected&&q.recovering(),"Unbounded slow replay accepted");
}
}
int main(){try{
service_samples_expire();present_gap_samples();normal_and_idle();decoded_expiry_static_refresh_and_epochs();older_presenter_cannot_rearm_newer_ready_output();epochs_and_reference_gaps();session_and_legacy();
intact_references_and_latest_presentation();severe_backlog_and_queue_budget();source_timing_and_session_integrity();slow_assembly_watchdog_and_clocks();
std::puts("PASS viewer: intact late references, latest local presentation deadline, bounded decode catch-up, severe backlog/overflow recovery, session/epoch isolation, source/wire provenance, watchdog progress and clock drift.");return 0;
}catch(const std::exception& e){std::printf("FAIL viewer: %s\n",e.what());return 1;}}
