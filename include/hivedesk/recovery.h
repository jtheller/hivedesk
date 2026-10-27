#pragma once
#include "gpu_policy_restart.h"
#include <tlhelp32.h>

namespace hive::recovery {
constexpr DWORD GraceMs=1500,KillWaitMs=5000,ExitCode=0x48445253;
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE h=nullptr):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
// Holds a kernel handle throughout: no name-based killing or PID reuse race.
inline bool stop_process(HANDLE process,DWORD grace=GraceMs,DWORD kill_wait=KillWaitMs,bool* forced=nullptr){
    if(forced)*forced=false;
    auto wait=WaitForSingleObject(process,grace);
    if(wait==WAIT_OBJECT_0)return true;
    if(wait!=WAIT_TIMEOUT)return false;
    if(!TerminateProcess(process,ExitCode))return WaitForSingleObject(process,0)==WAIT_OBJECT_0;
    if(forced)*forced=true;
    return WaitForSingleObject(process,kill_wait)==WAIT_OBJECT_0;
}
inline void post_close(DWORD pid){
    EnumWindows([](HWND window,LPARAM value)->BOOL{
        DWORD owner=0;GetWindowThreadProcessId(window,&owner);
        if(owner==DWORD(value))PostMessageW(window,WM_CLOSE,0,0);
        return TRUE;
    },LPARAM(pid));
}
inline bool same_image(HANDLE process,const std::filesystem::path& expected,DWORD session){
    DWORD actual_session=0,length=32768;wchar_t path[32768]{};
    return ProcessIdToSessionId(GetProcessId(process),&actual_session)&&actual_session==session&&
        QueryFullProcessImageNameW(process,0,path,&length)&&
        CompareStringOrdinal(path,-1,expected.c_str(),-1,TRUE)==CSTR_EQUAL;
}
// Settings is separate from the tray. Only children of this pinned predecessor,
// created after it and running its adjacent Settings image, are eligible.
inline std::vector<HANDLE> settings_children(HANDLE previous){
    std::vector<HANDLE> result;DWORD session=0,length=32768;wchar_t path[32768]{};
    FILETIME created{},exit{},kernel{},user{};
    if(!ProcessIdToSessionId(GetProcessId(previous),&session)||!QueryFullProcessImageNameW(previous,0,path,&length)||
       !GetProcessTimes(previous,&created,&exit,&kernel,&user))return result;
    const auto expected=std::filesystem::path(path).parent_path()/L"settings"/L"HiveDesk.Settings.exe";
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W entry{sizeof(entry)};
    if(snapshot.value==INVALID_HANDLE_VALUE||!Process32FirstW(snapshot.value,&entry))return result;
    do{if(entry.th32ParentProcessID!=GetProcessId(previous))continue;
        HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,entry.th32ProcessID);
        if(!process)continue;
        FILETIME child_created{};
        if(same_image(process,expected,session)&&GetProcessTimes(process,&child_created,&exit,&kernel,&user)&&CompareFileTime(&created,&child_created)<=0)
            result.push_back(process);
        else CloseHandle(process);
    }while(Process32NextW(snapshot.value,&entry));
    return result;
}
inline bool previous(HANDLE process,bool& forced){
    Handle owned(process);
    if(!gpu_policy_restart::valid_previous(process))return false;
    const auto settings=settings_children(process);
    post_close(GetProcessId(process));
    for(auto child:settings)post_close(GetProcessId(child));
    const bool stopped=stop_process(process,GraceMs,KillWaitMs,&forced);
    bool settings_stopped=true;
    for(auto child:settings){settings_stopped=stop_process(child,0)&&settings_stopped;CloseHandle(child);}
    return stopped&&settings_stopped;
}
inline std::wstring event_name(DWORD session){return L"Global\\HiveDesk.Recovery.v1."+std::to_wstring(session);}
inline HANDLE create_event(DWORD session,const std::wstring& sid){
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    const auto security=gpu_policy_restart::event_security(sid);
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(security.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    HANDLE event=CreateEventW(&attributes,FALSE,FALSE,event_name(session).c_str());LocalFree(descriptor);
    if(event&&!update::system_owned(event)){CloseHandle(event);return nullptr;}
    return event;
}
enum class BrokerResult {Restarted,Requested,Unavailable,Failed};
inline BrokerResult restart_broker(){
    DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session))return BrokerResult::Failed;
    Handle event(OpenEventW(EVENT_MODIFY_STATE|READ_CONTROL,FALSE,event_name(session).c_str()));
    if(!event.value||!update::system_owned(event.value))return BrokerResult::Unavailable;
    // Observe only the broker from this installation and this interactive
    // session. Termination stays inside the SYSTEM service that owns its handle.
    wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))return BrokerResult::Failed;
    const auto expected=std::filesystem::path(path).parent_path()/L"HiveDeskHost.exe";
    std::vector<HANDLE> old;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W entry{sizeof(entry)};
    if(snapshot.value!=INVALID_HANDLE_VALUE&&Process32FirstW(snapshot.value,&entry))do{
        if(_wcsicmp(entry.szExeFile,L"HiveDeskHost.exe"))continue;
        HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|READ_CONTROL,FALSE,entry.th32ProcessID);
        if(!process)continue;
        if(same_image(process,expected,session)&&update::system_owned(process))old.push_back(process);else CloseHandle(process);
    }while(Process32NextW(snapshot.value,&entry));
    const bool signalled=gpu_policy_restart::signal_broker(event.value);
    bool stopped=signalled;const auto deadline=GetTickCount64()+KillWaitMs+1000;
    for(auto process:old){const auto now=GetTickCount64();stopped=(WaitForSingleObject(process,now<deadline?DWORD(deadline-now):0)==WAIT_OBJECT_0)&&stopped;CloseHandle(process);}
    return !stopped?BrokerResult::Failed:old.empty()?BrokerResult::Requested:BrokerResult::Restarted;
}
}
