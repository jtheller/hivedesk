#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hive::update {
constexpr wchar_t PendingEvent[]=L"Global\\HiveDesk.Update.Pending.v1";
constexpr wchar_t StartupMutex[]=L"Global\\HiveDesk.Update.Startup.v1";
struct Version {
    std::array<uint32_t,5> parts{};
    auto operator<=>(const Version&) const = default;
};
bool parse_version(const std::string&,Version&);
struct Manifest {std::string version,file,sha256;uint64_t size=0;};
Manifest parse_manifest(const std::vector<uint8_t>&);
Manifest verify_manifest(const std::vector<uint8_t>&,const std::vector<uint8_t>&);
std::string hash_file(const std::filesystem::path&);
void verify_package(const Manifest&,const std::filesystem::path&);
std::vector<uint8_t> download(const std::wstring&,size_t limit);
void download_file(const std::wstring&,const std::filesystem::path&,uint64_t expected_size);
std::wstring release_url(const Manifest&);
std::filesystem::path installed_directory();
std::filesystem::path module_directory();
std::string installed_version(const std::filesystem::path&);
bool is_installed_agent();
bool pending();
bool is_system();
bool system_owned(HANDLE);
HANDLE create_system_event(const wchar_t*);
HANDLE create_system_mutex(const wchar_t*,bool user_access=false);
std::filesystem::path private_cache();
void log(const std::string&);
void launch_worker(HANDLE& process); // SYSTEM service only, copies runner out of install tree.
// A new agent cannot start while the updater holds the install barrier.
class StartupGuard {
    HANDLE handle_=nullptr;
    bool allowed_=true,owned_=false;
public:
    StartupGuard();~StartupGuard();
    bool allowed()const{return allowed_;}
    void release();
};
// Called under App's host/admission locks, after command work has completed.
constexpr bool idle_for_update(bool viewing,bool hosting,bool reconnecting,bool queued) {
    return !viewing&&!hosting&&!reconnecting&&!queued;
}
}
