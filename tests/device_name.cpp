#include "hivedesk/config.h"
#include "hivedesk/device_name.h"
#include <fstream>
#include <stdexcept>
#include <cstdio>
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
int main(){
    wchar_t temp[MAX_PATH]{},prior[32768]{};GetTempPathW(MAX_PATH,temp);GetEnvironmentVariableW(L"LOCALAPPDATA",prior,32768);
    const auto root=std::filesystem::path(temp)/(L"HiveDesk-name-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    struct Cleanup{std::filesystem::path path;std::wstring prior;~Cleanup(){SetEnvironmentVariableW(L"LOCALAPPDATA",prior.c_str());std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{root,prior};
    try{
        require(root.is_absolute()&&std::filesystem::equivalent(root.parent_path(),std::filesystem::path(temp)),"test profile escaped temporary directory");
        std::filesystem::create_directories(root/L"HiveDesk");SetEnvironmentVariableW(L"LOCALAPPDATA",root.c_str());
        const auto path=root/L"HiveDesk"/L"settings.ini";
        {std::ofstream old(path,std::ios::binary);old<<"[HiveDesk]\r\nName=Jateruy-VI\r\n[Preserved]\r\nMarker=keep-me\r\n";}
        auto config=hive::load_config();require(config.name==L"Jateruy-VI","saved case changed during migration");
        {std::ifstream raw(path,std::ios::binary);unsigned char bom[2]{};raw.read(reinterpret_cast<char*>(bom),2);require(bom[0]==0xff&&bom[1]==0xfe,"profile remained ANSI");}
        wchar_t value[128]{};GetPrivateProfileStringW(L"Preserved",L"Marker",L"",value,128,path.c_str());require(std::wstring(value)==L"keep-me","migration lost unknown section");
        for(const auto* name:{L"Jateruy-VI",L"JT-WORKS-PRINT-SHOP",L"工作室・東京・München & Δοκιμή",L"Station-\U0001f5a5"}){
            config.name=name;hive::save_config(config);const auto restored=hive::load_config();
            require(restored.name==name&&hive::widen(hive::narrow(restored.name))==name,"Unicode/case roundtrip failed");
        }
        require(!hive::default_device_name().empty(),"default physical host name unavailable");
        std::puts("PASS device names: original case, Unicode/UTF-8 transport roundtrip, ANSI to UTF-16 migration, preserved unknown settings");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL device names: %s\n",e.what());return 1;}
}
