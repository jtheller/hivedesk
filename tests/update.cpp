#include "hivedesk/update.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace update=hive::update;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F function,const char* message){bool rejected=false;try{function();}catch(const std::exception&){rejected=true;}require(rejected,message);}
std::vector<uint8_t> read(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);require(bool(file),"Missing fixture");return {std::istreambuf_iterator<char>(file),{}};}
}
int main(int argc,char** argv){try{
    require(argc==2,"Missing fixture root");const std::filesystem::path root=argv[1];
    update::Version a,b;
    require(update::parse_version("0.1.0-alpha.9",a)&&update::parse_version("0.1.0-alpha.27",b)&&b>a,"Numeric prerelease order failed");
    require(update::parse_version("0.1.0-rc.9",a)&&update::parse_version("0.1.0",b)&&b>a,"Stable release order failed");
    require(update::parse_version("0.2.0-alpha.1",b)&&b>a,"Major/minor upgrade order failed");
    require(update::parse_version("0.1.0-alpha.39",a)&&update::parse_version("0.2.0",b)&&b>a,"Alpha-to-numbered release migration failed");
    a=b;require(update::parse_version("0.2.1",b)&&b>a,"Patch upgrade order failed");
    a=b;require(update::parse_version("0.3.0",b)&&b>a,"Feature upgrade order failed");
    require(update::parse_version("0.2.0",a)&&update::parse_version("0.2.0-rc.99",b)&&b<a,"Stable release accepted a same-version preview downgrade");
    for(const auto* bad:{"","v0.1.0","0.1","0.1.0.2","0.1.0-alpha","0.1.0-alpha.01","0.1.0-alpha.1/evil","0.1.0-alpha.-1","0.1.0-alpha.99999999","01.1.0","0.1.0+dirty"})require(!update::parse_version(bad,a),"Malformed version accepted");
    auto manifest=read(root/L"update.manifest"),signature=read(root/L"update.manifest.sig");
    const auto parsed=update::verify_manifest(manifest,signature);
    require(parsed.version=="0.0.0-alpha.1"&&parsed.size==8,"Signed fixture did not verify");
    auto tampered=manifest;tampered[30]^=1;rejects([&]{update::verify_manifest(tampered,signature);},"Modified manifest verified");
    auto wrong_signature=signature;wrong_signature[0]^=1;rejects([&]{update::verify_manifest(manifest,wrong_signature);},"Wrong signature verified");
    wrong_signature.pop_back();rejects([&]{update::verify_manifest(manifest,wrong_signature);},"Truncated signature verified");
    const auto text=std::string(manifest.begin(),manifest.end());
    for(auto changed:{text+"extra=1\n",text.substr(0,text.size()-1),std::string("HiveDesk-Update-2\n")+text.substr(text.find('\n')+1)})rejects([&]{update::parse_manifest({changed.begin(),changed.end()});},"Malformed manifest accepted");
    auto traversal=text;traversal.replace(traversal.find("file="),5+parsed.file.size(),"file=../../evil.exe");rejects([&]{update::parse_manifest({traversal.begin(),traversal.end()});},"Path traversal accepted");
    const auto temp=std::filesystem::temp_directory_path()/(L"HiveDesk-update-check-"+std::to_wstring(GetCurrentProcessId())+L".bin");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);}}cleanup{temp};
    {std::ofstream file(temp,std::ios::binary);file<<"fixture\n";}
    update::verify_package(parsed,temp);
    {std::ofstream file(temp,std::ios::binary);file<<"tampered";}
    rejects([&]{update::verify_package(parsed,temp);},"Same-length corrupted installer verified");
    {std::ofstream file(temp,std::ios::binary);file<<"short";}
    rejects([&]{update::verify_package(parsed,temp);},"Truncated installer verified");
    require(update::idle_for_update(false,false,false,false),"Discovery-only idle blocked");
    require(update::idle_for_update(false,true,false,false,true),"Prewarm lease blocked idle update");
    require(!update::idle_for_update(true,true,false,false,true),"Warm host allowed an active viewer update");
    require(!update::idle_for_update(false,true,true,false,true),"Warm host allowed a reconnect update");
    require(!update::idle_for_update(true,false,false,false),"Outbound remote could be interrupted");
    require(!update::idle_for_update(false,true,false,false),"Inbound remote could be interrupted");
    require(!update::idle_for_update(false,false,true,false),"Pending reconnect could be interrupted");
    require(!update::idle_for_update(false,false,false,true),"Queued selection could be interrupted");
    require(!update::idle_for_update(true,true,true,true),"Reciprocal session could be interrupted");
    require(!update::signal_check_event(nullptr)&&!update::signal_check_event(INVALID_HANDLE_VALUE),"Invalid update wake handle accepted");
    HANDLE untrusted=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(untrusted!=nullptr,"Event fixture failed");
    if(!update::is_system()){
        require(!update::system_owned(untrusted),"Unprivileged update request trusted");
        require(!update::signal_check_event(untrusted),"Unprivileged update wake event trusted");
        require(WaitForSingleObject(untrusted,0)==WAIT_TIMEOUT,"Rejected update wake modified an untrusted event");
    }else{
        // This is an anonymous fixture, never the installed worker's event.
        require(update::signal_check_event(untrusted)&&update::signal_check_event(untrusted),"Trusted update wake rejected");
        require(WaitForSingleObject(untrusted,0)==WAIT_OBJECT_0&&WaitForSingleObject(untrusted,0)==WAIT_TIMEOUT,"Update requests did not coalesce");
    }
    CloseHandle(untrusted);
    std::cout<<"PASS updater: numeric versions, pinned release signature, tamper/truncation/path rejection, SHA256, idle session policy and manual-check event owner validation\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL update: "<<error.what()<<'\n';return 1;}}
