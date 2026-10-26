#include "../src/viewer_queue.h"
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
hive::EncodedFrame picture(uint64_t sequence,uint64_t capture,bool keyframe=false,uint64_t session=7) {
    hive::EncodedFrame frame;frame.sequence=sequence;frame.capture_time_us=capture;
    frame.keyframe=keyframe;frame.session=session;frame.data={1};return frame;
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
}
int main(){try{
    service_samples_expire();present_gap_samples();normal_and_idle();prepared_picture_survives_new_compressed_arrival();worker_and_decoder_stalls();tcp_stall_and_burst();
    epochs_and_reference_gaps();session_and_legacy();slow_replay_and_clock_drift();bounded_recovery_requires_new_session();
    std::puts("PASS viewer freshness: bounded queue/age; stalled worker and decode; stale TCP burst/IDRs; reference recovery; epoch/session isolation; slow replay; unrelated clocks and drift; two-second recovery session barrier.");
    return 0;
}catch(const std::exception& problem){std::printf("FAIL viewer freshness: %s\n",problem.what());return 1;}}
