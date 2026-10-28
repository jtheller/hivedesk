#include "hivedesk/config.h"
#include <windows.h>
#include <cstdio>
#include <stdexcept>

int main(){try{
    auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    wchar_t previous[32768]{};const auto size=GetEnvironmentVariableW(L"LOCALAPPDATA",previous,32768);
    require(size&&size<32768,"Missing isolated preference test environment");
    const auto root=std::filesystem::temp_directory_path()/(L"HiveDesk-preferences-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(root),"Create isolated preference directory");
    struct Cleanup{
        std::filesystem::path root;std::wstring previous;
        ~Cleanup(){SetEnvironmentVariableW(L"LOCALAPPDATA",previous.c_str());std::error_code error;std::filesystem::remove(root/L"HiveDesk"/L"settings.ini",error);std::filesystem::remove(root/L"HiveDesk",error);std::filesystem::remove(root,error);}
    }cleanup{root,previous};
    require(SetEnvironmentVariableW(L"LOCALAPPDATA",root.c_str())!=FALSE,"Isolate preference storage");
    auto config=hive::load_config();require(!config.desktop.display_cadence,"Cadence experiment enabled by default");require(!config.desktop.relative_mouse,"Relative input unexpectedly enabled by default");
    config.device_order={"offline-device","device-c","device-a"};config.name=L"Preference test";config.desktop.relative_mouse=true;config.desktop.debug_overlay=true;config.desktop.display_cadence=true;hive::save_config(config);
    auto loaded=hive::load_config();require(loaded.desktop.display_cadence,"Cadence preference did not persist");require(loaded.device_order==config.device_order,"Computer order did not survive settings reload");require(loaded.desktop.relative_mouse&&loaded.desktop.debug_overlay&&loaded.name==config.name,"Relative input preference did not persist with existing settings");
    loaded.desktop.relative_mouse=false;loaded.desktop.display_cadence=false;hive::save_config(loaded);require(!hive::load_config().desktop.relative_mouse,"Relative input preference did not disable");
    require(!hive::load_config().desktop.display_cadence,"Cadence preference did not disable");
    const auto ini=root/L"HiveDesk"/L"settings.ini";
    require(WritePrivateProfileStringW(L"HiveDesk",L"RelativeMouse",nullptr,ini.c_str())!=FALSE,"Prepare legacy preferences");
    require(!hive::load_config().desktop.relative_mouse,"Legacy settings did not default to absolute input");
    std::puts("PASS relative mouse preferences: default, durable enable/disable, existing fields and legacy migration; isolated storage, no input or registry changes.");return 0;
}catch(const std::exception& error){std::printf("FAIL preferences: %s\n",error.what());return 1;}}
