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
    std::puts("PASS nested return: A-B-C single-level unwind, repeated presses, stale sessions/generations, revoked controller, timeout, cancellation, cycle/limit responses");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
