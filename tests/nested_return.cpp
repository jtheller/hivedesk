#include "hivedesk/nested_return.h"
#include <cstdio>
#include <stdexcept>
using namespace hive;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try{
    NestedReturn a,b;
    NestedReturn::View ab{"b",11,1},bc{"c",22,2};
    auto ar=a.begin(ab,{},16,100);
    auto br=b.begin(bc,{"a",ar},15,101);
    // C is the leaf. B closes C, reports success; A must keep B connected.
    auto bottom=b.reply("c",{22,br.id,ReturnResult::Leaf},bc,true);
    require(bottom&&bottom->close&&bottom->result==ReturnResult::Returned,"deepest viewer did not close");
    auto top=a.reply("b",{11,ar.id,bottom->result},ab,true);
    require(top&&!top->close&&top->result==ReturnResult::Returned&&!a.busy()&&!b.busy(),"unwind closed more than one level");
    ar=a.begin(ab,{},16,110);
    top=a.reply("b",{11,ar.id,ReturnResult::Leaf},ab,true);
    require(top&&top->close,"second press did not return to A");
    ar=a.begin(ab,{},16,120);
    require(!a.reply("wrong",{11,ar.id,ReturnResult::Leaf},ab,true),"wrong peer accepted");
    require(!a.reply("b",{12,ar.id,ReturnResult::Leaf},ab,true),"wrong session accepted");
    require(!a.reply("b",{11,ar.id-1,ReturnResult::Leaf},ab,true),"old operation replayed");
    auto switched=ab;switched.generation++;
    top=a.reply("b",{11,ar.id,ReturnResult::Leaf},switched,true);
    require(top&&!top->close&&top->result==ReturnResult::Stale,"old reply closed new selection");
    ar=a.begin(ab,{},16,130);
    top=a.reply("b",{11,ar.id,ReturnResult::Leaf},ab,false);
    require(top&&!top->close,"revoked controller still unwound a view");
    ar=a.begin(ab,{},16,140);
    require(!a.tick(10139,ab,true),"early timeout");
    top=a.tick(10140,ab,true);
    require(top&&!top->close&&top->result==ReturnResult::Timeout,"timeout guessed a parent to close");
    require(!a.reply("b",{11,ar.id,ReturnResult::Leaf},ab,true),"late reply acted after timeout");
    for(auto result:{ReturnResult::Busy,ReturnResult::Stale,ReturnResult::Limit,ReturnResult::Timeout}){
        ar=a.begin(ab,{},16,150);top=a.reply("b",{11,ar.id,result},ab,true);
        require(top&&!top->close&&top->result==result,"error caused extra unwind");
    }
    ar=a.begin(ab,{},16,160);require(a.cancel().has_value()&&!a.busy(),"emergency cancel stuck");
    require(!a.reply("b",{11,ar.id,ReturnResult::Leaf},ab,true),"cancelled operation revived");
    ar=a.begin(ab,{},16,200,true);br=b.begin(bc,{"a",ar},15,201,true);
    require(!a.reply("b",{11,ar.id,ReturnResult::Returned},ab,true),"return reply completed remote summon");
    bottom=b.reply("c",{22,br.id,ReturnResult::Returned,true},bc,true);
    require(bottom&&bottom->summon&&!bottom->close,"remote summon closed intermediate viewer");
    top=a.reply("b",{11,ar.id,bottom->result,true},ab,true);
    require(top&&top->summon&&!top->close&&!a.busy(),"remote summon did not preserve outer viewer");
    for(auto result:{ReturnResult::Leaf,ReturnResult::Busy,ReturnResult::Stale,ReturnResult::Limit,ReturnResult::Timeout}){
        ar=a.begin(ab,{},16,210,true);top=a.reply("b",{11,ar.id,result,true},ab,true);
        require(top&&top->summon&&!top->close,"summon result closed a viewer");
    }
    ar=a.begin(ab,{},16,220,true);top=a.tick(10220,ab,true);
    require(top&&top->summon&&top->result==ReturnResult::Timeout&&!top->close,"summon timeout lost operation type");
    ar=a.begin(ab,{},16,230,true);top=a.reply("b",{11,ar.id,ReturnResult::Returned,true},switched,true);
    require(top&&top->result==ReturnResult::Stale&&!top->close,"summon crossed view generation");
    NestedReturn c;NestedReturn::View cd{"d",33,3};
    ar=a.begin(ab,{},16,300,false,true);br=b.begin(bc,{"a",ar},15,301,false,true);
    auto cr=c.begin(cd,{"b",br},14,302,false,true);
    require(!c.reply("d",{33,cr.id,ReturnResult::Leaf},cd,true),"ordinary reply completed stats");
    auto deepest=c.reply("d",{33,cr.id,ReturnResult::Leaf,false,true},cd,true);
    require(deepest&&deepest->toggle_stats&&!deepest->close&&deepest->result==ReturnResult::Returned,"deepest stats did not toggle C-D only");
    bottom=b.reply("c",{22,br.id,deepest->result,false,true},bc,true);
    top=a.reply("b",{11,ar.id,bottom->result,false,true},ab,true);
    require(bottom&&!bottom->toggle_stats&&!bottom->close&&top&&!top->toggle_stats&&!top->close,"stats toggled more than deepest viewer");
    for(auto result:{ReturnResult::Busy,ReturnResult::Stale,ReturnResult::Limit,ReturnResult::Timeout}){
        ar=a.begin(ab,{},16,310,false,true);top=a.reply("b",{11,ar.id,result,false,true},ab,true);
        require(top&&!top->toggle_stats&&!top->close,"stats error toggled or closed a view");
    }
    ar=a.begin(ab,{},16,320,false,true);top=a.reply("b",{11,ar.id,ReturnResult::Leaf,false,true},switched,true);
    require(top&&!top->toggle_stats,"stats crossed generation");
    ar=a.begin(ab,{},16,330,false,true);top=a.reply("b",{11,ar.id,ReturnResult::Leaf,false,true},ab,false);
    require(top&&!top->toggle_stats,"stats accepted revoked authorization");
    ar=a.begin(ab,{},16,340,false,true);top=a.tick(10340,ab,true);
    require(top&&top->stats&&!top->toggle_stats&&!a.reply("b",{11,ar.id,ReturnResult::Leaf,false,true},ab,true),"stats late response survived timeout");
    ar=a.begin(ab,{},16,350,false,true);a.cancel();
    require(!a.reply("b",{11,ar.id,ReturnResult::Leaf,false,true},ab,true),"cancelled stats revived");
    std::puts("PASS compact stats: deepest A-B-C-D only, mode isolation, revocation, timeout, cancellation");
    std::puts("PASS nested return and remote summon: A-B-C, stale sessions/generations, separate reply types, timeout, cancellation and bounded errors");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
