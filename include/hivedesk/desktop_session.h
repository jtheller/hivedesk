#pragma once
#include <windows.h>
#include <string>
#include <stdexcept>
#include <system_error>
namespace hive {
inline std::wstring desktop_name(HDESK desktop){
    wchar_t name[256]{};DWORD size=0;
    return desktop&&GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof(name),&size)?name:L"";
}
inline std::wstring input_desktop_name(){
    HDESK desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    auto name=desktop_name(desktop);if(desktop)CloseDesktop(desktop);return name;
}
enum class DesktopUse { Capture, Input };
inline constexpr ACCESS_MASK desktop_access(DesktopUse use){
    return DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_SWITCHDESKTOP|
        (use==DesktopUse::Input?DESKTOP_JOURNALPLAYBACK:0);
}
class InputDesktop {
    HDESK original_=GetThreadDesktop(GetCurrentThreadId()),owned_=nullptr;
    DesktopUse use_;
public:
    explicit InputDesktop(DesktopUse use=DesktopUse::Capture):use_(use){}
    InputDesktop(const InputDesktop&)=delete;
    InputDesktop& operator=(const InputDesktop&)=delete;
    ~InputDesktop(){if(owned_){SetThreadDesktop(original_);CloseDesktop(owned_);}}
    bool attach(){
        // Capture access alone is insufficient for SendInput on Winlogon.
        // Request the playback right explicitly; do not modify desktop ACLs.
        HDESK next=OpenInputDesktop(0,FALSE,desktop_access(use_));
        if(!next)throw std::system_error(GetLastError(),std::system_category(),"Open input desktop");
        const auto name=desktop_name(next);
        if(name.empty()){const auto error=GetLastError();CloseDesktop(next);throw std::system_error(error?error:ERROR_INVALID_DATA,std::system_category(),"Read input desktop identity");}
        const bool changed=name!=desktop_name(GetThreadDesktop(GetCurrentThreadId()));
        // Even on the same named desktop, establish our input handle once:
        // an inherited thread handle may not carry the injection access mask.
        if(!changed&&(owned_||use_==DesktopUse::Capture)){CloseDesktop(next);return false;}
        if(!SetThreadDesktop(next)){const auto error=GetLastError();CloseDesktop(next);throw std::system_error(error,std::system_category(),"Attach input desktop");}
        if(owned_)CloseDesktop(owned_);owned_=next;return changed;
    }
};
}
