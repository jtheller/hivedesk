#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace hive {
enum class FixedHotkey { None, ReturnLocal, KickController, ReturnOne };
struct FixedHotkeyResult { bool consume=false; FixedHotkey action=FixedHotkey::None; };

// Physical key state only. Windows virtual-key values identify modifiers;
// Scans 0x29/0x0e identify backtick/Backspace independently of keyboard layout.
class FixedHotkeys {
public:
    FixedHotkeyResult observe(uint32_t key,uint32_t scan,bool extended,bool up,bool injected) {
        if(injected)return {};
        if(key<down_.size())down_[key]=!up;
        if((scan!=0x29&&scan!=0x0e)||extended)return {};
        auto& consumed=scan==0x29?backtick_consumed_:backspace_consumed_;
        if(consumed) {
            if(up)consumed=false;
            return {true,FixedHotkey::None};
        }
        if(up)return {};
        const auto mods=modifiers();
        const auto action=scan==0x0e?(mods==3?FixedHotkey::ReturnLocal:FixedHotkey::None):
            mods==7?FixedHotkey::KickController:mods==3?FixedHotkey::ReturnOne:FixedHotkey::None;
        if(action==FixedHotkey::None)return {};
        consumed=true;
        return {true,action};
    }
    uint32_t modifiers() const {
        // MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=4, MOD_WIN=8.
        return ((down_[0x11]||down_[0xa2]||down_[0xa3])?2u:0u)|
            ((down_[0x12]||down_[0xa4]||down_[0xa5])?1u:0u)|
            ((down_[0x10]||down_[0xa0]||down_[0xa1])?4u:0u)|
            ((down_[0x5b]||down_[0x5c])?8u:0u);
    }
    void forwarded(uint32_t scan,bool extended,bool up) {
        for(std::size_t i=0;i<ModifierKeys.size();++i)
            if(ModifierKeys[i].scan==scan&&ModifierKeys[i].extended==extended)forwarded_[i]=!up;
    }
    template<class Release> void release_forwarded_modifiers(Release release) {
        // Leave unrelated held keys and pointer buttons alone. The later
        // physical modifier key-up is harmless after this explicit release.
        for(std::size_t i=0;i<ModifierKeys.size();++i)if(forwarded_[i]) {
            forwarded_[i]=false;release(ModifierKeys[i].scan,ModifierKeys[i].extended);
        }
    }
    void clear_forwarded() {forwarded_.fill(false);}
private:
    struct ModifierKey {uint32_t scan;bool extended;};
    static constexpr std::array<ModifierKey,6> ModifierKeys{{{0x1d,false},{0x1d,true},{0x38,false},{0x38,true},{0x2a,false},{0x36,false}}};
    std::array<bool,256> down_{};
    std::array<bool,ModifierKeys.size()> forwarded_{};
    bool backtick_consumed_=false,backspace_consumed_=false;
};
}
