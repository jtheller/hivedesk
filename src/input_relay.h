#pragma once
#include "hivedesk/input_channel.h"
#include <deque>
#include <vector>
#include <string>
#include <algorithm>

namespace hive {
struct RelayRoute {
    std::string peer;uint64_t session=0,generation=0;
    bool operator==(const RelayRoute&)const=default;
    explicit operator bool()const{return session&&!peer.empty();}
};
struct RelayPlan {RelayRoute route;InputEvent event;bool local=true,forward=false;};
// One incoming controller; called under InputCapture's snapshot mutex. Held
// edges retain their destination even if focus moves between local UI/viewer.
class RelayRouter {
    struct Held {bool down=false,remote=false;RelayRoute route;};
    std::array<Held,512> keys_{};std::array<Held,6> buttons_{};
public:
    void reset(){keys_={};buttons_={};}
    void invalidate_remote(){for(auto& k:keys_)if(k.down&&k.remote)k.route={};for(auto& b:buttons_)if(b.down&&b.remote)b.route={};}
    bool remote_drag(const RelayRoute& route)const{if(!route)return false;for(const auto& b:buttons_)if(b.down&&b.remote&&b.route==route)return true;return false;}
    bool local_drag()const{for(const auto& b:buttons_)if(b.down&&!b.remote)return true;return false;}
    RelayPlan plan(InputEvent event,const RelayRoute& current,bool candidate,bool paused){
        RelayPlan p{current,event,true,false};
        if(event.kind==InputKind::ReleaseAll){reset();p.forward=bool(current);return p;}
        if(paused){p.local=false;return p;}
        Held* held=nullptr;
        if(event.kind==InputKind::Key&&event.a>0&&event.a<256)held=&keys_[size_t(event.a)+(event.c?256:0)];
        if(event.kind==InputKind::Button&&event.a>0&&event.a<6)held=&buttons_[size_t(event.a)];
        if(held){
            if(!event.b&&!held->down)*held={true,candidate,current};
            if(held->down){p.forward=bool(current)&&held->remote&&held->route==current;p.local=!held->remote;if(event.b)*held={};}
            else p.local=p.forward=false; // Unmatched up must not target a new desktop.
        }else {p.forward=candidate;p.local=!candidate||event.kind==InputKind::Absolute;}
        return p;
    }
};

// Upstream ACKs remain ordered even when local cursor positioning and downstream
// injection complete on different threads. B never acknowledges C's input merely
// because it was enqueued. Storage is bounded by the input channel's credit cap.
class RelayLedger {
public:
    struct Entry {InputPacket input;RelayRoute child;uint64_t child_sequence=0;bool local=false,remote=false,ok=true;uint32_t queue_us=0,inject_us=0;};
    bool push(const InputPacket& p,bool local,RelayRoute child={},uint64_t child_sequence=0){
        if(entries_.size()>=512)return false;
        entries_.push_back({p,std::move(child),child_sequence,local,child_sequence!=0,true});return true;
    }
    void local(const InputAck& ack){for(auto& e:entries_)if(e.input.session==ack.session&&e.input.sequence<=ack.sequence&&e.local){e.local=false;e.ok&=ack.injected;e.queue_us=std::max(e.queue_us,ack.host_queue_us);e.inject_us=std::max(e.inject_us,ack.inject_us);}}
    void remote(const std::string& peer,const InputAck& ack){for(auto& e:entries_)if(e.child.peer==peer&&e.child.session==ack.session&&e.child_sequence<=ack.sequence&&e.remote){e.remote=false;e.ok&=ack.injected;e.queue_us=std::max(e.queue_us,ack.host_queue_us);e.inject_us=std::max(e.inject_us,ack.inject_us);}}
    void cancel_remote(){for(auto& e:entries_)e.remote=false;}
    std::vector<InputAck> drain(){
        std::vector<InputAck> result;
        while(!entries_.empty()&&!entries_.front().local&&!entries_.front().remote){
            const auto e=entries_.front();entries_.pop_front();
            // Relay latency includes another network leg, not broker CPU time.
            const InputAck ack{e.input.session,e.input.sequence,e.input.capture_time_us,e.queue_us,e.inject_us,e.ok};
            // Input ACKs are cumulative. Preserve the newest event's clock and
            // timing instead of turning one child completion into a burst of
            // upstream writes/RTT samples. Never hide a failed injection behind
            // a subsequent successful release.
            if(ack.injected&&!result.empty()&&result.back().injected&&result.back().session==ack.session)
                result.back()=ack;
            else result.push_back(ack);
        }return result;
    }
    void reset(){entries_.clear();}
    size_t size()const{return entries_.size();}
private:std::deque<Entry> entries_;
};
}
