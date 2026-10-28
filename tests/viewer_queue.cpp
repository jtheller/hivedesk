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
void prepared_picture_survives_new_compressed_arrival() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Initial prepared picture missing");
    auto prepared=queue.pop(Local);
    require(prepared&&queue.presentable(*prepared,Local),"Initial conversion could not start");
    // The stream is healthy, but each next packet arrives during conversion.
    // It is worth skipping conversion of superseded work; cancelling an already
    // prepared picture here would instead suppress every visible update.
    for(uint64_t i=1;i<=100;++i){
        const auto now=Local+i*5000;
        require(queue.push(picture(i,Source+i*5000),now),"New compressed picture rejected");
        require(!queue.presentable(*prepared,now),"Obsolete conversion was not skipped");
        require(queue.fresh(*prepared,now),"Fresh prepared pixels lost to an undecoded packet");
        require(queue.mark_ready(*prepared,now),"Prepared display could not mark fresh progress");
        prepared=queue.pop(now);
        require(prepared&&queue.presentable(*prepared,now),"Newest picture could not start conversion");
    }
    require(!queue.recovering()&&queue.discarded()==0,"Healthy prepared pictures triggered reference recovery");
    // Keeping prepared output eligible must never bypass the original age or
    // epoch bounds, even when the queue contains a newer independent picture.
    require(queue.push(picture(101,Source+505000,true),Local+505000),"Replacement IDR rejected");
    require(!queue.fresh(*prepared,Local+505000),"Prepared pixels crossed an IDR epoch");
    prepared=queue.pop(Local+505000);
    require(prepared&&!queue.fresh(*prepared,Local+555001),"Prepared pixels escaped the hard age bound");
    require(queue.recovering(),"Expired prepared pixels did not request reference recovery");
}
void worker_and_decoder_stalls() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Initial IDR missing");
    require(!queue.pop(Local+50001),"Expired queued frame escaped dequeue guard");
    require(queue.empty()&&queue.recovering(),"Worker stall failed to request IDR");
    require(queue.age_expired()==1&&queue.sequence_gaps()==0&&queue.queue_overflows()==0,"Worker expiry reason misclassified");
    require(!queue.push(picture(1,Source+60000),Local+60000),"Reference accepted after dropped chain");
    require(queue.push(picture(2,Source+70000,true),Local+70000),"Fresh recovery IDR rejected");
    auto active=queue.pop(Local+70000);require(bool(active),"Recovery IDR missing");
    require(!queue.presentable(*active,Local+120001),"Decoder stall produced stale presentation");
    require(queue.recovering(),"Decoder stall failed to request fresh IDR");
}
void decoded_expiry_preserves_references() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Decoded expiry baseline rejected");
    auto decoded=queue.pop(Local);require(decoded&&queue.fresh(*decoded,Local),"Initial decode admission failed");
    require(queue.push(picture(1,Source+60000),Local+60000),"Fresh P after slow decoded output rejected");
    require(!queue.output_fresh(*decoded,Local+60000),"Expired decoded output was presentable");
    require(queue.size()==1&&queue.recovering()&&queue.recovery_pending(),"Decoded expiry discarded a valid queued reference or omitted refresh");
    require(queue.age_expired()==1&&queue.discarded()==0&&queue.sequence_gaps()==0,"Display-only expiry was treated as reference loss");
    require(!queue.output_fresh(*decoded,Local+61000)&&queue.age_expired()==1,"Repeated presentation polling inflated expiry count");
    auto next=queue.pop(Local+61000);
    require(next&&next->epoch==decoded->epoch&&queue.fresh(*next,Local+61000),"Decoded expiry changed the reference epoch");
    require(queue.mark_output_ready(*next,Local+62000),"Fresh dependent P could not finish display recovery");
    require(!queue.recovering()&&!queue.recovery_pending()&&!queue.recovery_expired(Local+5000000),"Fresh P left a spurious IDR request or deadline armed");

    // The display-only exemption never applies to compressed pictures waiting
    // for admission: missing one of those still invalidates the entire chain.
    require(queue.push(picture(2,Source+70000),Local+70000),"Pre-expiry dependent picture rejected");
    require(!queue.output_fresh(*next,Local+111001),"Old decoded P unexpectedly remained fresh");
    require(!queue.pop(Local+120001)&&queue.empty()&&queue.recovering(),"Expired encoded queue survived display refresh state");
    require(!queue.push(picture(3,Source+130000),Local+130000),"P picture accepted after a real compressed reference loss");
    require(queue.push(picture(4,Source+140000,true),Local+140000),"Fresh IDR could not recover a real compressed expiry");
    auto recovery=queue.pop(Local+140000);
    require(recovery&&recovery->epoch!=next->epoch&&queue.mark_output_ready(*recovery,Local+141000),"Real encoded expiry failed to advance/recover epoch");
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
void tcp_stall_and_burst() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Initial IDR rejected");
    auto initial=queue.pop(Local);require(initial&&queue.presentable(*initial,Local),"Initial presentation failed");
    // The receive thread wakes 200 ms late. Arrival timestamps all look new;
    // capture intervals prove this batch is old, including its periodic IDRs.
    for(uint64_t i=1;i<=15;++i)
        require(!queue.push(picture(i,Source+i*10000,i%5==0),Local+210000+i*100),"Old TCP burst accepted");
    require(queue.recovering()&&queue.empty(),"Stale IDR incorrectly cleared recovery");
    require(queue.push(picture(16,Source+220000,true),Local+222000),"Fresh IDR after TCP drain rejected");
    auto fresh=queue.pop(Local+222000);
    require(fresh&&queue.presentable(*fresh,Local+223000),"Fresh recovery frame suppressed");
    require(!queue.recovering(),"Recovery remained latched after fresh IDR");
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
void slow_replay_and_clock_drift() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Initial IDR rejected");
    queue.pop(Local);bool rejected=false;
    for(uint64_t i=1;i<=20;++i){
        const auto local=Local+i*10000;
        if(!queue.push(picture(i,Source+i*1000,i%5==0),local)){rejected=true;continue;}
        auto frame=queue.pop(local);
        require(frame&&queue.presentable(*frame,local),"Unexpected accepted replay state");
        require(!rejected,"Old slow replay resumed without a fresh IDR");
    }
    require(rejected&&queue.recovering(),"Progressively delayed replay was not bounded");
    hive::ViewerTransitClock clock;
    // An hour at 100 Hz with 100 ppm relative oscillator drift should not be
    // mistaken for a growing network queue. No sleeps or wall-clock dependency.
    for(uint64_t i=0;i<360000;++i){
        const auto source=Source+i*10000,local=Local+i*10001;
        clock.observe(source,local);
        require(clock.extra_age(source,local)<10000,"Ordinary clock drift accumulated as backlog");
    }
}
void bounded_recovery_requires_new_session() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Initial recovery baseline rejected");
    auto first=queue.pop(Local);require(first&&queue.mark_ready(*first,Local),"Initial recovery baseline not ready");
    // A true latency increase and a still-buffered stream are indistinguishable
    // here. Never silently rebase within this session, even on repeated IDRs.
    for(uint64_t i=1;i<=7;++i){
        const auto time=i*300000;
        require(!queue.push(picture(i,Source+time,true),Local+time+60000),"Stale IDR silently rebased transit floor");
    }
    const auto began=Local+360000;
    require(!queue.recovery_expired(began+hive::ViewerQueue::RecoveryTimeoutUs-1),"Recovery timeout fired early");
    require(queue.recovery_expired(began+hive::ViewerQueue::RecoveryTimeoutUs),"Repeated stale IDRs deferred recovery timeout");
    // The selected-stream restart supplies a new session ID and newly captured
    // frame. Old wire packets cannot establish the baseline for this new epoch.
    queue.reset();
    require(queue.push(picture(0,Source+3000000,true,8),Local+3060000),"New-session IDR failed to establish baseline");
    require(!queue.push(picture(8,Source+3000000,true,7),Local+3060001),"Old-session packet crossed restart barrier");
    auto fresh=queue.pop(Local+3060000);
    require(fresh&&queue.mark_ready(*fresh,Local+3061000),"New-session recovery frame not ready");
    require(!queue.recovery_expired(Local+10000000),"New session retained expired recovery timer");

    queue.reset();queue.push(picture(0,Source,true),Local);queue.pop(Local);
    require(!queue.push(picture(1,Source+10000),Local+70000),"Stale reference accepted");
    require(queue.push(picture(2,Source+80000,true),Local+80000),"Fresh pending IDR rejected");
    require(queue.recovery_pending(),"An unrendered IDR prematurely cleared recovery deadline");
    auto ready=queue.pop(Local+80000);
    require(ready&&queue.mark_ready(*ready,Local+81000),"Fresh IDR could not finish recovery");
    require(!queue.recovery_pending()&&!queue.recovery_expired(Local+10000000),"Completed fresh recovery left timeout armed");
}
void source_work_is_not_transport_backlog() {
    hive::ViewerQueue queue;
    require(queue.push(completed_picture(0,Source,Source+4000,true),Local+4000),"Timed initial IDR rejected");
    auto first=queue.pop(Local+4000);require(first&&queue.mark_ready(*first,Local+5000),"Timed initial IDR missing");
    // An 80 ms GPU job after a 4 ms job is completed work, not 76 ms of TCP
    // backlog. Later fast frames must keep the same reference chain intact.
    require(queue.push(completed_picture(1,Source+10000,Source+90000),Local+90000),"Slow intrinsic encoding was mistaken for queued video");
    auto slow=queue.pop(Local+90000);require(slow&&queue.mark_ready(*slow,Local+91000),"Freshly completed slow picture suppressed");
    require(queue.transit_extra_us()==0&&!queue.recovering(),"Encode duration polluted timed transport excess");
    require(queue.push(completed_picture(2,Source+100000,Source+104000),Local+104000),"Fast frame following slow encoding rejected");
    auto fast=queue.pop(Local+104000);require(fast&&queue.mark_ready(*fast,Local+105000),"Fast frame failed after slow encoding");
    require(queue.discarded()==0&&queue.age_expired()==0,"Valid source work caused reference recovery");
    require(!queue.fresh(*fast,Local+154001)&&queue.recovering(),"Timed frame bypassed the 50 ms local residence limit");

    // A slow FIRST frame cannot teach an artificially low capture baseline that
    // makes ordinary following frames look stale or undercount actual transit.
    queue.reset();
    require(queue.push(completed_picture(0,Source,Source+300000,true),Local+300000),"Slow first timed IDR rejected");
    auto initial=queue.pop(Local+300000);require(initial&&queue.mark_ready(*initial,Local+301000),"Slow first timed IDR not usable");
    require(queue.push(completed_picture(1,Source+310000,Source+314000),Local+314000),"First-frame work poisoned later freshness");
    queue.pop(Local+314000);
    require(!queue.push(completed_picture(2,Source+320000,Source+324000,true),Local+384001),"Slow initial encode hid subsequent TCP backlog");
    require(queue.recovering()&&queue.age_expired()==1,"Timed TCP expiry failed to preserve reference recovery");
}
void timed_transport_and_reference_guards() {
    hive::ViewerQueue queue;
    require(queue.push(completed_picture(0,Source,Source+4000,true),Local+4000),"Timed TCP baseline rejected");
    queue.pop(Local+4000);
    for(uint64_t i=1;i<=15;++i)
        require(!queue.push(completed_picture(i,Source+i*10000,Source+i*10000+4000,i%5==0),Local+214000+i*100),"Timed stale TCP burst accepted");
    require(queue.recovering()&&queue.empty(),"Timed stale IDR reset transit floor");
    require(queue.push(completed_picture(16,Source+220000,Source+224000,true),Local+226000),"Timed fresh IDR rejected after TCP drain");
    auto fresh=queue.pop(Local+226000);require(fresh&&queue.mark_ready(*fresh,Local+227000),"Timed recovery did not finish");
    require(!queue.push(completed_picture(18,Source+230000,Source+234000),Local+236000),"Timed provenance bypassed a lost reference");
    require(queue.sequence_gaps()==1&&queue.recovering(),"Timed lost reference did not require IDR");
    // New source timestamps must not let a stream of stale recovery IDRs keep
    // a selected connection alive forever or reset its old transit baseline.
    for(uint64_t i=19;i<=26;++i){
        const auto at=300000+(i-19)*300000;
        require(!queue.push(completed_picture(i,Source+at,Source+at+4000,true),Local+at+64000),"Timed stale recovery IDR became fresh");
    }
    require(queue.recovery_expired(Local+236000+hive::ViewerQueue::RecoveryTimeoutUs),"Timed recovery lost the two-second session barrier");
}
void malformed_or_unbounded_source_work() {
    hive::ViewerQueue queue;
    require(queue.push(completed_picture(0,Source,Source+4000,true),Local+4000),"Provenance baseline rejected");queue.pop(Local+4000);
    require(!queue.push(completed_picture(1,Source+10000,Source+9999),Local+14000),"Completion before capture accepted");
    require(!queue.push(picture(2,Source+20000,true),Local+24000),"Missing provenance switched a timed stream to a new clock basis");
    require(!queue.push(completed_picture(3,Source+1000,Source+3000,true),Local+25000),"Backward completion timestamp accepted");
    require(!queue.push(completed_picture(4,Source+30000,Source+30000+hive::MaxSourceWorkUs+1,true),Local+1040000),"Unbounded intrinsic source work accepted");
    require(queue.push(completed_picture(5,Source+1050000,Source+1054000,true),Local+1054000),"Invalid source timing poisoned recovery baseline");
    auto recovered=queue.pop(Local+1054000);require(recovered&&queue.mark_ready(*recovered,Local+1055000),"Provenance rejection prevented fresh recovery");
    // Exactly the finite source-work ceiling is admissible, without enlarging
    // either the compressed queue or the post-arrival lifetime.
    queue.reset();
    require(queue.push(completed_picture(0,Source,Source+hive::MaxSourceWorkUs,true),Local+hive::MaxSourceWorkUs),"Finite source-work boundary rejected");
    require(!queue.pop(Local+hive::MaxSourceWorkUs+hive::ViewerQueue::MaxAgeUs+1),"Source-work allowance leaked into local queue age");

    queue.reset();require(queue.push(picture(0,Source,true),Local),"Legacy mixed-version baseline failed");queue.pop(Local);
    require(!queue.push(completed_picture(1,Source+10000,Source+90000),Local+90000),"Mid-session provenance changed a legacy clock baseline");
}
void recent_compressed_backlog_feedback() {
    hive::ViewerQueue queue;
    require(queue.push(picture(0,Source,true),Local),"Backlog baseline rejected");
    auto first=queue.pop(Local+25000);
    require(first&&queue.mark_ready(*first,Local+26000),"A bounded queued picture could not recover");
    require(queue.queue_age(Local+30000)==0&&queue.feedback_queue_age(Local+30000)==25000,
        "Dequeue erased real local backlog before the next feedback report");
    for(uint64_t at=Local+35000;at<Local+275000;at+=5000)
        require(queue.feedback_queue_age(at)==25000,"Recent queue evidence vanished too early");
    require(queue.feedback_queue_age(Local+275000)==0,"Polling kept idle queue evidence alive past 250ms");
    require(queue.push(picture(1,Source+300000),Local+300000),"New work failed after backlog evidence expired");
    auto current=queue.pop(Local+301000);
    require(current&&queue.mark_ready(*current,Local+302000)&&queue.feedback_queue_age(Local+302000)==0,
        "Healthy work inherited an expired backlog ceiling");

    queue.reset();
    for(uint64_t i=0;i<4;++i)
        require(queue.push(picture(i,Source+i*10000,i==0),Local+i*10000),"Overflow evidence setup failed");
    require(!queue.push(picture(4,Source+40000),Local+40000)&&queue.queue_overflows()==1,
        "Compressed queue ownership bound changed");
    require(queue.empty()&&queue.feedback_queue_age(Local+100000)==40000,
        "Overflow flushed the local backlog signal before feedback");
    require(queue.feedback_queue_age(Local+290000)==0,"Overflow evidence never expired");

    queue.reset();require(queue.push(picture(0,Source,true),Local),"Expiry evidence setup failed");
    require(!queue.pop(Local+50001)&&queue.age_expired()==1&&queue.feedback_queue_age(Local+60000)==50001,
        "Encoded freshness expiry hid queued work or admitted an old picture");
    queue.reset();require(queue.feedback_queue_age(Local+60000)==0,"Session reset retained old queue pressure");

    require(queue.push(picture(0,Source,true),Local),"Admission backlog baseline rejected");
    auto reserved=queue.pop(Local);require(bool(reserved),"Admission backlog fixture could not remove its compressed picture");
    require(queue.empty()&&queue.admit(*reserved,Local+25000)&&queue.feedback_queue_age(Local+30000)==25000,
        "Compressed picture waiting outside the queue lost its decoder-credit backlog");
    require(!queue.admit(*reserved,Local+50001)&&queue.feedback_queue_age(Local+60000)==50001,
        "Expired decoder-credit reservation bypassed strict admission or erased pressure");
    require(!queue.admit(*reserved,Local+200000)&&queue.feedback_queue_age(Local+300001)==0,
        "Retired reservation renewed stale backlog evidence");
    queue.reset();

    // Long source work and display-only age do not imply local compressed
    // backlog. Neither is allowed to recreate the old latency-derived cap.
    require(queue.push(completed_picture(0,Source,Source+80000,true),Local+80000),"Slow source work rejected");
    auto decoded=queue.pop(Local+80000);require(decoded&&queue.feedback_queue_age(Local+80000)==0,"Source work became local backlog");
    require(!queue.output_fresh(*decoded,Local+140000)&&queue.feedback_queue_age(Local+140000)==0,
        "Presentation-only expiry became compressed backlog evidence");
    queue.reset();require(queue.push(picture(0,Source,true),Local),"Transport-only baseline rejected");queue.pop(Local);
    require(!queue.push(picture(1,Source+10000,true),Local+90000)&&queue.feedback_queue_age(Local+90000)==0,
        "Stale TCP transit without local queued work became decoder capacity pressure");
}
void slow_packet_assembly_and_progress(){
    hive::ViewerQueue q;
    auto wire=[](uint64_t seq,uint64_t source,uint64_t start,uint64_t end,bool key){
        auto f=completed_picture(seq,source,source+1000,key);
        f.receive_started_us=start;f.receive_completed_us=end;return f;
    };
    require(q.push(wire(0,Source,Local,Local+10000000,true),Local+10000000),"Initial large IDR assembly rejected");
    auto first=q.pop(Local+10000000);require(first&&q.mark_output_ready(*first,Local+10001000),"Large IDR never became ready");
    require(q.push(wire(1,Source+11000000,Local+11000000,Local+11600000,false),Local+11600000),"Fresh slow-serialized P picture mistaken for queued playback");
    auto next=q.pop(Local+11600000);require(next&&q.mark_output_ready(*next,Local+11601000),"Slow P picture did not render");
    require(!q.output_fresh(*next,Local+11650001),"Wire timing excused a slow local decoder/Present");
    // Assembly does not forgive time waiting BEHIND a previous packet.
    require(!q.push(wire(2,Source+11020000,Local+11600000,Local+12200000,false),Local+12200000),"Wire timing hid an already-old TCP tail");
    q.network_progress(7,100,Local+12200000);
    q.network_progress(99,200,Local+12500000);
    require(q.recovery_expired(Local+13650001),"Another session renewed recovery");
    q.network_progress(7,200,Local+13000000);
    require(!q.recovery_expired(Local+14000000),"New video bytes did not renew recovery");
    q.network_progress(7,200,Local+14500000);
    require(q.recovery_expired(Local+15000000),"Repeated counter poll renewed recovery without bytes");
    q.network_progress(7,50,Local+15500000);
    require(q.recovery_expired(Local+15500000),"Counter reset invented receive progress");
    q.network_progress(7,100,Local+41000000);
    require(q.recovery_expired(Local+41650001),"Trickling peer bypassed finite recovery ceiling");
    q.reset();auto invalid=wire(0,Source,Local+1,Local,true);
    require(!q.push(invalid,Local),"Reversed receiver timing accepted");
    q.reset();invalid=wire(0,Source,Local,Local+100,true);
    require(!q.push(invalid,Local),"Future receive completion accepted");
    q.reset();invalid=wire(0,Source,0,Local,true);
    require(!q.push(invalid,Local),"Partial receive timing accepted");
}
}
int main(){try{
    slow_packet_assembly_and_progress();
    service_samples_expire();present_gap_samples();normal_and_idle();prepared_picture_survives_new_compressed_arrival();worker_and_decoder_stalls();
    decoded_expiry_preserves_references();decoded_expiry_static_refresh_and_epochs();older_presenter_cannot_rearm_newer_ready_output();tcp_stall_and_burst();
    epochs_and_reference_gaps();session_and_legacy();slow_replay_and_clock_drift();bounded_recovery_requires_new_session();
    source_work_is_not_transport_backlog();timed_transport_and_reference_guards();malformed_or_unbounded_source_work();recent_compressed_backlog_feedback();
    std::puts("PASS viewer freshness: bounded queue/age; source completion provenance separates intrinsic GPU work from TCP backlog; malformed/nonmonotonic timing and finite work bound; legacy policy preserved; decoded output expiry retains references and requests bounded static refresh; stalled worker and decode; stale TCP burst/IDRs; reference recovery; epoch/session isolation; slow replay; unrelated clocks and drift; two-second recovery session barrier.");
    return 0;
}catch(const std::exception& problem){std::printf("FAIL viewer freshness: %s\n",problem.what());return 1;}}
