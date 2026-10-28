#include "../src/source_queue.h"
#include <cstdio>
#include <stdexcept>
int main(){try{
    auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    auto frame=[](uint64_t sequence,uint64_t time,bool key=false){hive::EncodedFrame f;f.sequence=sequence;f.capture_time_us=time;f.keyframe=key;return f;};
    hive::SourceFrameQueue q;
    require(q.push(frame(0,1000000,true),1000010),"Initial IDR rejected");
    require(q.push(frame(1,1010000),1010010),"Contiguous frame rejected");
    require(!q.push(frame(2,1020000),1020010)&&q.empty()&&q.needs_keyframe(),"Overflow did not abandon reference chain");
    require(!q.push(frame(3,1030000),1030010),"Dependent frame survived lost reference");
    require(q.push(frame(4,1040000,true),1040010),"Fresh IDR did not recover");
    require(!q.take(1200000)&&q.needs_keyframe(),"Stalled output replayed old frame");
    require(!q.push(frame(5,1040000,true),1200000),"Old IDR cleared recovery");
    require(q.push(frame(6,1200000,true),1200001),"Fresh recovery IDR rejected");
    auto latest=q.take(1200002);require(latest&&latest->sequence==6&&q.empty(),"Recovery replayed an obsolete tail");
    require(q.push(frame(7,1210000),1210010),"Fresh P after recovery rejected");
    require(!q.push(frame(8,1000000),1210011)&&q.empty(),"Expired completion did not invalidate dependent queue");
    require(q.push(frame(9,1220000,true),1220001)&&q.push(frame(10,1230000,true),1230001),"Fresh IDR replacement failed");
    latest=q.take(1230002);require(latest&&latest->sequence==10&&q.empty(),"Fresh IDR left an obsolete predecessor to replay");
    require(q.push(frame(11,1300000,true),1440000,140000),"Intrinsic slow encoding became an endless IDR drop");
    require(!q.take(1460001)&&q.needs_keyframe(),"Slow encoding allowed additional stale output backlog");
    require(q.discarded()==10,"Source discard diagnostics missed expired, overflowed, dependent or superseded pictures");
    hive::SourceFrameQueue reserved;
    require(reserved.reserve_completion()&&reserved.push_reserved(frame(0,2000000,true),2000010),"Reserved initial IDR failed");
    require(reserved.take(2000020).has_value(),"Reserved initial IDR was not delivered");
    // Two GPU jobs may run together. After the first completes, the second
    // still owns output space even if Encoder::receive has freed its GPU slot
    // and is waiting for the source mutex. It cannot admit a third P picture.
    require(reserved.reserve_completion()&&reserved.reserve_completion()&&!reserved.reserve_completion(),"Empty output queue did not admit exactly two GPU jobs");
    require(reserved.push_reserved(frame(1,2010000),2010010)&&reserved.size()==1&&reserved.reserved()==1,"First concurrent P completion lost its reservation");
    require(!reserved.can_reserve()&&!reserved.reserve_completion(),"One completed plus one in-transit picture admitted an overflowing GPU job");
    require(reserved.push_reserved(frame(2,2020000),2020010)&&reserved.size()==2&&!reserved.reserved(),"Second concurrent P completion reset the reference chain");
    require(!reserved.needs_keyframe()&&!reserved.discarded(),"Bounded P completion burst forced unnecessary IDR recovery");
    latest=reserved.take(2020020);require(latest&&latest->sequence==1&&reserved.reserve_completion(),"Delivery did not release one completion slot");
    latest=reserved.take(2020030);require(latest&&latest->sequence==2,"Concurrent completion burst reordered P pictures");
    require(reserved.push_reserved(frame(3,2030000),2030010)&&reserved.reserve_completion(),"Reserved next P picture failed");
    // Expiring the queued predecessor must invalidate its pending dependent,
    // without accidentally releasing that dependent's reservation early.
    require(!reserved.take(2140000)&&reserved.needs_keyframe()&&reserved.reserved()==1,"Expiry lost an accepted GPU completion reservation");
    require(!reserved.push_reserved(frame(4,2140000),2140010)&&!reserved.reserved(),"Expired P chain accepted a pending dependent or leaked its reservation");
    require(reserved.reserve_completion(),"Failed submit could not reserve output");reserved.cancel_completion();
    require(!reserved.reserved()&&reserved.reserve_completion()&&reserved.push_reserved(frame(5,2150000,true),2150010),"Canceled submit prevented fresh IDR recovery");
    latest=reserved.take(2150020);require(latest&&latest->sequence==5&&reserved.empty()&&!reserved.needs_keyframe(),"Reserved recovery replayed an obsolete tail");
    hive::SourceFrameQueue timed;
    auto complete=frame(0,3000000,true);complete.source_ready_us=3140000;
    // The immutable completion timestamp, not a caller-supplied processing
    // duration, establishes how much of this frame's age was actual work.
    require(timed.push(complete,3140001),"Source completion provenance lost intrinsic slow work allowance");
    latest=timed.take(3140002);require(latest&&latest->source_ready_us==3140000,"Source delivery restamped completion provenance");
    complete.sequence=1;complete.capture_time_us=3200000;complete.source_ready_us=3340000;
    require(!timed.push(complete,3360001,1000000)&&timed.needs_keyframe(),"Caller work estimate hid a completed frame's downstream backlog");
    complete.sequence=2;complete.capture_time_us=3400000;complete.source_ready_us=3400000+hive::MaxSourceWorkUs+1;
    require(!timed.push(complete,complete.source_ready_us),"Unbounded intrinsic source work entered delivery queue");
    complete.sequence=3;complete.source_ready_us=complete.capture_time_us-1;
    require(!timed.push(complete,3400001),"Source completion preceding capture accepted");
    complete.sequence=4;complete.source_ready_us=3401000;
    require(!timed.push(complete,3400999),"Future completion entered local delivery queue");
    hive::SourceFrameQueue paced;
    require(paced.push(frame(0,5000000,true),5000010)&&paced.begin_delivery(5000020).has_value(),"Delivery handoff setup failed");
    require(paced.reserve_completion()&&!paced.can_reserve(),"Output handoff released its slot before downstream took ownership");
    paced.cancel_completion();
    paced.finish_delivery();require(paced.can_reserve(),"Output handoff leaked credit");
    paced.observe_credit(false,5000100);paced.observe_credit(false,5100100);
    require(paced.reserve_completion()&&!paced.can_reserve(),"Sustained blocked output still admitted speculative second GPU job");
    paced.cancel_completion();paced.observe_credit(true,5200100);
    require(paced.reserve_completion()&&!paced.can_reserve(),"One ready notification prematurely restored speculative work");
    paced.cancel_completion();paced.observe_credit(false,9000000);paced.observe_credit(true,9001000);
    paced.observe_credit(true,10100100);
    require(paced.reserve_completion()&&paced.reserve_completion()&&!paced.can_reserve(),"Healthy recovery failed to restore asynchronous GPU capacity");
    std::puts("PASS source output queue: bounded GPU reservations, atomic delivery handoff, slow-output serialization and healthy capacity recovery, age expiry without catch-up playback.");return 0;
}catch(const std::exception& e){std::printf("FAIL source output queue: %s\n",e.what());return 1;}}
