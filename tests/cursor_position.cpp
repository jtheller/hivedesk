#include "../src/cursor_position.h"
#include <cstdio>
#include <stdexcept>
using hive::CursorPositionGate;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){try{
    using R=CursorPositionGate::Result;CursorPositionGate gate;gate.reset(41);
    require(hive::cursor_on_primary(0,0,1920,1080)&&hive::cursor_on_primary(1919,1079,1920,1080),"Valid primary pixel excluded");
    require(!hive::cursor_on_primary(-1,500,1920,1080)&&!hive::cursor_on_primary(1920,500,1920,1080)&&
        !hive::cursor_on_primary(500,-1,1920,1080)&&!hive::cursor_on_primary(500,1080,1920,1080),"Off-primary pointer would clamp to a corner");
    hive::CursorState c;c.session=41;c.position_revision=1;c.x=c.y=0;
    require(gate.observe(c,true)==R::Baseline,"Initial corner snapshot moved the pointer");
    c.position_valid=true;c.position_revision=2;c.input_capture_us=100;
    require(gate.observe(c,false)==R::LocalInput,"Late host movement overrode newer local input");
    require(gate.observe(c,true)==R::Revision,"Shape update replayed a rejected position");
    c.position_revision=3;c.input_capture_us=200;
    require(gate.observe(c,true)==R::Allowed,"Causally current host movement was lost");
    c.position_revision=2;require(gate.observe(c,true)==R::Revision,"Out-of-order position was accepted");
    gate.reset(42);c.position_revision=999;
    require(gate.observe(c,true)==R::Session,"Previous same-peer session reclaimed the pointer");
    c.session=42;c.position_revision=1;c.position_valid=false;
    require(gate.observe(c,true)==R::Baseline,"Reconnect snapshot reclaimed the pointer");
    c.session=0;c.position_valid=true;
    require(gate.observe(c,true)==R::Session,"Legacy cursor moved the pointer without causal identity");
    c.session=42;c.position_revision=2;
    require(gate.observe(c,true)==R::Allowed,"Old session poisoned the new revision counter");
    gate.reset(0);require(gate.observe(c,true)==R::Session,"Inactive viewer accepted a position");
    std::puts("PASS cursor position: baseline, delayed/local-input race, no shape replay, monotonic revisions, reconnect isolation and legacy safety; no desktop input");return 0;
}catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}}
