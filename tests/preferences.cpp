#include "hivedesk/config.h"
#include <windows.h>
#include <cstdio>
#include <stdexcept>

int main(){try{
    auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    require(!hive::valid_hive_hotkey(3,'I')&&!hive::valid_hive_hotkey(7,'I')&&hive::valid_hive_hotkey(3,'H'),"Stats chords not reserved or default Hive blocked");
    wchar_t previous[32768]{};const auto size=GetEnvironmentVariableW(L"LOCALAPPDATA",previous,32768);
    require(size&&size<32768,"Missing isolated preference test environment");
    const auto root=std::filesystem::temp_directory_path()/(L"HiveDesk-preferences-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(root),"Create isolated preference directory");
    struct Cleanup{
        std::filesystem::path root;std::wstring previous;
        ~Cleanup(){SetEnvironmentVariableW(L"LOCALAPPDATA",previous.c_str());std::error_code error;std::filesystem::remove(root/L"HiveDesk"/L"settings.ini",error);std::filesystem::remove(root/L"HiveDesk",error);std::filesystem::remove(root,error);}
    }cleanup{root,previous};
    require(SetEnvironmentVariableW(L"LOCALAPPDATA",root.c_str())!=FALSE,"Isolate preference storage");
    auto config=hive::load_config();require(!config.desktop.relative_mouse,"Relative input unexpectedly enabled by default");
    config.device_order={"offline-device","device-c","device-a"};config.stats_devices={"device-a","offline-device"};config.name=L"Preference test";config.desktop.relative_mouse=true;config.desktop.debug_overlay=true;hive::save_config(config);
    const auto ini=root/L"HiveDesk"/L"settings.ini";
    for(const auto* value:{L"1",L"0"}){
        require(WritePrivateProfileStringW(L"HiveDesk",L"DisplayCadence",value,ini.c_str())!=FALSE,"Prepare retired cadence preference");
        const auto migrated=hive::load_config();wchar_t retired[16]{};
        GetPrivateProfileStringW(L"HiveDesk",L"DisplayCadence",L"missing",retired,16,ini.c_str());
        require(wcscmp(retired,L"missing")==0,"Retired cadence preference survived migration");
        require(migrated.device_order==config.device_order&&migrated.name==config.name&&migrated.desktop.relative_mouse&&migrated.desktop.debug_overlay,"Cadence migration changed unrelated preferences");
    }
    auto loaded=hive::load_config();require(loaded.device_order==config.device_order,"Computer order did not survive settings reload");require(loaded.desktop.relative_mouse&&loaded.desktop.debug_overlay&&loaded.name==config.name,"Relative input preference did not persist with existing settings");
    require(loaded.stats_devices==config.stats_devices,"Per-device stats did not survive restart with debug enabled");
    loaded.desktop.debug_overlay=false;loaded.stats_devices.erase(loaded.stats_devices.begin());hive::save_config(loaded);
    require(hive::load_config().stats_devices==loaded.stats_devices,"Disabling one device's stats changed another device");
    loaded.desktop.relative_mouse=false;hive::save_config(loaded);require(!hive::load_config().desktop.relative_mouse,"Relative input preference did not disable");
    require(WritePrivateProfileStringW(L"HiveDesk",L"RelativeMouse",nullptr,ini.c_str())!=FALSE,"Prepare legacy preferences");
    require(!hive::load_config().desktop.relative_mouse,"Legacy settings did not default to absolute input");
    for(const auto* modifiers:{L"3",L"7"}){
        WritePrivateProfileStringW(L"HiveDesk",L"HotkeyModifiers",modifiers,ini.c_str());
        WritePrivateProfileStringW(L"HiveDesk",L"HotkeyKey",L"73",ini.c_str());
        const auto reserved=hive::load_config();require(reserved.desktop.hotkey_key=='H'&&reserved.desktop.hotkey_modifiers==3,"Existing conflicting shortcut did not fall back safely");
    }
    std::puts("PASS relative mouse preferences and retired cadence migration: default, durable enable/disable, old enabled/disabled cadence removed without changing other preferences; isolated storage, no input or registry changes.");return 0;
}catch(const std::exception& error){std::printf("FAIL preferences: %s\n",error.what());return 1;}}
