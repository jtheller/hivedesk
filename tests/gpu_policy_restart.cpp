#include "hivedesk/gpu_policy_restart.h"
#include <fstream>
#include <iostream>
#include <thread>

namespace {
namespace restart=hive::gpu_policy_restart;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::wstring environment(const wchar_t* name){wchar_t value[32768]{};const auto size=GetEnvironmentVariableW(name,value,32768);require(size&&size<32768,"Missing restart fixture environment");return value;}
void write(const std::filesystem::path& path,const char* text){std::ofstream out(path);out<<text;out.close();require(bool(out),"Write restart fixture");}
void await_file(const std::filesystem::path& path){const auto until=GetTickCount64()+5000;while(!std::filesystem::exists(path)&&GetTickCount64()<until)Sleep(10);require(std::filesystem::exists(path),"Restart fixture timed out");}
int child(const wchar_t* value){
    HANDLE previous=nullptr,unrelated=nullptr;
    require(restart::parse_wait_handle(value,previous),"Child process handle parse");
    require(restart::parse_wait_handle(environment(L"HIVEDESK_RESTART_TEST_EXTRA").c_str(),unrelated),"Extra handle parse");
    const auto root=std::filesystem::path(environment(L"HIVEDESK_RESTART_TEST_ROOT"));
    // This event belongs only to this test worker; successful signalling would
    // reveal that the relaunch leaked an unrelated inheritable handle.
    SetEvent(unrelated);
    write(root/L"started","started");
    require(restart::wait_for_previous(previous),"Child did not wait for its same-executable predecessor");
    write(root/L"finished","finished");return 0;
}
int launcher(){
    const auto root=std::filesystem::path(environment(L"HIVEDESK_RESTART_TEST_ROOT"));
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE unrelated=CreateEventW(&security,TRUE,FALSE,nullptr);require(unrelated!=nullptr,"Create unrelated inheritable fixture");
    SetEnvironmentVariableW(L"HIVEDESK_RESTART_TEST_EXTRA",std::to_wstring(reinterpret_cast<uintptr_t>(unrelated)).c_str());
    restart::Relaunch next;require(next.commit(),"Resume replacement child");
    await_file(root/L"started");
    require(WaitForSingleObject(unrelated,0)==WAIT_TIMEOUT,"Relaunch inherited an unrelated handle");
    Sleep(100);require(!std::filesystem::exists(root/L"finished"),"Replacement ran before predecessor exited");
    CloseHandle(unrelated);return 0;
}
void process_test(){
    const auto root=std::filesystem::temp_directory_path()/(L"HiveDesk-restart-check-"+std::to_wstring(GetCurrentProcessId()));
    require(std::filesystem::create_directory(root),"Create isolated restart fixture");
    struct Cleanup{std::filesystem::path root;~Cleanup(){std::error_code ignored;std::filesystem::remove(root/L"started",ignored);std::filesystem::remove(root/L"finished",ignored);std::filesystem::remove(root,ignored);}}cleanup{root};
    SetEnvironmentVariableW(L"HIVEDESK_RESTART_TEST_ROOT",root.c_str());
    wchar_t module[32768]{};require(GetModuleFileNameW(nullptr,module,32768)!=0,"Find test executable");
    std::wstring command=L"\""+std::wstring(module)+L"\" --launcher";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(module,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"Start isolated relaunch fixture");
    CloseHandle(process.hThread);
    const auto waited=WaitForSingleObject(process.hProcess,10000);
    DWORD exit_code=1;GetExitCodeProcess(process.hProcess,&exit_code);
    if(waited!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,ERROR_TIMEOUT);
    CloseHandle(process.hProcess);require(waited==WAIT_OBJECT_0&&exit_code==0,"Predecessor fixture failed");
    await_file(root/L"finished");SetEnvironmentVariableW(L"HIVEDESK_RESTART_TEST_ROOT",nullptr);
}
void event_test(){
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(restart::event_security(L"S-1-5-21-1-2-3-1001").c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"Parse broker event ACL");
    PSID owner=nullptr;BOOL defaulted=FALSE,present=FALSE;PACL acl=nullptr;
    require(GetSecurityDescriptorOwner(descriptor,&owner,&defaulted)&&IsWellKnownSid(owner,WinLocalSystemSid),"Broker event owner is not SYSTEM");
    require(GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted)&&present&&acl&&acl->AceCount==2,"Unexpected broker event ACL");
    ACCESS_ALLOWED_ACE* ace=nullptr;require(GetAce(acl,1,reinterpret_cast<void**>(&ace))!=FALSE,"Missing console user ACL");
    PSID expected=nullptr;require(ConvertStringSidToSidW(L"S-1-5-21-1-2-3-1001",&expected)!=FALSE,"SID fixture");
    require(ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE&&ace->Mask==(EVENT_MODIFY_STATE|READ_CONTROL)&&EqualSid(&ace->SidStart,expected),"Console user has excessive event rights");
    LocalFree(expected);LocalFree(descriptor);
    require(!restart::signal_broker(nullptr)&&!restart::signal_broker(INVALID_HANDLE_VALUE),"Invalid broker event accepted");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"Create isolated broker event fixture");
    if(!hive::update::is_system()){
        require(!restart::signal_broker(event),"User-owned event accepted as broker");
        require(WaitForSingleObject(event,0)==WAIT_TIMEOUT,"Rejected event was signalled");
    }else{
        require(restart::signal_broker(event)&&restart::signal_broker(event),"SYSTEM event rejected");
        require(WaitForSingleObject(event,0)==WAIT_OBJECT_0&&WaitForSingleObject(event,0)==WAIT_TIMEOUT,"Broker requests did not coalesce");
    }
    CloseHandle(event);
}
void marker_test(){
    const auto root=std::filesystem::temp_directory_path()/(L"HiveDesk-restart-marker-check-"+std::to_wstring(GetCurrentProcessId()));
    require(std::filesystem::create_directory(root),"Create marker fixture");
    const auto path=root/L"pending";
    struct Cleanup{std::filesystem::path root;~Cleanup(){std::error_code ignored;std::filesystem::remove(root/L"pending",ignored);std::filesystem::remove(root,ignored);}}cleanup{root};
    require(!restart::pending_request(path)&&restart::clear_request(path),"Absent marker is not idle");
    require(restart::save_request(path)&&restart::pending_request(path),"Restart request did not persist");
    const auto size=std::filesystem::file_size(path);
    require(size>0&&size<128&&restart::save_request(path)&&std::filesystem::file_size(path)==size,"Restart marker grows across requests");
    HANDLE held=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(held!=INVALID_HANDLE_VALUE,"Open locked marker fixture");
    require(!restart::save_request(path)&&!restart::clear_request(path)&&restart::pending_request(path),"Locked marker request did not report failure or lost existing intent");
    CloseHandle(held);
    require(restart::clear_request(path)&&!restart::pending_request(path),"Completed restart marker not cleared");
    require(!restart::save_request(root)&&!restart::pending_request(root),"Directory accepted as marker");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3&&wcscmp(argv[1],L"--restart-after")==0)return child(argv[2]);
    if(argc==2&&wcscmp(argv[1],L"--launcher")==0)return launcher();
    require(argc==1,"Unexpected test command");
    for(unsigned flags=0;flags<16;++flags)require(hive::update::idle_for_update(flags&1,flags&2,flags&4,flags&8)==(flags==0),"Restart can bypass session/queue barrier");
    restart::SettingsAdmission admission;
    require(admission.open()&&admission.open()&&!admission.prepare(),"Open Settings did not block restart");
    admission.close();require(admission.prepare()&&!admission.open(),"Committed restart admitted Settings");
    admission.close();require(!admission.open(),"Late Settings close removed restart admission");
    admission.cancel();require(admission.open(),"Failed restart did not restore Settings admission");
    HANDLE parsed=nullptr;
    for(const auto* invalid:{L"",L"0",L"-1",L"12x",L" 12",L"18446744073709551616",L"18446744073709551615"})require(!restart::parse_wait_handle(invalid,parsed),"Invalid process handle accepted");
    require(restart::parse_wait_handle(L"128",parsed)&&reinterpret_cast<uintptr_t>(parsed)==128,"Valid process handle rejected");
    event_test();marker_test();process_test();
    std::cout<<"PASS GPU restart: idle barriers, atomic Settings admission, SYSTEM owner and console ACL, bounded persistent request, process exit ordering, restricted inherited handle list\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL GPU restart: "<<error.what()<<'\n';return 1;}}
