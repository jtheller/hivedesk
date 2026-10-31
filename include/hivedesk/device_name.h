#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace hive {
// Use the physical DNS host spelling, not the legacy NetBIOS alias. Never
// case-fold display text or replace a user-saved/custom HiveDesk name.
inline std::wstring default_device_name(){
    DWORD size=0;GetComputerNameExW(ComputerNamePhysicalDnsHostname,nullptr,&size);
    if(size&&size<=32768){std::vector<wchar_t> buffer(size);
        if(GetComputerNameExW(ComputerNamePhysicalDnsHostname,buffer.data(),&size)&&size)return {buffer.data(),size};}
    wchar_t fallback[MAX_COMPUTERNAME_LENGTH+1]{};size=MAX_COMPUTERNAME_LENGTH+1;
    if(GetComputerNameW(fallback,&size)&&size)return {fallback,size};
    return L"This computer";
}
}
