#include "../src/session_power.h"
#include "../src/wake_lan.h"
#include "hivedesk/reconnect.h"
#include <cstdio>
#include <stdexcept>

void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
struct FakePower {
    static inline unsigned creates=0,closes=0,pulses=0,sets=0,clears=0;
    static inline int denied=-1;static inline bool create_fails=false;
    static HANDLE create(){++creates;return create_fails?INVALID_HANDLE_VALUE:reinterpret_cast<HANDLE>(1);}
    static bool set(HANDLE,POWER_REQUEST_TYPE t){++sets;return int(t)!=denied;}
    static void clear(HANDLE,POWER_REQUEST_TYPE){++clears;}
    static void close(HANDLE){++closes;}
    static void wake_display(){++pulses;}
    static void reset(){creates=closes=pulses=sets=clears=0;denied=-1;create_fails=false;}
};
int main(){try{
    using namespace hive;
    const wake::Mac mac{0x40,0x74,0xe0,0x75,0x70,0x2e};
    require(wake::valid(mac)&&!wake::valid({})&&!wake::valid({1,2,3,4,5,6}),"invalid unicast identity admitted");
    const auto packet=wake::magic(mac);require(packet.size()==102,"magic packet length");
    for(unsigned i=0;i<6;++i)require(packet[i]==255,"magic synchronization");
    for(unsigned i=0;i<16;++i)for(unsigned j=0;j<6;++j)require(packet[6+i*6+j]==mac[j],"magic repetitions");
    wake::Book book;Peer p{"device","Print","192.0.2.1",47980,false};
    book.remember(p,mac,100);require(book.records(100).empty(),"unauthenticated discovery poisoned wake cache");
    p.authenticated=true;book.remember(p,mac,100);require(book.records(101).size()==1,"authenticated wake target missing");
    require(book.records(100+wake::Book::Lifetime).empty(),"expired wake identity retained");
    for(unsigned i=0;i<140;++i){p.id=std::to_string(i);book.remember(p,mac,100+i);}
    require(book.records(240).size()==128,"wake cache unbounded");book.forget("139");require(book.records(240).size()==127,"wake identity revocation");
    ReconnectIntent intent;intent.select("device",7);
    require(!intent.wake_due(8,0),"cancelled intent wakes device");
    for(uint64_t i=0;i<10;++i){require(intent.wake_due(7,i*3000),"wake retry missing");require(!intent.wake_due(7,i*3000+1),"wake storm");}
    require(!intent.wake_due(7,30000)&&!intent.wake_due(7,999999),"wake burst unbounded");
    intent.select("other",9);require(intent.wake_due(9,1000000),"new selection did not reset wake burst");
    intent.connected();require(!intent.wake_due(9,1003000),"connected stream keeps waking");
    intent.lost(1005000);require(intent.wake_due(9,1005000),"recovery missing wake opportunity");
    {SessionPower<FakePower> guard;require(!FakePower::creates,"idle/warm session holds power");
        require(guard.activate()&&guard.activate(),"active power request failed");
        require(FakePower::creates==1&&FakePower::sets==3&&FakePower::pulses==1,"duplicate promotion leaks requests or repeats display wake");}
    require(FakePower::clears==3&&FakePower::closes==1,"disconnect did not release requests");
    FakePower::reset();FakePower::denied=int(PowerRequestExecutionRequired);
    {SessionPower<FakePower> guard;require(!guard.activate(),"partial power failure concealed");guard.release();guard.release();}
    require(FakePower::clears==2&&FakePower::closes==1,"partial failure did not balance acquired requests");
    FakePower::reset();FakePower::create_fails=true;
    {SessionPower<FakePower> guard;require(!guard.activate(),"create failure concealed");}
    require(!FakePower::sets&&!FakePower::clears&&!FakePower::closes&&!FakePower::pulses,"invalid power handle used");
    std::puts("PASS wake: exact magic packet, authenticated/bounded/expired cache, cancellable limited retries, active-only power lifetime, repeated promotion, partial failures. No display/input/sleep changes.");return 0;
}catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}}
