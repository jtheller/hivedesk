#include "hivedesk/reconnect.h"
#include <stdexcept>
#include <cstdio>
void require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
int main(){try{
    hive::StreamStartupDeadline startup(100,0);
    require(!startup.expired(8099)&&startup.expired(8100),"Silent startup did not time out");
    for(uint64_t now=1000;now<16100;now+=1000)startup.progress(now,now);
    require(!startup.expired(16099)&&startup.expired(16100),"Continuous bytes renewed Connecting past its hard deadline");
    startup.progress(20000,20000);require(startup.expired(20000),"Expired startup resurrected by late bytes");
    hive::ReconnectIntent r;r.select("VI",10);
    require(r.due(10,0),"first attempt delayed");r.attempted(0);
    require(!r.due(10,999)&&r.due(10,1000),"retry interval");
    r.attempted(1000);require(!r.due(10,2999)&&r.due(10,3000),"backoff");
    require(!r.due(11,100000),"Escape did not cancel a pending retry");
    r.connected();require(!r.due(10,100000),"connected session retried");
    r.lost(110000);require(r.due(10,110000),"lost session did not resume");
    r.select("DEVG",12);require(!r.current(10)&&r.peer=="DEVG"&&r.due(12,0),"new selection retained old intent");
    for(unsigned i=0;i<20;++i){r.attempted(0);require(r.next_ms<=8000,"unbounded backoff");}
    r.connected();r.lost(1);r.attempted(1);require(r.next_ms==1001,"successful reconnect did not reset backoff");
    std::puts("PASS session retry: backoff, loss recovery, Escape generation cancellation, replacement selection, successful reset.");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
