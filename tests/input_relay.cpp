#include "../src/input_relay.h"
#include "../src/input_send_window.h"
#include <cstdio>
#include <stdexcept>
using namespace hive;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
    RelayRoute c{"c",42,3},d{"d",43,4};RelayRouter router;
    auto p=router.plan({InputKind::Key,30,0},c,true,false);require(p.forward&&!p.local,"key duplicated locally");
    p=router.plan({InputKind::Key,30,1},c,false,false);require(p.forward&&!p.local,"key up lost its original destination on focus loss");
    router.plan({InputKind::Key,30,0},c,true,false);
    p=router.plan({InputKind::Key,30,1},d,true,false);require(!p.forward&&!p.local,"old key up went to new session");
    router.plan({InputKind::Button,1,0},c,true,false);require(router.remote_drag(c),"remote drag not retained");
    p=router.plan({InputKind::Button,1,1},c,false,false);require(p.forward&&!p.local&&!router.remote_drag(c),"outside drag release lost");
    router.plan({InputKind::Button,1,0},c,false,false);require(router.local_drag(),"local drag ownership lost");
    p=router.plan({InputKind::Button,1,1},c,true,false);require(p.local&&!p.forward,"local down/remote up split");
    p=router.plan({InputKind::Relative,17,-8},c,true,false);require(p.forward&&p.event.a==17&&p.event.b==-8,"raw relative displacement altered");
    p=router.plan({InputKind::Wheel,120,1},c,true,false);require(p.forward&&!p.local,"wheel duplicated locally");
    p=router.plan({InputKind::Key,30,0},c,true,true);require(!p.forward&&!p.local,"input during return escaped pause");
    p=router.plan({InputKind::ReleaseAll},c,false,true);require(p.forward&&p.local,"release barrier did not cross paused layer");
    router.plan({InputKind::Key,30,0},c,true,false);router.invalidate_remote();
    p=router.plan({InputKind::Key,30,0},c,true,false);require(!p.forward&&!p.local,"held repeat reacquired focus after release");
    p=router.plan({InputKind::Key,30,1},c,true,false);require(!p.forward&&!p.local,"stale key release leaked to regained viewer");
    p=router.plan({InputKind::Key,30,0},c,true,false);require(p.forward,"fresh press did not reacquire viewer");
    router.invalidate_remote();
    p=router.plan({InputKind::Key,30,1},{},false,false);require(!p.forward&&!p.local,"Invalidated key forwarded to empty route");
    router.reset();
    InputPacket a{11,1,100,{InputKind::Absolute,10,20}},b{11,2,110,{InputKind::Key,30,0}};
    RelayLedger ledger;require(ledger.push(a,true,c,8)&&ledger.push(b,false,c,9),"ledger admission");
    ledger.local({11,1,100,2,3,true});require(ledger.drain().empty(),"ACK before downstream acceptance");
    ledger.remote("wrong",{42,9,200,4,5,true});require(ledger.drain().empty(),"wrong peer ACK accepted");
    ledger.remote("c",{41,9,200,4,5,true});require(ledger.drain().empty(),"wrong session ACK accepted");
    ledger.remote("c",{42,9,200,4,5,true});const auto done=ledger.drain();
    require(done.size()==1&&done[0].sequence==2&&done[0].capture_time_us==110&&done[0].host_queue_us==4,"cumulative child ACK expanded into stale per-event ACKs");
    require(ledger.push(a,true,c,10)&&ledger.push(b,true),"ledger mixed admission");
    ledger.local({11,2,110,7,9,true});require(ledger.drain().empty(),"local ACK overtook pending child input");
    ledger.cancel_remote();require(ledger.drain().size()==1,"return/handoff stranded upstream ACK");
    ledger.push(a,false,c,11);ledger.remote("c",{42,11,200,0,0,false});require(!ledger.drain()[0].injected,"child failure hidden");
    {
        // A delayed cumulative completion must free the original credit window
        // once, without fabricating an RTT sample for every old motion event.
        InputSendWindow origin;origin.reset(11);
        for(uint64_t i=1;i<=8;++i){
            InputPacket motion{11,i,1000+i,{InputKind::Absolute,int(i),20}};
            require(origin.enqueue(motion,1000+i),"origin admission");origin.take(1100+i);
            require(ledger.push(motion,false,c,100+i),"burst relay admission");
        }
        ledger.remote("c",{42,108,2000,2,3,true});const auto batch=ledger.drain();
        require(batch.size()==1&&batch[0].sequence==8&&batch[0].capture_time_us==1008,"cumulative completion amplified across relay");
        require(origin.acknowledge(batch[0],201108),"cumulative origin ACK rejected");
        const auto stats=origin.snapshot(201108);
        require(stats.acknowledged==8&&!stats.inflight&&stats.samples==1&&stats.ack_roundtrip_p50_us==200000,"ACK compression corrupted credits or RTT clock");
        ledger.push({11,9,1009,{InputKind::Key,30,0}},false,c,109);
        ledger.push({11,10,1010,{InputKind::ReleaseAll}},false,c,110);
        ledger.remote("c",{42,109,2001,0,0,false});ledger.remote("c",{42,110,2002,0,0,true});
        const auto failed=ledger.drain();
        require(failed.size()==2&&!failed[0].injected&&failed[0].sequence==9&&failed[1].injected,"successful release hid prior rejected input");
    }
    for(unsigned i=0;i<512;++i)require(ledger.push(a,true),"early ledger overflow");require(!ledger.push(a,true),"unbounded relay ledger");ledger.reset();
    require(input_append(a,input_node("A"))&&input_append(a,input_node("B"))&&!input_append(a,input_node("A")),"A-B-A loop accepted");
    for(unsigned i=0;i<14;++i)require(input_append(a,input_node("hop"+std::to_string(i))),"early hop limit");require(!input_append(a,999),"hop limit ignored");
    InputSendWindow window;window.reset(11);a.path={};a.path_size=0;input_append(a,1);
    b=a;b.sequence=2;b.path[0]=2;
    require(window.enqueue(a,120)&&window.enqueue(b,121)&&window.snapshot(122).queued==2,"motion from different paths coalesced");
    std::puts("PASS relay: held-edge ownership, drag release, raw relative/wheel, return barrier, ordered local/child ACKs, telemetry, stale peers/sessions, cancellation, bounded storage and loops");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
