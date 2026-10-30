#pragma once
#include "hivedesk/types.h"
#include <array>
#include <algorithm>
#include <map>

namespace hive::wake {
using Mac=std::array<uint8_t,6>;
inline bool valid(const Mac& mac){return !(mac[0]&1)&&std::any_of(mac.begin(),mac.end(),[](auto b){return b!=0;});}
inline std::array<uint8_t,102> magic(const Mac& mac){
    std::array<uint8_t,102> packet{};std::fill_n(packet.begin(),6,uint8_t(255));
    for(unsigned i=0;i<16;++i)std::copy(mac.begin(),mac.end(),packet.begin()+6+i*6);return packet;
}
Mac local_mac(const std::string& address);
Mac neighbor_mac(const std::string& address);
// On-link IPv4 only. A successful UDP send is not proof the machine woke.
unsigned send(const std::string& address,const Mac& mac);
struct Record {Peer peer;Mac mac{};uint64_t seen=0;};
class Book {
    std::map<std::string,Record> records_;
public:
    static constexpr uint64_t Lifetime=24*60*60*1000ull;
    void remember(const Peer& peer,const Mac& mac,uint64_t now){
        if(!peer.authenticated||!valid(mac))return;
        std::erase_if(records_,[&](const auto& item){return now-item.second.seen>=Lifetime;});
        if(records_.size()>=128&&!records_.contains(peer.id)){
            auto oldest=std::min_element(records_.begin(),records_.end(),[](const auto& a,const auto& b){return a.second.seen<b.second.seen;});records_.erase(oldest);
        }
        records_[peer.id]={peer,mac,now};
    }
    void forget(const std::string& id){records_.erase(id);}
    void refresh(const Peer& peer,uint64_t now){auto it=records_.find(peer.id);if(it!=records_.end()&&peer.authenticated){it->second.peer=peer;it->second.seen=now;}}
    std::vector<Record> records(uint64_t now)const{
        std::vector<Record> result;for(const auto& [id,r]:records_)if(now-r.seen<Lifetime)result.push_back(r);return result;
    }
};
}
