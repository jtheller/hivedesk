#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <shlobj.h>
#include "hivedesk/bootstrap.h"
#include <sddl.h>
#include <string>
#include <cwchar>
#include <tlhelp32.h>
#include <vector>
#include "hivedesk/update.h"
#include "hivedesk/gpu_policy_restart.h"
#include "hivedesk/recovery.h"

namespace {
constexpr wchar_t name[]=L"HiveDeskAgent";
SERVICE_STATUS_HANDLE status_handle=nullptr;
SERVICE_STATUS service_status{};
HANDLE stop_event=nullptr,session_event=nullptr;
void report(DWORD state,DWORD error=NO_ERROR) {
    service_status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState=state;
    service_status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN|SERVICE_ACCEPT_SESSIONCHANGE|SERVICE_ACCEPT_POWEREVENT:0;
    service_status.dwWin32ExitCode=error;
    SetServiceStatus(status_handle,&service_status);
}
HANDLE host_process=nullptr,updater_process=nullptr,recovery_event=nullptr;
HANDLE prelogon_process=nullptr,prelogon_stop=nullptr;
DWORD host_session=0,recovery_session=0;std::wstring host_sid;
void service_error(const std::string& text){static std::string previous;static ULONGLONG time=0;const auto now=GetTickCount64();if(text!=previous||now-time>30000){hive::update::log(text);previous=text;time=now;}}
bool stop_prelogon(){
    if(prelogon_stop)SetEvent(prelogon_stop);
    if(prelogon_process){
        if(!hive::recovery::stop_process(prelogon_process,1500)){service_error("Prelogon handoff could not terminate its old endpoint; retrying.");return false;}
        CloseHandle(prelogon_process);prelogon_process=nullptr;
    }
    if(prelogon_stop){CloseHandle(prelogon_stop);prelogon_stop=nullptr;}return true;
}
void cache_identity(HANDLE user){
    auto identity=hive::bootstrap::initial();
    if(user){
        PWSTR path=nullptr;
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,user,&path))){
            const auto file=std::wstring(path)+L"\\HiveDesk\\settings.ini";CoTaskMemFree(path);
            wchar_t id[129]{},device_name[257]{};
            // Read only this user's public identity/preferences under their
            // token, then return to SYSTEM before updating the protected cache.
            if(ImpersonateLoggedOnUser(user)){
                GetPrivateProfileStringW(L"HiveDesk",L"DeviceId",L"",id,129,file.c_str());
                GetPrivateProfileStringW(L"HiveDesk",L"Name",L"",device_name,257,file.c_str());
                const bool enabled=GetPrivateProfileIntW(L"HiveDesk",L"Startup",1,file.c_str())!=0;
                if(!RevertToSelf())ExitProcess(ERROR_ACCESS_DENIED);
                if(hive::bootstrap::valid_id(id)){identity.id=id;if(device_name[0])identity.name=device_name;identity.enabled=enabled;}
            }
        }
    }
    if(!hive::bootstrap::save(identity))service_error("Cannot persist prelogon device identity; retrying.");
}
void start_host() {
    if(host_process&&WaitForSingleObject(host_process,0)==WAIT_OBJECT_0){CloseHandle(host_process);host_process=nullptr;}
    if(prelogon_process&&WaitForSingleObject(prelogon_process,0)==WAIT_OBJECT_0){CloseHandle(prelogon_process);prelogon_process=nullptr;}
    DWORD session=WTSGetActiveConsoleSessionId();if(session==0xffffffff||!session)return;
    HANDLE user=nullptr;const bool signed_in=WTSQueryUserToken(session,&user)!=FALSE;
    if(!signed_in&&GetLastError()!=ERROR_NO_TOKEN){service_error("Console user token temporarily unavailable: "+std::to_string(GetLastError()));return;}
    std::wstring sid=L"S-1-5-18";
    if(user){
        BYTE information[4096]{};DWORD size=0;LPWSTR value=nullptr;
        const bool ok=GetTokenInformation(user,TokenUser,information,sizeof(information),&size)&&ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(information)->User.Sid,&value);
        if(!ok){CloseHandle(user);return;}sid=value;LocalFree(value);
    }
    cache_identity(user);if(user)CloseHandle(user);
    const bool changed=hive::bootstrap::replace_host(host_session,host_sid,session,sid);
    if(signed_in||changed||!hive::bootstrap::load().enabled){if(!stop_prelogon())return;}
    if(changed){
        if(host_process){if(!hive::recovery::stop_process(host_process,0))return;CloseHandle(host_process);host_process=nullptr;}
        if(recovery_event){CloseHandle(recovery_event);recovery_event=nullptr;}
    }
    if(!signed_in&&(!hive::bootstrap::load().enabled||hive::update::pending()))return;
    if(host_process&&signed_in&&!changed)return;
    if(!recovery_event){recovery_event=hive::recovery::create_event(session,sid);recovery_session=session;}
    const std::wstring sddl=L"O:SYG:SYD:P(A;;GA;;;SY)(A;;0x00121000;;;"+sid+L")";
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    HANDLE self=nullptr,token=nullptr;
    bool ok=OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY,&self)&&DuplicateTokenEx(self,TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&token);
    if(self)CloseHandle(self);
    if(!ok){LocalFree(descriptor);return;}
    if(!SetTokenInformation(token,TokenSessionId,&session,sizeof(session))){CloseHandle(token);LocalFree(descriptor);return;}
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    std::wstring directory=path;directory.resize(directory.find_last_of(L"\\/"));
    auto launch=[&](const wchar_t* filename,const std::wstring& arguments,HANDLE& target){
        const auto exe=directory+L"\\"+filename;std::wstring command=L"\""+exe+L"\" "+arguments;
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.lpDesktop=const_cast<wchar_t*>(L"winsta0\\default");startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION process{};
        if(CreateProcessAsUserW(token,exe.c_str(),command.data(),&attributes,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)){CloseHandle(process.hThread);target=process.hProcess;return true;}
        service_error("Console process startup failed: "+std::to_string(GetLastError()));return false;
    };
    if(!host_process&&launch(L"HiveDeskHost.exe",L"--parent "+std::to_wstring(GetCurrentProcessId()),host_process)){
        host_session=session;host_sid=sid;hive::update::log(signed_in?"Desktop broker ready for signed-in console.":"Desktop broker starting before first logon.");
    }
    if(host_process&&!signed_in&&!prelogon_process){
        if(!prelogon_stop)prelogon_stop=hive::update::create_system_event(hive::bootstrap::stop_event(session).c_str());
        if(prelogon_stop){ResetEvent(prelogon_stop);launch(L"HiveDesk.exe",L"--prelogon "+std::to_wstring(GetCurrentProcessId()),prelogon_process);}
    }
    CloseHandle(token);LocalFree(descriptor);
}
DWORD WINAPI control(DWORD code,DWORD event,void*,void*) {
    if(code==SERVICE_CONTROL_STOP||code==SERVICE_CONTROL_SHUTDOWN){report(SERVICE_STOP_PENDING);SetEvent(stop_event);return NO_ERROR;}
    if(code==SERVICE_CONTROL_SESSIONCHANGE&&(event==WTS_SESSION_LOGOFF||event==WTS_SESSION_LOGON||event==WTS_CONSOLE_CONNECT||event==WTS_SESSION_LOCK||event==WTS_SESSION_UNLOCK)) {SetEvent(session_event);return NO_ERROR;}
    if(code==SERVICE_CONTROL_POWEREVENT)SetEvent(session_event);
    return NO_ERROR;
}
void WINAPI service_main(DWORD,LPWSTR*) {
    status_handle=RegisterServiceCtrlHandlerExW(name,control,nullptr);if(!status_handle)return;
    report(SERVICE_START_PENDING);
    stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);session_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!stop_event||!session_event){report(SERVICE_STOPPED,GetLastError());return;}
    report(SERVICE_RUNNING);start_host();hive::update::launch_worker(updater_process);
    auto next_updater=GetTickCount64()+60000;
    for(;;){
        // A policy restart exits only the idle broker. Observe its process
        // handle so the replacement does not wait for the one-second watchdog.
        std::vector<HANDLE> events{stop_event,session_event};
        const DWORD recovery_index=DWORD(events.size());if(recovery_event)events.push_back(recovery_event);
        const DWORD host_index=DWORD(events.size());if(host_process)events.push_back(host_process);
        const auto wait=WaitForMultipleObjects(DWORD(events.size()),events.data(),FALSE,1000);
        if(wait==WAIT_OBJECT_0||wait==WAIT_FAILED)break;
        if(recovery_event&&wait==WAIT_OBJECT_0+recovery_index){
            // Explicit emergency restart bypasses broker/GPU cleanup and idle
            // admission. The service acts only on its own pinned child handle.
            if(host_process&&host_session==recovery_session){
                if(hive::recovery::stop_process(host_process,0)){
                    CloseHandle(host_process);host_process=nullptr;
                    hive::update::log("Manual recovery: Host terminated; creating replacement.");
                }else hive::update::log("Manual recovery: Windows could not terminate Host within deadline.");
            }
        }else if(host_process&&wait==WAIT_OBJECT_0+host_index){
            DWORD exit_code=0;GetExitCodeProcess(host_process,&exit_code);
            if(exit_code!=hive::gpu_policy_restart::BrokerRestartExitCode){
                // Failed initialization must not create an unbounded spawn
                // loop. Session/stop controls remain responsive during delay.
                const auto retry=WaitForMultipleObjects(2,events.data(),FALSE,1000);
                if(retry==WAIT_OBJECT_0||retry==WAIT_FAILED)break;
            }
        }
        start_host();
        if(GetTickCount64()>=next_updater){hive::update::launch_worker(updater_process);next_updater=GetTickCount64()+60000;}
    }
    stop_prelogon();
    // The updater must survive this service being stopped by its own installer.
    if(updater_process)CloseHandle(updater_process);
    if(recovery_event)CloseHandle(recovery_event);
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
                SC_ACTION actions[]={{SC_ACTION_RESTART,1000},{SC_ACTION_RESTART,3000},{SC_ACTION_RESTART,10000}};
                SERVICE_FAILURE_ACTIONSW recovery{};recovery.dwResetPeriod=60;recovery.cActions=3;recovery.lpsaActions=actions;
                if(!ChangeServiceConfig2W(service,SERVICE_CONFIG_FAILURE_ACTIONS,&recovery))result=static_cast<int>(GetLastError());
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
