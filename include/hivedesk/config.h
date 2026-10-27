#pragma once
#include "types.h"
#include <filesystem>
#include <string>
#include <vector>
namespace hive {
struct AppConfig {
    std::string device_id;
    std::wstring name;
    std::string group_key;
    StreamSettings stream;
    DesktopPreferences desktop;
    std::vector<std::string> device_order;
    uint16_t port = 47980;
    bool first_run = true;
};
std::wstring widen(const std::string&);
std::string narrow(const std::wstring&);
std::filesystem::path data_directory();
AppConfig load_config();
void save_config(const AppConfig&);
void set_startup(bool enabled);
void log(const std::string&);
}
