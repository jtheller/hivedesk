#pragma once
#include <windows.h>
#include <string>
#include <stdexcept>
namespace hive {
inline std::wstring desktop_name(HDESK desktop){
    wchar_t name[256]{};DWORD size=0;
    return desktop&&GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof(name),&size)?name:L"";
}
inline std::wstring input_desktop_name(){
    HDESK desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    auto name=desktop_name(desktop);if(desktop)CloseDesktop(desktop);return name;
}
class InputDesktop {
    HDESK original_=GetThreadDesktop(GetCurrentThreadId()),owned_=nullptr;
public:
    ~InputDesktop(){if(owned_){SetThreadDesktop(original_);CloseDesktop(owned_);}}
    bool attach(){
        HDESK next=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_SWITCHDESKTOP);
        if(!next)throw std::runtime_error("Open input desktop: "+std::to_string(GetLastError()));
        if(desktop_name(next)==desktop_name(GetThreadDesktop(GetCurrentThreadId()))){CloseDesktop(next);return false;}
        if(!SetThreadDesktop(next)){const auto error=GetLastError();CloseDesktop(next);throw std::runtime_error("Attach input desktop: "+std::to_string(error));}
        if(owned_)CloseDesktop(owned_);owned_=next;return true;
    }
};
}
