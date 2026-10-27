#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <string>
#include <cwchar>
#include <tlhelp32.h>
#include <vector>
#include "hivedesk/update.h"
#include "hivedesk/gpu_policy_restart.h"

namespace {
constexpr wchar_t name[]=L"HiveDeskAgent";
SERVICE_STATUS_HANDLE status_handle=nullptr;
SERVICE_STATUS service_status{};
HANDLE stop_event=nullptr,session_event=nullptr;
void report(DWORD state,DWORD error=NO_ERROR) {
    service_status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState=state;
    service_status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN|SERVICE_ACCEPT_SESSIONCHANGE:0;
    service_status.dwWin32ExitCode=error;
    SetServiceStatus(status_handle,&service_status);
}
HANDLE host_process=nullptr,updater_process=nullptr;DWORD host_session=0;
void start_host() {
    if(host_process&&WaitForSingleObject(host_process,0)==WAIT_OBJECT_0){CloseHandle(host_process);host_process=nullptr;}
    DWORD session=WTSGetActiveConsoleSessionId();if(session==0xffffffff)return;
    if(host_process&&host_session==session&&WaitForSingleObject(host_process,0)==WAIT_TIMEOUT)return;
    if(host_process){if(WaitForSingleObject(host_process,0)==WAIT_TIMEOUT)TerminateProcess(host_process,0);CloseHandle(host_process);host_process=nullptr;}
    HANDLE user=nullptr;if(!WTSQueryUserToken(session,&user))return;
    BYTE information[4096]{};DWORD size=0;LPWSTR sid=nullptr;
    bool ok=GetTokenInformation(user,TokenUser,information,sizeof(information),&size)&&ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(information)->User.Sid,&sid);CloseHandle(user);if(!ok)return;
    // The ordinary tray process may query the broker executable identity, but
    // cannot read/write its memory, duplicate handles, or terminate it.
    const std::wstring sddl=L"O:SYG:SYD:P(A;;GA;;;SY)(A;;0x00101000;;;"+std::wstring(sid)+L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    HANDLE self=nullptr,token=nullptr;
    ok=OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY,&self)&&DuplicateTokenEx(self,TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&token);
    if(self)CloseHandle(self);
    if(!ok){LocalFree(descriptor);return;}
    if(!SetTokenInformation(token,TokenSessionId,&session,sizeof(session))){CloseHandle(token);LocalFree(descriptor);return;}
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    std::wstring directory=path;directory.resize(directory.find_last_of(L"\\/"));const auto exe=directory+L"\\HiveDeskHost.exe";
    std::wstring command=L"\""+exe+L"\" --parent "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.lpDesktop=const_cast<wchar_t*>(L"winsta0\\default");startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    if(CreateProcessAsUserW(token,exe.c_str(),command.data(),&attributes,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)){CloseHandle(process.hThread);host_process=process.hProcess;host_session=session;}
    CloseHandle(token);LocalFree(descriptor);
}
DWORD WINAPI control(DWORD code,DWORD event,void*,void*) {
    if(code==SERVICE_CONTROL_STOP||code==SERVICE_CONTROL_SHUTDOWN){report(SERVICE_STOP_PENDING);SetEvent(stop_event);return NO_ERROR;}
    if(code==SERVICE_CONTROL_SESSIONCHANGE&&(event==WTS_SESSION_LOGON||event==WTS_CONSOLE_CONNECT||event==WTS_SESSION_LOCK||event==WTS_SESSION_UNLOCK)) {SetEvent(session_event);return NO_ERROR;}
    return NO_ERROR;
}
void WINAPI service_main(DWORD,LPWSTR*) {
    status_handle=RegisterServiceCtrlHandlerExW(name,control,nullptr);if(!status_handle)return;
    report(SERVICE_START_PENDING);
    stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);session_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!stop_event||!session_event){report(SERVICE_STOPPED,GetLastError());return;}
    report(SERVICE_RUNNING);start_host();hive::update::launch_worker(updater_process);
    for(;;){
        // A policy restart exits only the idle broker. Observe its process
        // handle so the replacement does not wait for the 60-second watchdog.
        HANDLE events[]={stop_event,session_event,host_process};
        const auto wait=WaitForMultipleObjects(host_process?3:2,events,FALSE,60000);
        if(wait==WAIT_OBJECT_0||wait==WAIT_FAILED)break;
        if(host_process&&wait==WAIT_OBJECT_0+2){
            DWORD exit_code=0;GetExitCodeProcess(host_process,&exit_code);
            if(exit_code!=hive::gpu_policy_restart::BrokerRestartExitCode){
                // Failed initialization must not create an unbounded spawn
                // loop. Session/stop controls remain responsive during delay.
                const auto retry=WaitForMultipleObjects(2,events,FALSE,1000);
                if(retry==WAIT_OBJECT_0||retry==WAIT_FAILED)break;
            }
        }
        start_host();hive::update::launch_worker(updater_process);
    }
    // The updater must survive this service being stopped by its own installer.
    if(updater_process)CloseHandle(updater_process);
    CloseHandle(session_event);CloseHandle(stop_event);if(host_process)CloseHandle(host_process);report(SERVICE_STOPPED);
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==2&&(std::wcscmp(argv[1],L"--install")==0||std::wcscmp(argv[1],L"--uninstall")==0)) {
        const bool install=std::wcscmp(argv[1],L"--install")==0;
        SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT|(install?SC_MANAGER_CREATE_SERVICE:0));
        if(!manager)return static_cast<int>(GetLastError());
        int result=0;
        if(install) {
            wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
            const std::wstring quoted=L"\""+std::wstring(path)+L"\"";
            SC_HANDLE service=CreateServiceW(manager,name,L"HiveDesk desktop broker",SERVICE_START|SERVICE_CHANGE_CONFIG|SERVICE_QUERY_STATUS,SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,quoted.c_str(),nullptr,nullptr,nullptr,nullptr,nullptr);
            if(!service&&GetLastError()==ERROR_SERVICE_EXISTS) {
                service=OpenServiceW(manager,name,SERVICE_START|SERVICE_CHANGE_CONFIG|SERVICE_QUERY_STATUS);
                if(service&&!ChangeServiceConfigW(service,SERVICE_NO_CHANGE,SERVICE_AUTO_START,SERVICE_NO_CHANGE,quoted.c_str(),nullptr,nullptr,nullptr,nullptr,nullptr,nullptr))result=static_cast<int>(GetLastError());
            }
            if(!service)result=static_cast<int>(GetLastError());
            else {
                SERVICE_DESCRIPTIONW description{const_cast<wchar_t*>(L"Provides local screen capture/input and signed automatic updates while remote sessions are idle. The update worker retrieves public HiveDesk releases over HTTPS.")};
                ChangeServiceConfig2W(service,SERVICE_CONFIG_DESCRIPTION,&description);
                if(!result&&!StartServiceW(service,0,nullptr)&&GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING)result=static_cast<int>(GetLastError());
                CloseServiceHandle(service);
            }
        } else {
            SC_HANDLE service=OpenServiceW(manager,name,SERVICE_STOP|SERVICE_QUERY_STATUS|DELETE);
            if(!service) {const auto error=GetLastError();if(error!=ERROR_SERVICE_DOES_NOT_EXIST)result=static_cast<int>(error);}
            else {
                SERVICE_STATUS_PROCESS running{};DWORD needed=0;HANDLE service_process=nullptr;std::vector<HANDLE> children;
                if(QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&running),sizeof(running),&needed)&&running.dwProcessId){
                    service_process=OpenProcess(SYNCHRONIZE,FALSE,running.dwProcessId);
                    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
                    if(snapshot!=INVALID_HANDLE_VALUE){PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
                        if(Process32FirstW(snapshot,&entry))do {if(entry.th32ParentProcessID==running.dwProcessId&&_wcsicmp(entry.szExeFile,L"HiveDeskHost.exe")==0){HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,entry.th32ProcessID);if(child)children.push_back(child);}}while(Process32NextW(snapshot,&entry));
                        CloseHandle(snapshot);
                    }
                }
                SERVICE_STATUS state{};ControlService(service,SERVICE_CONTROL_STOP,&state);
                for(int i=0;i<100;++i) {if(!QueryServiceStatus(service,&state)||state.dwCurrentState==SERVICE_STOPPED)break;Sleep(100);}
                if(service_process){if(WaitForSingleObject(service_process,5000)!=WAIT_OBJECT_0)result=ERROR_BUSY;CloseHandle(service_process);}
                for(HANDLE child:children){if(WaitForSingleObject(child,5000)!=WAIT_OBJECT_0)result=ERROR_BUSY;CloseHandle(child);}
                if(!result&&!DeleteService(service))result=static_cast<int>(GetLastError());CloseServiceHandle(service);
            }
        }
        CloseServiceHandle(manager);return result;
    }
    SERVICE_TABLE_ENTRYW table[]={{const_cast<wchar_t*>(name),service_main},{nullptr,nullptr}};
    if(!StartServiceCtrlDispatcherW(table))return static_cast<int>(GetLastError());
    return 0;
}
