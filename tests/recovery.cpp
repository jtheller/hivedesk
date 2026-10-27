#include "hivedesk/recovery.h"
#include <fstream>
#include <iostream>
#include <thread>

namespace {
namespace recovery=hive::recovery;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::filesystem::path root(){wchar_t path[32768]{};require(GetEnvironmentVariableW(L"HIVEDESK_RECOVERY_TEST",path,32768)>0,"Test root missing");return path;}
void write(const wchar_t* name,const char* text){std::ofstream out(root()/name);out<<text;require(bool(out),"Write fixture");}
void await(const wchar_t* name){const auto end=GetTickCount64()+10000;while(!std::filesystem::exists(root()/name)&&GetTickCount64()<end)Sleep(10);require(std::filesystem::exists(root()/name),"Recovery child deadline");}
std::wstring mode;
LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    if(m==WM_CLOSE){write(L"close","close");if(mode==L"cleanup")Sleep(INFINITE);PostQuitMessage(0);return 0;}
    return DefWindowProcW(w,m,wp,lp);
}
int launcher(const wchar_t* selected){
    mode=selected;
    WNDCLASSW wc{};wc.lpfnWndProc=window_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"HiveDesk.Recovery.Test";
    require(RegisterClassW(&wc)!=0,"Register private hidden fixture");
    HWND window=CreateWindowW(wc.lpszClassName,L"",0,0,0,0,0,nullptr,nullptr,wc.hInstance,nullptr);require(window!=nullptr,"Hidden fixture window");
    if(mode==L"settings"){
        const auto exe=root()/L"settings"/L"HiveDesk.Settings.exe";std::wstring command=L"\""+exe.wstring()+L"\" --park";
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
        require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"Start stuck Settings fixture");
        CloseHandle(process.hThread);CloseHandle(process.hProcess);write(L"settings-pid",std::to_string(process.dwProcessId).c_str());
        await(L"settings-observed");
    }
    // A parked worker models the real command queue being stuck in media close.
    std::thread blocked([]{Sleep(INFINITE);});blocked.detach();
    SECURITY_ATTRIBUTES attrs{sizeof(attrs),nullptr,TRUE};recovery::Handle extra(CreateEventW(&attrs,TRUE,FALSE,nullptr));
    SetEnvironmentVariableW(L"HIVEDESK_RECOVERY_EXTRA",std::to_wstring(reinterpret_cast<uintptr_t>(extra.value)).c_str());
    hive::gpu_policy_restart::Relaunch next(false,true);require(next.commit(),"Start independent recovery child");
    await(L"started");require(WaitForSingleObject(extra.value,0)==WAIT_TIMEOUT,"Leaked unrelated inherited handle");
    if(mode==L"ui")Sleep(INFINITE);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    DestroyWindow(window);return 0;
}
int child(const wchar_t* text){
    HANDLE previous=nullptr;require(hive::gpu_policy_restart::parse_wait_handle(text,previous),"Parse previous handle");
    wchar_t extra[64]{};GetEnvironmentVariableW(L"HIVEDESK_RECOVERY_EXTRA",extra,64);HANDLE extra_handle=nullptr;
    require(hive::gpu_policy_restart::parse_wait_handle(extra,extra_handle),"Parse unrelated handle");SetEvent(extra_handle);
    write(L"started","started");bool forced=false;
    require(recovery::previous(previous,forced),"Predecessor recovery failed");
    write(L"finished",forced?"forced":"graceful");return 0;
}
void process_test(const wchar_t* selected,bool forced){
    const auto directory=std::filesystem::temp_directory_path()/(L"HiveDesk-recovery-"+std::to_wstring(GetCurrentProcessId())+L"-"+selected);
    require(std::filesystem::create_directory(directory),"Create isolated fixture directory");SetEnvironmentVariableW(L"HIVEDESK_RECOVERY_TEST",directory.c_str());
    struct Cleanup{std::filesystem::path directory;~Cleanup(){std::error_code ignored;for(auto name:{L"started",L"finished",L"close",L"settings-pid",L"settings-observed",L"HiveDesk.exe",L"settings/HiveDesk.Settings.exe",L"settings"})std::filesystem::remove(directory/name,ignored);std::filesystem::remove(directory,ignored);}}cleanup{directory};
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
    auto executable=std::filesystem::path(module);
    if(std::wstring(selected)==L"settings"){
        std::filesystem::create_directory(directory/L"settings");
        std::filesystem::copy_file(module,directory/L"settings"/L"HiveDesk.Settings.exe");
        executable=directory/L"HiveDesk.exe";std::filesystem::copy_file(module,executable);
    }
    std::wstring command=L"\""+executable.wstring()+L"\" --launcher "+selected;
    STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"Start fixture");
    recovery::Handle owner(process.hProcess),thread(process.hThread);const auto begin=GetTickCount64();
    recovery::Handle settings;
    if(std::wstring(selected)==L"settings"){
        await(L"settings-pid");std::ifstream input(directory/L"settings-pid");DWORD pid=0;input>>pid;
        settings.value=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);require(settings.value!=nullptr,"Observe Settings fixture");
        // The different executable must never qualify as our predecessor.
        HANDLE foreign=nullptr;require(DuplicateHandle(GetCurrentProcess(),settings.value,GetCurrentProcess(),&foreign,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"Copy foreign handle");
        bool killed=false;require(!recovery::previous(foreign,killed)&&WaitForSingleObject(settings.value,0)==WAIT_TIMEOUT,"Foreign predecessor was touched");
        require(!recovery::stop_process(settings.value,0,10)&&WaitForSingleObject(settings.value,0)==WAIT_TIMEOUT,"Missing termination rights did not fail promptly");
        write(L"settings-observed","ready");
    }
    const auto wait=WaitForSingleObject(owner.value,10000);if(wait!=WAIT_OBJECT_0)TerminateProcess(owner.value,ERROR_TIMEOUT);
    require(wait==WAIT_OBJECT_0,"Stuck predecessor survived deadline");DWORD code=1;GetExitCodeProcess(owner.value,&code);
    require(code==(forced?recovery::ExitCode:0),"Wrong predecessor exit reason");
    await(L"finished");std::ifstream in(directory/L"finished");std::string result;in>>result;
    require(result==(forced?"forced":"graceful"),"Incorrect recovery outcome");
    require(GetTickCount64()-begin<9000,"Recovery was not bounded");
    require((std::wstring(selected)==L"ui")||std::filesystem::exists(directory/L"close"),"Graceful close was not attempted");
    if(settings.value){require(WaitForSingleObject(settings.value,0)==WAIT_OBJECT_0,"Stuck Settings survived recovery");DWORD settings_code=0;GetExitCodeProcess(settings.value,&settings_code);require(settings_code==recovery::ExitCode,"Settings did not take force fallback");}
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==2&&wcscmp(argv[1],L"--park")==0){Sleep(INFINITE);return 0;}
    if(argc==4&&wcscmp(argv[1],L"--recover-after")==0&&wcscmp(argv[3],L"--quiet")==0)return child(argv[2]);
    if(argc==3&&wcscmp(argv[1],L"--launcher")==0)return launcher(argv[2]);
    require(argc==1,"Unexpected recovery arguments");
    HANDLE own=nullptr;require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&own,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,0)!=FALSE,"Self handle");
    bool forced=false;require(!recovery::previous(own,forced),"Recovery accepted itself");
    require(!recovery::previous(CreateEventW(nullptr,TRUE,FALSE,nullptr),forced),"Recovery accepted a nonprocess handle");
    recovery::Handle event(CreateEventW(nullptr,FALSE,FALSE,nullptr));
    if(!hive::update::is_system())require(!hive::gpu_policy_restart::signal_broker(event.value),"User-owned recovery event accepted");
    require(recovery::event_name(1)!=recovery::event_name(2),"Recovery event is not session scoped");
    process_test(L"normal",false);process_test(L"cleanup",true);process_test(L"ui",true);process_test(L"settings",false);
    std::cout<<"PASS recovery: parked command worker, graceful exit, stuck cleanup, hung UI, bounded forced termination, pinned identity, handle isolation and session-scoped service request\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL recovery: "<<error.what()<<'\n';return 1;}}
