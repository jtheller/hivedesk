#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <algorithm>

namespace hive::bootstrap {
constexpr wchar_t Registry[]=L"Software\\HiveDesk\\Bootstrap";
inline std::wstring stop_event(DWORD session){return L"Global\\HiveDesk.Prelogon.Stop.v1."+std::to_wstring(session);}
struct Identity {std::wstring id,name;bool enabled=true;};
inline bool valid_id(const std::wstring& id){return id.size()==32&&std::all_of(id.begin(),id.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');});}
inline std::wstring read_string(HKEY key,const wchar_t* value){
    wchar_t text[257]{};DWORD size=sizeof(text);
    if(RegGetValueW(key,nullptr,value,RRF_RT_REG_SZ,nullptr,text,&size)!=ERROR_SUCCESS)return {};
    return text;
}
inline Identity load(){
    Identity value;HKEY key{};
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,Registry,0,KEY_READ,&key)!=ERROR_SUCCESS)return value;
    value.id=read_string(key,L"DeviceId");value.name=read_string(key,L"Name");DWORD enabled=1,size=sizeof(enabled);
    RegGetValueW(key,nullptr,L"Enabled",RRF_RT_REG_DWORD,nullptr,&enabled,&size);value.enabled=enabled!=0;RegCloseKey(key);
    if(!valid_id(value.id))value.id.clear();return value;
}
// Only the SYSTEM service writes this protected cache. It stores public device
// identity/startup preference, never credentials or a user-controlled command.
inline bool save(const Identity& value){
    if(!valid_id(value.id)||value.name.empty()||value.name.size()>256)return false;
    const auto old=load();if(old.id==value.id&&old.name==value.name&&old.enabled==value.enabled)return true;
    HKEY key{};if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,Registry,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    const DWORD enabled=value.enabled?1:0;
    bool ok=RegSetValueExW(key,L"DeviceId",0,REG_SZ,reinterpret_cast<const BYTE*>(value.id.c_str()),DWORD((value.id.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
    ok=(RegSetValueExW(key,L"Name",0,REG_SZ,reinterpret_cast<const BYTE*>(value.name.c_str()),DWORD((value.name.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS)&&ok;
    ok=(RegSetValueExW(key,L"Enabled",0,REG_DWORD,reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled))==ERROR_SUCCESS)&&ok;
    RegCloseKey(key);return ok;
}
inline Identity initial(){
    auto value=load();
    if(value.id.empty()){
        BYTE bytes[16]{};if(BCryptGenRandom(nullptr,bytes,sizeof(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return {};
        constexpr wchar_t hex[]=L"0123456789abcdef";for(auto b:bytes){value.id+=hex[b>>4];value.id+=hex[b&15];}
    }
    if(value.name.empty()){wchar_t name[MAX_COMPUTERNAME_LENGTH+1]{};DWORD size=MAX_COMPUTERNAME_LENGTH+1;GetComputerNameW(name,&size);value.name=name;}
    return value;
}
// A lock keeps the user's SID, so it never replaces a working broker. A first
// login/logoff changes its authority even when the console session ID is reused.
inline bool replace_host(DWORD current_session,const std::wstring& current_sid,DWORD session,const std::wstring& sid){return current_session!=session||current_sid!=sid;}
inline bool prelogon_idle(bool has_lease,bool warm,bool preview){return !has_lease||(warm&&!preview);}
}
