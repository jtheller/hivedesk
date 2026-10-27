// Exercise the production permission retry boundary without showing UAC or
// loading/changing NVIDIA profiles. Real authorization remains a Windows UI.
#define wmain gpu_policy_entry_unused
#include "../src/gpu_policy_main.cpp"
#undef wmain
#include <thread>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int main(){try{
    for(const auto action:{Action::enable,Action::restore}){
        int attempts=0,retries=0;bool in_operation=false;
        const auto result=with_permissions(action,false,[&](Action received)->Result{
            require(received==action,"Permission retry changed requested action");++attempts;
            struct Lifetime{bool& active;Lifetime(bool& value):active(value){active=true;}~Lifetime(){active=false;}} lifetime(in_operation);
            throw NvError(-175,"Denied");
        },[&](Action received){
            require(!in_operation&&received==action,"Elevation started before failed transaction unwound");++retries;
            return Result{action==Action::enable?"enabled":"disabled","applied","Saved",true};
        });
        require(attempts==1&&retries==1&&result.success&&result.restart,"Access denial did not retry exactly once");
    }
    int retries=0;
    auto retry=[&](Action){++retries;return Result{"unavailable","elevation_cancelled","Cancelled",false,false};};
    const auto cancelled=with_permissions(Action::enable,false,[](Action)->Result{throw NvError(-175,"Denied");},retry);
    require(!cancelled.success&&!cancelled.restart&&cancelled.code=="elevation_cancelled"&&retries==1,"Cancellation reported success or restart");
    for(const auto action:{Action::status,Action::enable,Action::restore})for(const bool admin:{false,true})for(const int code:{-175,-1}){
        if(code==-175&&action!=Action::status&&!admin)continue;
        bool threw=false;
        try{with_permissions(action,admin,[&](Action)->Result{throw NvError(code,"Denied");},retry);}
        catch(const NvError& error){threw=error.status==code;}
        require(threw&&retries==1,"Read, elevated denial or unrelated failure requested elevation");
    }
    auto success=with_permissions(Action::enable,false,[](Action){return Result{"enabled","unchanged","Already set",false};},retry);
    require(success.success&&!success.restart&&retries==1,"Successful write unnecessarily requested permission");
    require(!user_sid().empty(),"Cannot resolve same-account permission identity");
    Handle mutex(CreateMutexW(nullptr,FALSE,nullptr));require(mutex.value!=nullptr,"Create permission recovery mutex");
    try{
        require(WaitForSingleObject(mutex.value,0)==WAIT_OBJECT_0,"Acquire permission recovery mutex");
        MutexOwner ownership{mutex.value};throw NvError(-175,"Denied");
    }catch(const NvError&){}
    bool released=false;
    std::thread waiter([&]{if(WaitForSingleObject(mutex.value,1000)==WAIT_OBJECT_0){released=true;ReleaseMutex(mutex.value);}});
    waiter.join();require(released,"Denied save left the policy mutex owned during elevation retry");
    {UserFiles original_user_access;}
    wchar_t executable[]=L"HiveDeskGpuPolicy.exe",argument[]=L"--elevated-enable",wrong_user[]=L"S-1-0-0";
    wchar_t* args[]{executable,argument,wrong_user};
    require(gpu_policy_entry_unused(3,args)==12,"Alternate account elevated command was accepted");
    std::puts("PASS GPU policy permissions: denied-write retry after unwind, exact action, cancellation, no read/recursive/unrelated escalation, same-account guard and original-user file scope. No UAC or driver writes.");return 0;
}catch(const std::exception& error){std::printf("FAIL GPU policy permissions: %s\n",error.what());return 1;}}
