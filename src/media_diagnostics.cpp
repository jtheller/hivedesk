#include "media_diagnostics.h"
#include <filesystem>
#include <algorithm>
#include <vector>

namespace hive {
namespace {
struct Handle {
    HANDLE h=nullptr;
    ~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
};
bool ipv4(const std::string& value) {
    unsigned parts=0,n=0,digits=0;
    for(char c:value){
        if(c=='.'){if(!digits||++parts>3)return false;n=digits=0;}
        else if(c>='0'&&c<='9'){if(++digits>3||(n=n*10+c-'0')>255)return false;}
        else return false;
    }
    return parts==3&&digits;
}
}
MediaProbeReport bounded_probe_process(const std::wstring& executable,const std::wstring& arguments,
    DWORD timeout_ms,const std::function<bool()>& cancelled) {
    MediaProbeReport report;
    if(cancelled()){report.result=ERROR_CANCELLED;report.text="Diagnostic cancelled before launch";return report;}
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle read,write,input,job,process,thread;
    const auto failed=[&](const char* operation){report.result=GetLastError();report.text=std::string(operation)+" Win32="+std::to_string(report.result);return report;};
    if(!CreatePipe(&read.h,&write.h,&security,0)||!SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0))return failed("Diagnostic pipe");
    input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    if(input.h==INVALID_HANDLE_VALUE)return failed("Diagnostic input");
    job.h=CreateJobObjectW(nullptr,nullptr);if(!job.h)return failed("Diagnostic job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limit,sizeof(limit)))return failed("Diagnostic limits");
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow=SW_HIDE;startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=write.h;startup.StartupInfo.hStdInput=input.h;
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<uint8_t> attributes(bytes);
    startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes))return failed("Diagnostic attributes");
    struct Attributes {LPPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}} cleanup{startup.lpAttributeList};
    HANDLE inherited[]={write.h,input.h};
    if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))return failed("Diagnostic handles");
    std::wstring command=L"\""+executable+L"\""+(arguments.empty()?L"":L" "+arguments);PROCESS_INFORMATION info{};
    const auto directory=std::filesystem::path(executable).parent_path().wstring();
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,directory.c_str(),&startup.StartupInfo,&info))return failed("Diagnostic launch");
    process.h=info.hProcess;thread.h=info.hThread;
    if(!AssignProcessToJobObject(job.h,process.h)){const auto code=GetLastError();TerminateProcess(process.h,code);WaitForSingleObject(process.h,1000);SetLastError(code);return failed("Diagnostic containment");}
    if(ResumeThread(thread.h)==DWORD(-1))return failed("Diagnostic resume");
    CloseHandle(write.h);write.h=nullptr;
    const auto started=GetTickCount64();bool exited=false,truncated=false;
    for(;;){
        DWORD available=0;
        if(PeekNamedPipe(read.h,nullptr,0,nullptr,&available,nullptr)&&available){
            char block[4096];DWORD count=0;
            if(ReadFile(read.h,block,std::min<DWORD>(sizeof(block),available),&count,nullptr)&&count){
                const auto room=MaxMediaProbeReport-report.text.size();report.text.append(block,std::min<size_t>(count,room));truncated|=count>room;
            }
        }else if(exited)break;
        if(cancelled()||GetTickCount64()-started>=timeout_ms){
            report.result=cancelled()?ERROR_CANCELLED:WAIT_TIMEOUT;TerminateJobObject(job.h,report.result);WaitForSingleObject(process.h,1000);
            const std::string reason="\nDiagnostic cancelled or timed out\n";report.text.resize(std::min(report.text.size(),MaxMediaProbeReport-reason.size()));report.text+=reason;return report;
        }
        if(WaitForSingleObject(process.h,available?0:20)==WAIT_OBJECT_0)exited=true;
    }
    DWORD exit_code=0;if(!GetExitCodeProcess(process.h,&exit_code))return failed("Diagnostic exit status");report.result=exit_code;
    if(truncated){const std::string suffix="\n[diagnostic output truncated]\n";report.text.resize(MaxMediaProbeReport-suffix.size());report.text+=suffix;}
    return report;
}
MediaProbeReport run_media_probe(uint32_t mode,const std::string& source,const std::function<bool()>& cancelled) {
    wchar_t module[32768]{};const DWORD n=GetModuleFileNameW(nullptr,module,32768);
    if(!n||n>=32768)return {0,ERROR_BAD_PATHNAME,"Diagnostic installation path unavailable"};
    const auto root=std::filesystem::path(module).parent_path();
    if(mode==1||mode==2)return bounded_probe_process((root/L"HiveDeskMediaProbe.exe").wstring(),mode==2?L"--intel-roundtrip":L"",45000,cancelled);
    if(mode==3&&ipv4(source))return bounded_probe_process((root/L"HiveDeskRemoteVideoCheck.exe").wstring(),std::wstring(source.begin(),source.end())+L" 15000",30000,cancelled);
    if(mode==4)return bounded_probe_process((root/L"HiveDeskProductionHostCheck.exe").wstring(),L"",30000,cancelled);
    return {0,ERROR_INVALID_PARAMETER,"Invalid fixed diagnostic mode or source address"};
}
void MediaDiagnostics::stop(){
    stop_=true;std::lock_guard lock(mutex_);if(worker_.joinable())worker_.join();
}
void MediaDiagnostics::request(uint64_t id,uint32_t mode,const std::string& source,
    std::function<bool()> allowed,std::function<void(MediaProbeReport)> reply) {
    std::unique_lock lock(mutex_);const auto now=GetTickCount64();
    if(stop_||busy_||(last_start_&&now-last_start_<30000)||!allowed()){
        lock.unlock();reply({id,ERROR_BUSY,"Diagnostic deferred: desktop active, another probe running, or 30-second cooldown"});return;
    }
    if(worker_.joinable())worker_.join();busy_=true;last_start_=now;
    try {worker_=std::thread([this,id,mode,source,allowed=std::move(allowed),reply=std::move(reply)]{
        MediaProbeReport report;
        try{report=run_media_probe(mode,source,[&]{return stop_||!allowed();});}
        catch(const std::exception& e){report.result=ERROR_GEN_FAILURE;report.text=e.what();}
        report.request=id;
        try{reply(std::move(report));}catch(...){}
        busy_=false;
    });}catch(...){busy_=false;throw;}
}
}
