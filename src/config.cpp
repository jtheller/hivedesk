#include "hivedesk/config.h"
#include "hivedesk/transport.h"
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <sstream>
#include <iomanip>
namespace hive {
std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) return L"[invalid UTF-8]";
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n);
    return result;
}
std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr);
    return result;
}
std::filesystem::path data_directory() {
    wchar_t buffer[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (!n || n >= 32768) throw std::runtime_error("LOCALAPPDATA is unavailable");
    const auto path = std::filesystem::path(buffer) / L"HiveDesk";
    std::filesystem::create_directories(path);
    return path;
}
static std::string hex(const BYTE* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (size_t i = 0; i < size; ++i) { out[i*2] = digits[data[i] >> 4]; out[i*2+1] = digits[data[i] & 15]; }
    return out;
}
static std::wstring read_ini(const std::filesystem::path& path, const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buffer[16384]{};
    GetPrivateProfileStringW(L"HiveDesk", key, fallback, buffer, 16384, path.c_str());
    return buffer;
}
AppConfig load_config() {
    AppConfig config;
    const auto path = data_directory() / L"settings.ini";
    config.first_run = !std::filesystem::exists(path);
    config.device_id = narrow(read_ini(path, L"DeviceId"));
    if (config.device_id.empty()) { BYTE random[16]{}; if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Random generator failed"); config.device_id=hex(random,sizeof(random)); }
    config.name=read_ini(path,L"Name");
    if(config.name.empty()) { wchar_t name[MAX_COMPUTERNAME_LENGTH+1]{}; DWORD size=MAX_COMPUTERNAME_LENGTH+1; GetComputerNameW(name,&size); config.name=name; }
    config.desktop.startup=GetPrivateProfileIntW(L"HiveDesk",L"Startup",1,path.c_str())!=0;
    config.desktop.tray_icon=GetPrivateProfileIntW(L"HiveDesk",L"TrayIcon",1,path.c_str())!=0;
    config.desktop.debug_overlay=GetPrivateProfileIntW(L"HiveDesk",L"DebugOverlay",0,path.c_str())!=0;
    config.desktop.relative_mouse=GetPrivateProfileIntW(L"HiveDesk",L"RelativeMouse",0,path.c_str())!=0;
    std::istringstream order(narrow(read_ini(path,L"DeviceOrder")));std::string id;
    while(order>>id)if(id.size()<=128&&config.device_order.size()<256&&
        std::find(config.device_order.begin(),config.device_order.end(),id)==config.device_order.end())config.device_order.push_back(id);
    config.desktop.hotkey_modifiers=GetPrivateProfileIntW(L"HiveDesk",L"HotkeyModifiers",3,path.c_str());
    config.desktop.hotkey_key=GetPrivateProfileIntW(L"HiveDesk",L"HotkeyKey",'H',path.c_str());
    if(!valid_hive_hotkey(config.desktop.hotkey_modifiers,config.desktop.hotkey_key)){config.desktop.hotkey_modifiers=3;config.desktop.hotkey_key='H';}
    config.stream.fps=GetPrivateProfileIntW(L"HiveDesk",L"FPS",120,path.c_str());
    config.stream.bitrate_mbps=GetPrivateProfileIntW(L"HiveDesk",L"BitrateMbps",80,path.c_str());
    if (!config.stream.bitrate_mbps || config.stream.bitrate_mbps>1000) config.stream.bitrate_mbps=80;
    save_config(config);
    return config;
}
void save_config(const AppConfig& config) {
    const auto path=data_directory()/L"settings.ini";
    auto write=[&](const wchar_t* key,const std::wstring& value){if(!WritePrivateProfileStringW(L"HiveDesk",key,value.c_str(),path.c_str()))throw std::runtime_error("Unable to save settings");};
    write(L"DeviceId",widen(config.device_id)); write(L"Name",config.name);
    std::string order;for(const auto& id:config.device_order){if(!order.empty())order+=' ';order+=id;}write(L"DeviceOrder",widen(order));
    write(L"Startup",config.desktop.startup?L"1":L"0");write(L"TrayIcon",config.desktop.tray_icon?L"1":L"0");
    write(L"DebugOverlay",config.desktop.debug_overlay?L"1":L"0");
    write(L"RelativeMouse",config.desktop.relative_mouse?L"1":L"0");
    write(L"HotkeyModifiers",std::to_wstring(config.desktop.hotkey_modifiers));write(L"HotkeyKey",std::to_wstring(config.desktop.hotkey_key));
    write(L"FPS",std::to_wstring(config.stream.fps)); write(L"BitrateMbps",std::to_wstring(config.stream.bitrate_mbps));
}
void set_startup(bool enabled) {
    HKEY registry{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&registry,nullptr)!=ERROR_SUCCESS)throw std::runtime_error("Could not open login startup preferences");
    LSTATUS result;
    if(enabled){
        wchar_t path[32768]{};const auto size=GetModuleFileNameW(nullptr,path,32768);
        if(!size||size>=32768){RegCloseKey(registry);throw std::runtime_error("Could not locate HiveDesk");}
        const std::wstring command=L"\""+std::wstring(path)+L"\" --background";
        result=RegSetValueExW(registry,L"HiveDesk",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),DWORD((command.size()+1)*sizeof(wchar_t)));
    }else{result=RegDeleteValueW(registry,L"HiveDesk");if(result==ERROR_FILE_NOT_FOUND)result=ERROR_SUCCESS;}
    RegCloseKey(registry);if(result!=ERROR_SUCCESS)throw std::runtime_error("Could not update login startup preferences");
}
void log(const std::string& text) {
    static std::mutex mutex; const std::lock_guard lock(mutex);
    try {
        const auto path=data_directory()/L"log.txt";
        if(std::filesystem::exists(path)&&std::filesystem::file_size(path)>1024*1024) {
            const auto previous=data_directory()/L"log.previous.txt";
            MoveFileExW(path.c_str(),previous.c_str(),MOVEFILE_REPLACE_EXISTING);
        }
        SYSTEMTIME time{}; GetLocalTime(&time);
        std::ofstream file(path,std::ios::app);
        file<<std::setfill('0')<<std::setw(4)<<time.wYear<<'-'<<std::setw(2)<<time.wMonth<<'-'<<std::setw(2)<<time.wDay<<' '<<std::setw(2)<<time.wHour<<':'<<std::setw(2)<<time.wMinute<<':'<<std::setw(2)<<time.wSecond<<' '<<text<<'\n';
    } catch(...) { OutputDebugStringA("HiveDesk log unavailable\n"); }
}
}
