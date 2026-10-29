#include "hivedesk/transport.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
using namespace hive;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void await(F f,const char* why){const auto end=GetTickCount64()+3000;while(!f()&&GetTickCount64()<end)Sleep(5);require(f(),why);}
int main(){try{
    ViewRoutes local;local.self="C";local.parent="B";local.session=12;
    local.peers["B"]={1,11,{"B","A"},false};local.rebuild();
    require(!local.allowed("A")&&!local.allowed("B")&&local.allowed("D"),"Transitive ancestor filtering");
    const auto rev=local.local.revision;require(!local.rebuild()&&local.local.revision==rev,"Unchanged route churn");
    local.peers["B"]={2,11,{"B","A","C"},false};local.rebuild();
    require(local.local.cycle_owner()=="C","Cycle arbitration owner");
    ViewRoute tail{1,1,{"Z","C","B","A","C"}};
    require(tail.cycle_owner()=="C","Downstream tail incorrectly owns cycle");
    local.parent.clear();local.session=0;local.rebuild();require(local.allowed("A")&&local.local.path.size()==1,"Ancestor restoration");
    std::vector<std::string> long_path;for(unsigned i=0;i<64;++i)long_path.push_back("node"+std::to_string(i));
    local.parent="node0";local.session=1;local.peers["node0"]={1,1,long_path,false};local.rebuild();
    require(local.local.path.size()==64&&local.local.full&&!local.allowed("new"),"Bounded ancestry failed closed");

    Transport a,b,c;std::atomic<bool> resolve=false;std::atomic<unsigned> cuts=0;
    const auto callbacks=[&](Transport& net,const std::string& id){TransportCallbacks cb;
        cb.view_route_changed=[&,id]{auto r=net.view_route();if(resolve&&r.cycle_owner()==id){++cuts;net.set_view_parent({},0);}};return cb;};
    std::string error,peer;
    require(a.start({"A","A","route-test",0,false,true},callbacks(a,"A"),error),error.c_str());
    require(b.start({"B","B","route-test",48231,false,true},callbacks(b,"B"),error),error.c_str());
    require(c.start({"C","C","route-test",48232,false,true},callbacks(c,"C"),error),error.c_str());
    require(a.connect("127.0.0.1",48231,peer,error)&&b.connect("127.0.0.1",48232,peer,error),error.c_str());
    b.set_view_parent("A",101);c.set_view_parent("B",102);
    await([&]{return !c.view_allowed("A")&&!c.view_allowed("B");},"Sparse A-B-C ancestry did not propagate");
    require(!c.request_stream("B",{0,0,60000,103})&&!c.preview("B",102,true),"Hidden ancestor bypassed stream/Peek gate");
    require(a.connect("127.0.0.1",48232,peer,error),error.c_str());
    await([&]{return c.visible_peers().empty();},"Hive list exposed ancestors");
    require(a.visible_peers().size()==2,"Normal downstream devices hidden");
    b.set_view_parent({},0);
    await([&]{return c.view_allowed("A")&&!c.view_allowed("B");},"Removed transitive ancestor stayed hidden");
    b.set_view_parent("A",104);await([&]{return !c.view_allowed("A");},"Recreated edge missing");
    resolve=true;a.set_view_parent("C",105);
    await([&]{return cuts&&a.view_route().cycle_owner().empty()&&b.view_route().cycle_owner().empty()&&c.view_route().cycle_owner().empty();},"Concurrent three-node cycle did not converge");
    require(c.view_route().session==0&&a.view_route().session==105&&b.view_route().session==104,"Arbitration tore down more than chosen edge");
    // Disconnect invalidates the parent route; an old connection cannot keep
    // ancestors hidden after the underlying viewing edge has disappeared.
    a.stop();await([&]{return b.view_allowed("A")&&b.view_route().session==0;},"Disconnect retained ancestry");
    b.stop();c.stop();
    std::puts("PASS view routes: transitive/sparse-chain propagation, restoration, UI filtering, stream/Peek admission, deterministic cycle cut, bounded paths and disconnected-parent cleanup");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL view routes: %s\n",e.what());return 1;}}
