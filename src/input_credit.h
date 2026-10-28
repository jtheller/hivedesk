#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>

namespace hive {
// A measured bandwidth-delay window, independent of address/network topology.
// Preserve the existing eight-packet LAN bound. Only sustained propagation
// delay may expand it; slow host injection must instead reduce pressure.
class InputCredit {
    std::array<uint32_t,32> propagation_{};
    size_t next_=0,count_=0,limit_=8;
public:
    size_t limit()const{return limit_;}
    void observe(uint32_t roundtrip,uint32_t queue,uint32_t inject,bool accepted){
        if(!accepted||queue>8000||inject>12000){*this=InputCredit{};return;}
        const uint64_t host=uint64_t(queue)+inject;
        propagation_[next_]=roundtrip>host?uint32_t(roundtrip-host):0;
        next_=(next_+1)%propagation_.size();count_=std::min(count_+1,propagation_.size());
        if(count_<8)return;
        const auto baseline=*std::min_element(propagation_.begin(),propagation_.begin()+count_);
        // Approximately 250 reports/s plus two credits; hard bound is 64.
        // A lone RTT spike cannot grow the window. Session reset restores eight.
        limit_=std::clamp<size_t>((uint64_t(baseline)+3999)/4000+2,8,64);
    }
};
}
