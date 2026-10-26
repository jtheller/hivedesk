#pragma once
#include "hivedesk/types.h"
#include <windows.h>
#include <array>
#include <algorithm>

namespace hive {
// Production supplies SendInput. Tests record INPUT values without touching
// the desktop; the state transitions are identical in both paths.
class InjectedInputState {
    std::array<bool,512> keys_{};
    std::array<bool,6> buttons_{};
    static DWORD button_flag(int button,bool up){
        switch(button){
        case 1:return up?MOUSEEVENTF_LEFTUP:MOUSEEVENTF_LEFTDOWN;
        case 2:return up?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_RIGHTDOWN;
        case 3:return up?MOUSEEVENTF_MIDDLEUP:MOUSEEVENTF_MIDDLEDOWN;
        case 4:case 5:return up?MOUSEEVENTF_XUP:MOUSEEVENTF_XDOWN;
        default:return 0;
        }
    }
public:
    static constexpr ULONG_PTR Marker=0x48495645;
    bool empty()const{
        return std::none_of(keys_.begin(),keys_.end(),[](bool held){return held;})&&
            std::none_of(buttons_.begin(),buttons_.end(),[](bool held){return held;});
    }
    template<class Send> bool release(Send&& send){
        bool complete=true;
        for(size_t key=0;key<keys_.size();++key){
            if(!keys_[key])continue;
            INPUT input{};input.type=INPUT_KEYBOARD;input.ki.wScan=WORD(key&255);
            input.ki.dwFlags=KEYEVENTF_SCANCODE|KEYEVENTF_KEYUP|(key>=256?KEYEVENTF_EXTENDEDKEY:0);
            input.ki.dwExtraInfo=Marker;
            if(send(input))keys_[key]=false;else complete=false;
        }
        for(int button=1;button<=5;++button){
            if(!buttons_[button])continue;
            INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=button_flag(button,true);
            input.mi.mouseData=button==4?XBUTTON1:button==5?XBUTTON2:0;input.mi.dwExtraInfo=Marker;
            if(send(input))buttons_[button]=false;else complete=false;
        }
        return complete;
    }
    template<class Send> bool apply(const InputEvent& event,Send&& send){
        if(event.kind==InputKind::ReleaseAll)return release(send);
        INPUT input{};
        switch(event.kind){
        case InputKind::Key:{
            if(event.a<=0||event.a>255||(event.b!=0&&event.b!=1)||(event.c!=0&&event.c!=1))return false;
            const auto key=size_t(event.a)+(event.c?256:0);
            if(event.b&&!keys_[key])return true;
            input.type=INPUT_KEYBOARD;input.ki.wScan=WORD(event.a);
            input.ki.dwFlags=KEYEVENTF_SCANCODE|(event.b?KEYEVENTF_KEYUP:0)|(event.c?KEYEVENTF_EXTENDEDKEY:0);
            input.ki.dwExtraInfo=Marker;
            if(!send(input))return false;
            keys_[key]=!event.b;return true;
        }
        case InputKind::Absolute:
            if(event.a<0||event.a>65535||event.b<0||event.b>65535)return false;
            input.type=INPUT_MOUSE;input.mi.dx=event.a;input.mi.dy=event.b;input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE;break;
        case InputKind::Relative:
            input.type=INPUT_MOUSE;input.mi.dx=event.a;input.mi.dy=event.b;input.mi.dwFlags=MOUSEEVENTF_MOVE;break;
        case InputKind::Button:
            if(event.a<1||event.a>5||(event.b!=0&&event.b!=1))return false;
            if(event.b&&!buttons_[event.a])return true;
            input.type=INPUT_MOUSE;input.mi.dwFlags=button_flag(event.a,event.b!=0);
            input.mi.mouseData=event.a==4?XBUTTON1:event.a==5?XBUTTON2:0;break;
        case InputKind::Wheel:
            if(event.b!=0&&event.b!=1)return false;
            input.type=INPUT_MOUSE;input.mi.dwFlags=event.b?MOUSEEVENTF_HWHEEL:MOUSEEVENTF_WHEEL;
            input.mi.mouseData=DWORD(event.a);break;
        default:return false;
        }
        input.mi.dwExtraInfo=Marker;
        if(!send(input))return false;
        if(event.kind==InputKind::Button)buttons_[event.a]=!event.b;
        return true;
    }
};
}
