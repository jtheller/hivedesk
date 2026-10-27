#pragma once
#include "update.h"
#include <sddl.h>
#include <atomic>
#include <limits>
#include <stdexcept>

namespace hive::gpu_policy_restart {
inline bool pending_request(const std::filesystem::path& path) {
    const auto attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_DIRECTORY);
}
inline bool save_request(const std::filesystem::path& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    constexpr char marker[]="HiveDesk video restart v1\n";DWORD written=0;
    const bool ok=WriteFile(file,marker,sizeof(marker)-1,&written,nullptr)&&written==sizeof(marker)-1&&FlushFileBuffers(file);
    CloseHandle(file);if(!ok)DeleteFileW(path.c_str());return ok;
}
inline bool clear_request(const std::filesystem::path& path) {
    if(DeleteFileW(path.c_str()))return true;
    const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;
}
class SettingsAdmission {
    std::atomic<unsigned> state_{0}; // closed, open, or restart committed
public:
    bool open(){unsigned expected=0;return state_.compare_exchange_strong(expected,1)||expected==1;}
    void close(){unsigned expected=1;state_.compare_exchange_strong(expected,0);}
    bool prepare(){unsigned expected=0;return state_.compare_exchange_strong(expected,2);}
    void cancel(){unsigned expected=2;state_.compare_exchange_strong(expected,0);}
};
// Local is scoped to the interactive session shared by the tray and broker.
constexpr wchar_t EventName[]=L"Local\\HiveDesk.Video.Restart.v1";
constexpr DWORD BrokerRestartExitCode=0x48445652;
inline std::wstring event_security(const std::wstring& user_sid) {
    // The console user may inspect ownership and signal, but cannot consume,
    // reset ownership, or change the event's ACL.
    return L"O:SYG:SYD:P(A;;GA;;;SY)(A;;0x00020002;;;"+user_sid+L")";
}
inline HANDLE create_broker_event(const std::wstring& user_sid) {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    const auto security=event_security(user_sid);
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(security.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    // Auto-reset consumes one request, even while the exiting tray retains a
    // handle. A replacement broker must not inherit an already consumed signal.
    HANDLE event=CreateEventW(&attributes,FALSE,FALSE,EventName);LocalFree(descriptor);
    if(event&&!update::system_owned(event)){CloseHandle(event);event=nullptr;}
    return event;
}
inline HANDLE open_broker_event() {
    HANDLE event=OpenEventW(EVENT_MODIFY_STATE|READ_CONTROL,FALSE,EventName);
    if(event&&!update::system_owned(event)){CloseHandle(event);event=nullptr;}
    return event;
}
inline bool signal_broker(HANDLE event) {
    return event&&event!=INVALID_HANDLE_VALUE&&update::system_owned(event)&&SetEvent(event)!=FALSE;
}
inline bool parse_wait_handle(const wchar_t* text,HANDLE& result) {
    if(!text||!*text)return false;
    uintptr_t value=0;
    for(const wchar_t* p=text;*p;++p){
        if(*p<L'0'||*p>L'9')return false;
        const auto digit=uintptr_t(*p-L'0');
        if(value>(std::numeric_limits<uintptr_t>::max()-digit)/10)return false;
        value=value*10+digit;
    }
    if(!value||value==std::numeric_limits<uintptr_t>::max())return false;
    result=reinterpret_cast<HANDLE>(value);return true;
}
inline bool wait_for_previous(HANDLE process) {
    const DWORD pid=GetProcessId(process);DWORD own_session=0,previous_session=0;
    wchar_t path[32768]{},own_path[32768]{};DWORD length=32768;
    const bool valid=pid&&pid!=GetCurrentProcessId()&&
        ProcessIdToSessionId(pid,&previous_session)&&ProcessIdToSessionId(GetCurrentProcessId(),&own_session)&&own_session==previous_session&&
        QueryFullProcessImageNameW(process,0,path,&length)&&GetModuleFileNameW(nullptr,own_path,32768)&&
        CompareStringOrdinal(path,-1,own_path,-1,TRUE)==CSTR_EQUAL;
    const bool exited=valid&&WaitForSingleObject(process,INFINITE)==WAIT_OBJECT_0;
    CloseHandle(process);return exited;
}
// Creation happens while the old agent is still running. Failure leaves it
// operational. The suspended child cannot touch GPU/UI state before commit.
class Relaunch {
    PROCESS_INFORMATION child_{};
    bool committed_=false;
public:
    explicit Relaunch(bool show_settings=true) {
        HANDLE previous=nullptr;
        if(!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&previous,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0))throw std::runtime_error("Prepare video restart process handle");
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
        auto* attributes=static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(),0,size));
        if(!attributes){CloseHandle(previous);throw std::runtime_error("Allocate video restart attributes");}
        const bool initialized=InitializeProcThreadAttributeList(attributes,1,0,&size)!=FALSE;
        const bool listed=initialized&&UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&previous,sizeof(previous),nullptr,nullptr)!=FALSE;
        wchar_t module[32768]{};const bool path_ok=GetModuleFileNameW(nullptr,module,32768)!=0;
        const std::filesystem::path executable=module;
        std::wstring command=L"\""+executable.wstring()+L"\" --restart-after "+std::to_wstring(reinterpret_cast<uintptr_t>(previous));
        if(!show_settings)command+=L" --quiet";
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.lpAttributeList=attributes;
        const bool created=listed&&path_ok&&CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,executable.parent_path().c_str(),&startup.StartupInfo,&child_)!=FALSE;
        if(initialized)DeleteProcThreadAttributeList(attributes);HeapFree(GetProcessHeap(),0,attributes);CloseHandle(previous);
        if(!created)throw std::runtime_error("Could not prepare video restart; HiveDesk is still running");
    }
    Relaunch(const Relaunch&)=delete;Relaunch& operator=(const Relaunch&)=delete;
    ~Relaunch(){
        // Only our own never-resumed child is cancelled, never an active app.
        if(child_.hProcess&&!committed_)TerminateProcess(child_.hProcess,ERROR_CANCELLED);
        if(child_.hThread)CloseHandle(child_.hThread);if(child_.hProcess)CloseHandle(child_.hProcess);
    }
    bool commit(){if(ResumeThread(child_.hThread)==DWORD(-1))return false;committed_=true;return true;}
};
}
