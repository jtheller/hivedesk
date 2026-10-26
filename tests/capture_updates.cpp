#include "../src/capture_updates.h"
#include <cstdio>
#include <stdexcept>

int main(){try{
    auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    hive::CaptureUpdateFilter filter;
    require(filter.needs_copy(0)&&!filter.have_picture(),"Pointer-only first acquisition left the initial desktop uninitialized");
    // Only a successful copy commits initialization; a failed first copy cannot
    // make the next pointer acquisition reuse an image that was never acquired.
    require(filter.needs_copy(0),"An uncommitted initial copy advanced capture state");
    filter.copied();
    require(filter.have_picture()&&!filter.needs_copy(0),"Pointer-only update resubmitted an unchanged desktop");

    // 1 kHz pointer metadata interspersed with 100 Hz desktop presents: exactly
    // the desktop updates consume video work. A last-image recovery IDR remains
    // possible during all intervening pointer-only acquisitions.
    uint32_t copies=0,cached_recoveries=0;
    for(int64_t tick=1;tick<=1000;++tick){
        const int64_t last_present=tick%10==0?tick:0;
        if(filter.needs_copy(last_present)){++copies;filter.copied();}
        if(tick%125==0){require(filter.have_picture(),"Pointer activity invalidated the cached recovery picture");++cached_recoveries;}
    }
    require(copies==100&&cached_recoveries==8,"Pointer traffic consumed desktop pacing slots");
    // LastPresentTime is decisive, not dirty-rectangle count or mouse metadata.
    // A software cursor drawn into the bitmap therefore still qualifies.
    require(filter.needs_copy(1001),"A real desktop/software-cursor update was suppressed");
    filter.copied();
    require(!filter.needs_copy(0)&&filter.have_picture(),"A subsequent pointer update changed cached-image eligibility");
    std::puts("PASS capture updates: initialized first image, pointer-only suppression, desktop/software-cursor changes and cached recovery image.");return 0;
}catch(const std::exception& e){std::printf("FAIL capture updates: %s\n",e.what());return 1;}}
