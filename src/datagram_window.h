#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace hive::datagram {
using Bytes = std::vector<uint8_t>;
inline void put(Bytes& b, uint64_t n, unsigned bytes) {
    for (unsigned i=bytes; i; --i) b.push_back(uint8_t(n >> ((i-1)*8)));
}
struct Reader {
    const Bytes& b; size_t at=0; bool good=true;
    uint64_t get(unsigned bytes) {
        if (bytes>b.size()-at) { good=false; return 0; }
        uint64_t n=0; while(bytes--) n=(n<<8)|b[at++]; return n;
    }
};

// Socket/crypto independent, single-owner state machine. Times are local monotonic
// microseconds. Only authenticated packets may enter receive(). No clock sync.
class Window {
public:
    static constexpr size_t Fragment=1100, MaxFrame=16*1024*1024+64;
    static constexpr size_t MaxBytes=32*1024*1024, MaxFrames=128, MaxFlight=4096;
    static constexpr uint64_t Deadline=2000000;
    enum : uint8_t { Hello=0, Data=1, Ack=2, Reset=3 };
    struct Picture { Bytes bytes; uint64_t started=0,completed=0; };
    struct Stats {
        uint64_t sent=0,retransmitted=0,received=0,duplicates=0,expired=0,delivered=0;
        uint64_t received_bytes=0,srtt=0,min_rtt=0; size_t flight=0,owned=0;
        double cwnd=32;
    };
    bool ready() const {
        // ACK ownership is not an unsent video FIFO. Allow another frame as
        // soon as all fragments have been scheduled, without a frame-ACK RTT.
        return frames_.size()<MaxFrames && owned_<MaxBytes-MaxFrame &&
            (frames_.empty() || frames_.back()->offset==frames_.back()->bytes.size());
    }
    bool enqueue(Bytes bytes,bool key,uint64_t session,uint64_t now) {
        if(bytes.empty()||bytes.size()>MaxFrame||(!key&&(need_key_||session!=session_))||!ready())return false;
        if(key) { clear_sender(); epoch_=next_frame_; need_key_=false; session_=session; }
        auto f=std::make_shared<Frame>(); f->id=next_frame_++; f->epoch=epoch_;
        f->base=next_packet_; f->at=now; f->bytes=std::move(bytes);
        next_packet_+=(f->bytes.size()+Fragment-1)/Fragment;
        owned_+=f->bytes.size(); frames_.push_back(std::move(f)); return true;
    }
    void cancel(uint64_t session) { if(session_==session) { clear_sender();need_key_=true; } }
    bool take_key_request() { return std::exchange(request_key_,false); }
    void abandon() { clear_sender();need_key_=true;request_key_=true; }
    Stats stats() const { auto s=stats_;s.flight=flight_.size();s.owned=owned_;s.cwnd=cwnd_;s.srtt=srtt_;s.min_rtt=min_rtt_;return s; }
    std::vector<Picture> take_pictures() { return std::exchange(pictures_,{}); }
    uint64_t next_wake(uint64_t now) const {
        uint64_t due=now+1000000;
        if(!acks_.empty())due=std::min(due,ack_due_);
        if(reset_epoch_)due=now;
        if(!frames_.empty()) {
            due=std::min(due,frames_.front()->at+Deadline);
            if(frames_.back()->offset<frames_.back()->bytes.size()&&flight_.size()<size_t(cwnd_))due=std::min(due,pace_due_);
        }
        if(!timers_.empty())due=std::min(due,std::max(pace_due_,timers_.begin()->first));
        if(!assembly_.empty())due=std::min(due,assembly_.begin()->second.at+Deadline);
        return std::max(now,due);
    }
    std::optional<Bytes> poll(uint64_t now) {
        if(!frames_.empty()&&now-frames_.front()->at>=Deadline) {
            ++stats_.expired; abandon(); congestion(now,true);
        }
        if(!assembly_.empty()&&now-assembly_.begin()->second.at>=Deadline) {
            reset_epoch_=receive_epoch_;assembly_.clear();receive_owned_=0;++stats_.expired;
        }
        if(reset_epoch_) {Bytes b{Reset};put(b,std::exchange(reset_epoch_,0),8);return b;}
        if(!acks_.empty()&&(acks_.size()>=32||now>=ack_due_)) {
            const size_t count=std::min<size_t>(128,acks_.size());Bytes b{Ack,uint8_t(count)};
            for(size_t i=0;i<count;++i){put(b,acks_.front(),8);acks_.pop_front();}
            ack_due_=now+1000;return b;
        }
        if(now<pace_due_)return {};
        while(!timers_.empty()&&now>=timers_.begin()->first&&!flight_.at(timers_.begin()->second).frame) {
            const auto id=timers_.begin()->second;timers_.erase(timers_.begin());flight_.erase(id);loss_sample(now);
        }
        if(!timers_.empty()&&now>=timers_.begin()->first) {
            const auto id=timers_.begin()->second;auto& p=flight_.at(id);timers_.erase(p.timer);
            loss_sample(now);p.at=now;p.timer=timers_.emplace(now+rto(),id);
            ++p.tries;++stats_.retransmitted;pace(now);return packet(id,p);
        }
        if(flight_.size()>=size_t(cwnd_))return {};
        for(auto& f:frames_) if(f->offset<f->bytes.size()) {
            const auto offset=f->offset;f->offset+=std::min(Fragment,f->bytes.size()-offset);
            const auto id=f->base+offset/Fragment;Packet p{f,offset,now,1,timers_.emplace(now+rto(),id)};
            flight_.emplace(id,p);++f->pending;++stats_.sent;pace(now);return packet(id,p);
        }
        return {};
    }
    bool receive(const Bytes& b,uint64_t now) {
        if(b.empty())return false;Reader r{b};const auto type=r.get(1);
        if(type==Hello)return b.size()==1;
        if(type==Ack) {
            const auto count=r.get(1);if(!r.good||!count||count>128||b.size()!=2+count*8)return false;
            const bool window_limited=flight_.size()>=size_t(cwnd_*.75);
            for(size_t i=0;i<count;++i) {
                const auto id=r.get(8);auto p=flight_.find(id);if(p==flight_.end())continue;
                if(p->second.tries==1&&now>p->second.at)sample_rtt(now-p->second.at,now);
                auto& bin=loss_bins_[(now/50000)%loss_bins_.size()];
                if(bin.epoch!=now/50000){bin={};bin.epoch=now/50000;}++bin.acked;
                if(window_limited)cwnd_=std::min<double>(MaxFlight,cwnd_+(cwnd_<threshold_?1.:1./cwnd_));
                if(p->second.frame)--p->second.frame->pending;timers_.erase(p->second.timer);flight_.erase(p);
            }
            while(!frames_.empty()&&frames_.front()->offset==frames_.front()->bytes.size()&&!frames_.front()->pending) {
                owned_-=frames_.front()->bytes.size();frames_.pop_front();
            }
            return true;
        }
        if(type==Reset) {
            const auto epoch=r.get(8);if(!r.good||b.size()!=9)return false;
            if(epoch==epoch_&&!need_key_){abandon();congestion(now,true);}return true;
        }
        if(type!=Data)return false;
        const auto id=r.get(8),epoch=r.get(8),frame=r.get(8);const auto total=r.get(4),offset=r.get(4);
        if(!r.good||!id||!epoch||frame<epoch||!total||total>MaxFrame||offset>=total||offset%Fragment||
            b.size()-r.at!=std::min<uint64_t>(Fragment,total-offset)||id<=offset/Fragment)return false;
        // A new reference epoch is carried by every fragment, so reordering the
        // IDR behind its P-frames neither loses the epoch nor exposes P pictures.
        if(epoch<receive_epoch_){ack(id,now);return true;}
        if(epoch>receive_epoch_) {
            if(frame-epoch>=MaxFrames)return false;
            assembly_.clear();receive_owned_=0;receive_epoch_=epoch;next_receive_=epoch;
        }
        if(frame<next_receive_){ack(id,now);++stats_.duplicates;return true;}
        if(frame-next_receive_>=MaxFrames)return false;
        auto it=assembly_.find(frame);
        if(it==assembly_.end()) {
            if(receive_owned_+total>MaxBytes||assembly_.size()>=MaxFrames)return false;
            Assembly a;a.bytes.resize(size_t(total));a.seen.resize(size_t((total+Fragment-1)/Fragment));a.base=id-offset/Fragment;a.at=now;
            receive_owned_+=total;it=assembly_.emplace(frame,std::move(a)).first;
        }
        auto& a=it->second;
        if(a.bytes.size()!=total||a.base!=id-offset/Fragment)return false;
        const auto index=size_t(offset/Fragment);ack(id,now);
        if(a.seen[index]){++stats_.duplicates;return true;}
        a.seen[index]=true;++a.count;std::copy(b.begin()+r.at,b.end(),a.bytes.begin()+size_t(offset));
        ++stats_.received;stats_.received_bytes+=b.size()-r.at;
        for(;;) {
            auto next=assembly_.find(next_receive_);if(next==assembly_.end()||next->second.count!=next->second.seen.size())break;
            auto& done=next->second;receive_owned_-=done.bytes.size();
            pictures_.push_back({std::move(done.bytes),done.at,now});++stats_.delivered;
            assembly_.erase(next);++next_receive_;
        }
        return true;
    }
private:
    struct Frame {Bytes bytes;uint64_t id=0,epoch=0,base=0,at=0;size_t offset=0,pending=0;};
    using Timers=std::multimap<uint64_t,uint64_t>;
    struct Packet {std::shared_ptr<Frame> frame;size_t offset;uint64_t at;unsigned tries;Timers::iterator timer;};
    struct Assembly {Bytes bytes;std::vector<bool> seen;size_t count=0;uint64_t base=0,at=0;};
    std::deque<std::shared_ptr<Frame>> frames_;std::map<uint64_t,Packet> flight_;
    Timers timers_;
    std::map<uint64_t,Assembly> assembly_;std::deque<uint64_t> acks_;std::vector<Picture> pictures_;
    size_t owned_=0,receive_owned_=0;bool need_key_=true,request_key_=false;
    uint64_t next_frame_=1,next_packet_=1,epoch_=0,session_=0,receive_epoch_=0,next_receive_=0,reset_epoch_=0;
    uint64_t pace_due_=0,ack_due_=0,srtt_=0,variance_=0,min_rtt_=0,congested_at_=0;
    double cwnd_=32,threshold_=MaxFlight;Stats stats_;
    struct LossBin {uint64_t epoch=0,acked=0,retries=0;};std::array<LossBin,20> loss_bins_{};
    void clear_sender(){
        // Cancel payload ownership, not congestion credit. Packets already on
        // the wire still occupy the path until ACK or timeout. Otherwise rapid
        // IDR/session changes could repeatedly bypass the congestion window.
        for(auto& [id,p]:flight_)p.frame.reset();frames_.clear();owned_=0;
    }
    uint64_t rto()const{return srtt_?std::clamp<uint64_t>(std::max(srtt_*2,srtt_+4*variance_),30000,1000000):300000;}
    void ack(uint64_t id,uint64_t now) {
        if(acks_.empty())ack_due_=now+1000;
        if(acks_.size()<1024)acks_.push_back(id); // A lost ACK is repaired by RTX.
    }
    void congestion(uint64_t now,bool loss) {
        if(congested_at_&&now-congested_at_<std::max<uint64_t>(srtt_,10000))return;
        congested_at_=now;threshold_=cwnd_=std::max(4.,cwnd_*(loss?.7:.85));
    }
    void loss_sample(uint64_t now) {
        auto& bin=loss_bins_[(now/50000)%loss_bins_.size()];
        if(bin.epoch!=now/50000){bin={};bin.epoch=now/50000;}++bin.retries;
        uint64_t acked=0,retries=0;
        for(const auto& item:loss_bins_)if(now/50000>=item.epoch&&now/50000-item.epoch<10){acked+=item.acked;retries+=item.retries;}
        // Queue growth is the primary congestion signal. An isolated wireless
        // loss must not repeatedly halve a healthy high-RTT path's window.
        // Sustained >=5% repair traffic or a black hole still backs off.
        if(retries>=3&&retries*20>=acked+retries)congestion(now,true);
    }
    void sample_rtt(uint64_t value,uint64_t now) {
        min_rtt_=min_rtt_?std::min(min_rtt_,value):value;
        if(!srtt_){srtt_=value;variance_=value/2;}
        else {variance_=(3*variance_+(srtt_>value?srtt_-value:value-srtt_))/4;srtt_=(7*srtt_+value)/8;}
        if(srtt_>min_rtt_+std::max<uint64_t>(8000,min_rtt_/8))congestion(now,false);
    }
    void pace(uint64_t now) {
        // At most one millisecond of catch-up after scheduling jitter. The
        // congestion window, not application bitrate, bounds outstanding bytes.
        const auto interval=std::max<uint64_t>(9,uint64_t(std::max<uint64_t>(srtt_,1000)/cwnd_));
        pace_due_=std::max(pace_due_+interval,now>1000?now-1000:0);
    }
    static Bytes packet(uint64_t id,const Packet& p) {
        Bytes b{Data};put(b,id,8);put(b,p.frame->epoch,8);put(b,p.frame->id,8);
        put(b,p.frame->bytes.size(),4);put(b,p.offset,4);
        const auto end=std::min(p.frame->bytes.size(),p.offset+Fragment);
        b.insert(b.end(),p.frame->bytes.begin()+p.offset,p.frame->bytes.begin()+end);return b;
    }
};
}
