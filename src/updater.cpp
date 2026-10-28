#include "hivedesk/update.h"
#include <tlhelp32.h>
#include <userenv.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
namespace update=hive::update;
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct Barrier {
    HANDLE gate,event;bool owned=false;
    ~Barrier(){if(owned){ResetEvent(event);ReleaseMutex(gate);}}
    bool take(){const auto result=WaitForSingleObject(gate,0);owned=result==WAIT_OBJECT_0||result==WAIT_ABANDONED;return owned;}
};
std::vector<uint8_t> read(const std::filesystem::path& file){std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("Cannot open verification file");return {std::istreambuf_iterator<char>(input),{}};}
update::Manifest latest(){
    const auto manifest=update::download(L"https://github.com/jtheller/hivedesk/releases/latest/download/update.manifest",4096);
    const auto signature=update::download(L"https://github.com/jtheller/hivedesk/releases/latest/download/update.manifest.sig",1024);
    return update::verify_manifest(manifest,signature);
}
// Preserve only normal user tokens from agents that actually participated in
// this update. The installer runs as SYSTEM; the tray must never do so.
struct Users {
    std::map<DWORD,HANDLE> tokens;
    ~Users(){for(auto [session,token]:tokens){(void)session;CloseHandle(token);}}
    void remember(HANDLE process,DWORD pid){
        DWORD session=0;if(!ProcessIdToSessionId(pid,&session)||!session||tokens.contains(session))return;
        Handle source;HANDLE token=nullptr;
        if(OpenProcessToken(process,TOKEN_QUERY|TOKEN_DUPLICATE,&source.value)&&DuplicateTokenEx(source.value,TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&token)){
            BYTE info[1024]{};DWORD size=0;
            if(GetTokenInformation(token,TokenUser,info,sizeof(info),&size)){
                if(IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(info)->User.Sid,WinLocalSystemSid)){CloseHandle(token);return;} // Prelogon endpoint: service restarts it, never a user tray.
                tokens.emplace(session,token);
            }else CloseHandle(token);
        }
        if(!tokens.contains(session))throw std::runtime_error("Cannot retain user token; update deferred");
    }
    void restart(const std::filesystem::path& root){
        const auto exe=root/L"HiveDesk.exe";
        for(auto [session,token]:tokens){
            (void)session;void* environment=nullptr;CreateEnvironmentBlock(&environment,token,FALSE);
            std::wstring command=L"\""+exe.wstring()+L"\" --quiet";STARTUPINFOW startup{sizeof(startup)};startup.lpDesktop=const_cast<wchar_t*>(L"winsta0\\default");startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
            if(CreateProcessAsUserW(token,exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,environment,root.c_str(),&startup,&process)){CloseHandle(process.hThread);CloseHandle(process.hProcess);}else update::log("Could not resume user tray: "+std::to_string(GetLastError()));
            if(environment)DestroyEnvironmentBlock(environment);
        }
    }
};
bool processes_present(const std::filesystem::path& root,Users& users){
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));if(snapshot.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot enumerate agents; update deferred");
    PROCESSENTRY32W entry{sizeof(entry)};bool found=false;
    if(Process32FirstW(snapshot.value,&entry))do{
        const bool agent=_wcsicmp(entry.szExeFile,L"HiveDesk.exe")==0,settings=_wcsicmp(entry.szExeFile,L"HiveDesk.Settings.exe")==0;
        if(!agent&&!settings)continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,entry.th32ProcessID));
        if(!process.value){if(GetLastError()!=ERROR_INVALID_PARAMETER)throw std::runtime_error("Cannot inspect agent; update deferred");continue;}
        wchar_t path[32768]{};DWORD size=32768;if(!QueryFullProcessImageNameW(process.value,0,path,&size))throw std::runtime_error("Cannot verify agent path; update deferred");
        const auto expected=agent?root/L"HiveDesk.exe":root/L"settings"/L"HiveDesk.Settings.exe";
        if(_wcsicmp(path,expected.c_str())!=0)continue;
        if(WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT)continue;
        found=true;if(agent)users.remember(process.value,entry.th32ProcessID);
    }while(Process32NextW(snapshot.value,&entry));
    return found;
}
bool still_installed(const std::filesystem::path& root){return !root.empty()&&root==update::installed_directory()&&std::filesystem::exists(root/L"HiveDesk.exe");}
bool wait_installed(const std::filesystem::path& root,unsigned seconds,HANDLE check=nullptr){
    for(unsigned i=0;i<seconds;++i){
        if(!still_installed(root))return false;
        if(!check){Sleep(1000);continue;}
        const auto result=WaitForSingleObject(check,1000);
        if(result==WAIT_OBJECT_0)return still_installed(root);
        if(result!=WAIT_TIMEOUT)throw std::runtime_error("Cannot wait for update check request");
    }
    return still_installed(root);
}
int check_now(){
    const auto manifest=latest();
    // Packaged helpers sit beside HiveDesk.exe. Standalone development helpers
    // can compare their own build version, but a damaged installed tree must
    // not silently report itself up to date.
    const auto root=update::module_directory();
    const auto running=std::filesystem::exists(root/L"HiveDesk.exe")||update::is_installed_agent()
        ?update::installed_version(root):std::string(HIVEDESK_VERSION);
    update::Version local,remote;
    if(!update::parse_version(running,local)||!update::parse_version(manifest.version,remote))throw std::runtime_error("Cannot compare update versions");
    const char* status=remote<=local?"current":update::request_check()?"requested":"unavailable";
    std::cout<<"HiveDesk-Update-Check-1\nstatus="<<status<<"\nversion="<<manifest.version<<'\n';
    return 0;
}
void install(const update::Manifest& manifest,const std::filesystem::path& package,const std::filesystem::path& root,HANDLE gate,HANDLE event){
    Users users;Barrier barrier{gate,event};if(!barrier.take()){update::log("Startup barrier busy; update deferred");return;}
    // Every installed app admits no new sessions once it accepts this request,
    // and exits only when host/view/reconnect work is idle. New processes cannot
    // cross the held startup barrier. No live agent is forcibly terminated.
    update::log("Update "+manifest.version+" staged; waiting for remote sessions to become idle");
    bool attempted=false;
    try{
        processes_present(root,users); // Retain user tokens before idle agents exit.
        if(!SetEvent(event))throw std::runtime_error("Cannot request idle update");
        while(processes_present(root,users)){if(!wait_installed(root,1))throw std::runtime_error("Installation removed while waiting");}
        // Retain the barrier through setup. Recheck signed size/hash after the
        // potentially long active-session wait, then recheck installed version.
        update::Version current,next;const auto running=update::installed_version(root);
        if(!update::parse_version(running,current)||!update::parse_version(manifest.version,next)||next<=current){update::log("Staged update is no longer newer");}
        else {
            update::verify_package(manifest,package);
            const auto log=update::private_cache()/L"setup.log";
            std::wstring command=L"\""+package.wstring()+L"\" /OTA=1 /VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART /NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /RESTARTEXITCODE=3010 /DIR=\""+root.wstring()+L"\" /LOG=\""+log.wstring()+L"\"";
            STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
            if(!CreateProcessW(package.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,package.parent_path().c_str(),&startup,&process))throw std::runtime_error("Could not launch signed update installer");
            attempted=true;Handle setup(process.hProcess);CloseHandle(process.hThread);WaitForSingleObject(setup.value,INFINITE);DWORD code=1;GetExitCodeProcess(setup.value,&code);
            if(code!=0)throw std::runtime_error("Installer returned "+std::to_string(code)+"; no forced reboot will be performed");
            if(update::installed_version(root)!=manifest.version)throw std::runtime_error("Installed version does not match release");
            update::log("Installed "+manifest.version+" successfully");
        }
    }catch(const std::exception& error){update::log(error.what());}
    // Release first: a resumed ordinary-user agent must pass its startup guard.
    ResetEvent(event);ReleaseMutex(gate);barrier.owned=false;
    if(still_installed(root))users.restart(root);
    if(attempted){
        // Avoid a rapid retry loop after setup failure; no app is held stopped
        // during this cooldown. Successful upgrades can hand off immediately.
        if(still_installed(root)&&update::installed_version(root)!=manifest.version)wait_installed(root,15*60);
        throw std::runtime_error("Update worker finished installation attempt; service will start a fresh worker");
    }
}
int run(){
    if(!update::is_system())throw std::runtime_error("Automatic updater is started by the HiveDesk system service");
    Handle singleton(update::create_system_mutex(L"Global\\HiveDesk.Update.Worker.v1"));
    const auto acquired=WaitForSingleObject(singleton.value,0);if(acquired!=WAIT_OBJECT_0&&acquired!=WAIT_ABANDONED)return 0;
    const auto root=update::installed_directory();if(!still_installed(root))return 0;
    Handle gate(update::create_system_mutex(update::StartupMutex,true)),event(update::create_system_event(update::PendingEvent)),check(update::create_system_check_event());ResetEvent(event.value);
    const auto cache=update::private_cache();update::log("Updater " HIVEDESK_VERSION " started");
    if(!wait_installed(root,60+GetCurrentProcessId()%31,check.value))return 0;
    for(;;){
        bool attempt_finished=false;
        try{
            const auto manifest=latest();update::Version local,remote;
            if(!update::parse_version(update::installed_version(root),local)||!update::parse_version(manifest.version,remote))throw std::runtime_error("Cannot compare update versions");
            if(remote>local){
                const auto package=cache/std::wstring(manifest.file.begin(),manifest.file.end());
                bool valid=false;if(std::filesystem::exists(package)){try{update::verify_package(manifest,package);valid=true;}catch(...){}}
                if(!valid){const auto partial=cache/L"installer.part";update::download_file(update::release_url(manifest),partial,manifest.size);update::verify_package(manifest,partial);if(!MoveFileExW(partial.c_str(),package.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot finish installer staging");}
                // This helper lives outside Program Files, so setup can replace
                // all installed binaries while it waits for completion.
                try{install(manifest,package,root,gate.value,event.value);}catch(const std::exception& message){update::log(message.what());attempt_finished=true;}
            }
        }catch(const std::exception& error){update::log(std::string("Update check deferred: ")+error.what());}
        if(attempt_finished)return 0;
        if(!wait_installed(root,15*60,check.value))return 0;
    }
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==2&&wcscmp(argv[1],L"--check-now")==0)return check_now();
        if(argc==2&&wcscmp(argv[1],L"--check-only")==0){const auto m=latest();std::cout<<"Verified release "<<m.version<<", "<<m.file<<", "<<m.size<<" bytes\n";return 0;}
        if(argc==3&&wcscmp(argv[1],L"--verify-release")==0){const std::filesystem::path root=argv[2];const auto m=update::verify_manifest(read(root/L"update.manifest"),read(root/L"update.manifest.sig"));update::verify_package(m,root/std::wstring(m.file.begin(),m.file.end()));std::cout<<"PASS signed installer "<<m.version<<'\n';return 0;}
        if(argc!=1)throw std::runtime_error("Unsupported updater arguments");return run();
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';if(update::is_system())update::log(error.what());return 1;}
}
