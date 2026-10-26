#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace hive {
// Fixed storage, cumulative counters, 250ms rolling rates. Call at most once
// per UI/source tick; history storage is spaced by at least 8ms.
// Kept separate from feedback/control sampling so HUD cadence cannot tune CBR.
template<size_t N> class DiagnosticWindow {
    struct Sample { uint64_t at=0;std::array<uint64_t,N> totals{}; };
    std::array<Sample,64> samples_{};
    size_t first_=0,count_=0;
public:
    struct Delta { uint64_t us=0;std::array<uint64_t,N> values{};
        uint32_t rate(size_t index,uint64_t scale,uint32_t cap=UINT32_MAX)const {
            return us?uint32_t(std::min<uint64_t>(cap,values[index]*scale/us)):0;
        }
    };
    Delta sample(uint64_t now,std::array<uint64_t,N> totals) {
        if(count_&&(now<samples_[(first_+count_-1)%64].at||totals[0]<samples_[(first_+count_-1)%64].totals[0]))*this={};
        if(!count_){samples_[0]={now,totals};count_=1;return {};}
        while(count_>1&&now-samples_[(first_+1)%64].at>=250000){first_=(first_+1)%64;--count_;}
        const auto base=samples_[first_];
        const auto last=(first_+count_-1)%64;
        if(now-samples_[last].at>=8000){if(count_==64){first_=(first_+1)%64;--count_;}samples_[(first_+count_)%64]={now,totals};++count_;}
        Delta result;result.us=now-base.at;
        for(size_t i=0;i<N;++i)result.values[i]=totals[i]>=base.totals[i]?totals[i]-base.totals[i]:0;
        return result;
    }
};
enum class DiagnosticTone { Normal,Muted,Warning,Critical };
inline DiagnosticTone diagnostic_threshold(uint64_t value,uint64_t warning,uint64_t critical,bool fresh=true) {
    return !fresh?DiagnosticTone::Muted:value>=critical?DiagnosticTone::Critical:value>=warning?DiagnosticTone::Warning:DiagnosticTone::Normal;
}
inline uint32_t diagnostic_budget(uint32_t target_millihz) {return target_millihz?uint32_t(1000000000ull/target_millihz):16667;}
// A cumulative counter is only abnormal briefly after it increases. Reset and
// first observation establish a baseline, rather than turning old totals red.
class DiagnosticPulse {
    uint64_t previous_=0,until_=0;bool initialized_=false;
public:
    bool update(uint64_t now,uint64_t value){if(initialized_&&value<previous_)until_=0;else if(initialized_&&value>previous_)until_=now+500;previous_=value;initialized_=true;return now<until_;}
};
}
