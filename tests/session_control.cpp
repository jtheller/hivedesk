#include "hivedesk/session_control.h"
#include "hivedesk/reconnect.h"
#include <cstdio>
#include <memory>
#include <stdexcept>
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    std::atomic<uint64_t> generation=10;
    hive::ReconnectIntent reconnect;reconnect.select("source",10);
    hive::ViewControl starting("source",100,10);
    require(!starting.cancel("other",100,generation)&&!starting.cancel("source",99,generation),"wrong peer/session cancelled viewer");
    require(starting.cancel("source",100,generation),"host kick did not cancel startup");
    require(!reconnect.current(generation)&&!reconnect.due(generation,999999),"kicked startup automatically reconnected during slow source teardown");
    require(!starting.cancel("source",100,generation),"duplicate termination cancelled again");
    reconnect.select("source",++generation);
    require(!starting.cancel("source",100,generation)&&reconnect.current(generation),"old termination cancelled queued manual selection");
    hive::ViewControl restarted("source",101,generation);
    require(!restarted.cancel("source",100,generation),"old stream cancelled replacement stream");
    auto hosted=std::make_shared<hive::HostControl>("controller",200,44);
    auto captured=hosted;
    const auto outbound_generation=generation.load();
    require(captured->revoke()&&captured->revoked&&!captured->revoke(),"host revoke not immediate/idempotent");
    require(generation==outbound_generation&&reconnect.current(generation),"host kick cancelled reciprocal viewer");
    hosted=std::make_shared<hive::HostControl>("controller",201,44);
    require(hosted!=captured&&!hosted->revoked&&captured->revoked,"queued old cleanup could own a new same-peer stream");
    require(restarted.cancel("source",101,generation)&&!reconnect.current(generation),"replacement stream not independently revocable");
    std::puts("PASS session control: immediate host revocation, independent reciprocal view, startup/retry cancellation, stale/duplicate notices and same-peer replacement.");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
