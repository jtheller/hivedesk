#include "hivedesk/fixed_hotkeys.h"
#include "hivedesk/types.h"
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
using hive::FixedHotkey;
using hive::FixedHotkeys;
void modifiers(FixedHotkeys& keys,bool right,bool shift,bool injected=false) {
    keys.observe(right?0xa3:0xa2,0x1d,right,false,injected);
    keys.observe(right?0xa5:0xa4,0x38,right,false,injected);
    if(shift)keys.observe(right?0xa1:0xa0,right?0x36:0x2a,false,false,injected);
}
}
int main(){try {
    for(const bool right:{false,true}) {
        FixedHotkeys keys;modifiers(keys,right,true);
        const auto kick=keys.observe(0xc0,0x29,false,false,false);
        require(kick.consume&&kick.action==FixedHotkey::KickController,"physical modifier kick not recognized");
        const auto repeat=keys.observe(0xc0,0x29,false,false,false);
        require(repeat.consume&&repeat.action==FixedHotkey::None,"held kick repeated its action");
        keys.observe(right?0xa1:0xa0,right?0x36:0x2a,false,true,false);
        const auto after_shift=keys.observe(0xc0,0x29,false,false,false);
        require(after_shift.consume&&after_shift.action==FixedHotkey::None,"releasing Shift changed a held kick into escape");
        require(!keys.observe('A',0x1e,false,false,false).consume,"kick swallowed an unrelated key");
        const auto up=keys.observe(0xc0,0x29,false,true,false);
        require(up.consume&&up.action==FixedHotkey::None,"kick key-up escaped to the peer");
        const auto escape=keys.observe(0xc0,0x29,false,false,false);
        require(escape.consume&&escape.action==FixedHotkey::ReturnOne,"Ctrl Alt backtick did not remain a separate escape");
        require(keys.observe(0xc0,0x29,false,true,false).consume,"escape key-up not consumed");
    }
    {
        FixedHotkeys keys;modifiers(keys,false,false);
        require(keys.observe(8,0x0e,false,false,false).action==FixedHotkey::ReturnLocal,"emergency local escape missing");
        require(keys.observe(8,0x0e,false,false,false).action==FixedHotkey::None,"emergency escape repeated");
        require(keys.observe(8,0x0e,false,true,false).consume,"emergency key-up leaked");
        require(!keys.observe(8,0x0e,false,false,true).consume,"injected emergency escape accepted");
        keys.observe(0xa0,0x2a,false,false,false);
        require(!keys.observe(8,0x0e,false,false,false).consume,"extra shift accepted for emergency escape");
    }
    {
        FixedHotkeys keys;modifiers(keys,false,true,true);
        require(keys.modifiers()==0,"injected modifiers polluted physical key state");
        require(!keys.observe(0xc0,0x29,false,false,false).consume,"remote modifiers triggered a local kick");
        modifiers(keys,false,true);
        require(!keys.observe(0xc0,0x29,false,false,true).consume,"injected backtick triggered kick");
        require(!keys.observe(0xc0,0x2b,false,false,false).consume,"virtual backtick without physical scan triggered kick");
        require(!keys.observe(0xc0,0x29,true,false,false).consume,"extended scan alias triggered kick");
        keys.observe(0x5b,0x5b,true,false,false);
        require(!keys.observe(0xc0,0x29,false,false,false).consume,"extra Windows modifier was accepted");
        keys.observe(0x5b,0x5b,true,true,false);
        const auto physical=keys.observe(0xdf,0x29,false,false,false);
        require(physical.consume&&physical.action==FixedHotkey::KickController,"shortcut depended on keyboard-layout virtual key");
        require(!keys.observe(0xdf,0x29,false,true,true).consume,"injected key-up was consumed");
        require(keys.observe(0xdf,0x29,false,false,false).action==FixedHotkey::None,"injected key-up reset physical autorepeat latch");
        require(keys.observe(0xdf,0x29,false,true,false).consume,"physical key-up lost its latch");
    }
    {
        FixedHotkeys keys;
        keys.forwarded(0x1d,false,false);keys.forwarded(0x38,true,false);keys.forwarded(0x2a,false,false);
        keys.forwarded(0x36,false,false);keys.forwarded(0x36,false,true);
        keys.forwarded(0x1e,false,false); // An unrelated held A must stay held.
        std::vector<std::pair<uint32_t,bool>> released;
        keys.release_forwarded_modifiers([&](uint32_t scan,bool ext){released.emplace_back(scan,ext);keys.forwarded(scan,ext,true);});
        require(released==std::vector<std::pair<uint32_t,bool>>{{0x1d,false},{0x38,true},{0x2a,false}},"kick released the wrong forwarded modifier state");
        keys.release_forwarded_modifiers([&](uint32_t scan,bool ext){released.emplace_back(scan,ext);});
        require(released.size()==3,"modifier release repeated");
        keys.forwarded(0x1d,false,false);keys.clear_forwarded();
        keys.release_forwarded_modifiers([&](uint32_t scan,bool ext){released.emplace_back(scan,ext);});
        require(released.size()==3,"old session modifiers leaked into a new session");
    }
    require(!hive::valid_hive_hotkey(3,0xc0)&&!hive::valid_hive_hotkey(7,0xc0),"configurable summon stole a fixed shortcut");
    std::puts("PASS fixed physical shortcuts: left/right modifiers, kick vs escape, repeat and key-up, injected input isolation, layout independence, modifier release, reserved chords.");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
