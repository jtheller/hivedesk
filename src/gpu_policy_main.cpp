#include "gpu_policy.h"
#include "nvapi_drs.h"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <sddl.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace hive::gpu_policy;
constexpr NvU32 power_setting = 0x1057eb71;
constexpr int not_found_app = -166, not_found_profile = -163, not_found_setting = -160;
static_assert(sizeof(NVDRS_SETTING) == 12320, "DRS setting ABI changed");
static_assert(sizeof(NVDRS_APPLICATION) == 20492, "DRS application ABI changed");
static_assert(sizeof(NVDRS_PROFILE) == 4116, "DRS profile ABI changed");
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct MutexOwner {
    HANDLE value;
    ~MutexOwner(){ReleaseMutex(value);}
};
void win_check(bool success, const char* what) {
    if (!success) throw std::runtime_error(std::string(what) + " (Windows " + std::to_string(GetLastError()) + ").");
}
struct NvError : std::runtime_error {
    int status;
    NvError(int value,const std::string& text):std::runtime_error(text),status(value){}
};
bool elevated() {
    Handle token;win_check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)!=FALSE,"Read process permissions");
    TOKEN_ELEVATION value{};DWORD bytes{};
    win_check(GetTokenInformation(token.value,TokenElevation,&value,sizeof(value),&bytes)!=FALSE,"Read elevation state");
    return value.TokenIsElevated!=0;
}
std::wstring user_sid() {
    Handle token;win_check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)!=FALSE,"Read user identity");
    DWORD bytes{};GetTokenInformation(token.value,TokenUser,nullptr,0,&bytes);
    std::vector<unsigned char> buffer(bytes);
    win_check(GetTokenInformation(token.value,TokenUser,buffer.data(),bytes,&bytes)!=FALSE,"Read user identity");
    LPWSTR sid{};win_check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid)!=FALSE,"Format user identity");
    std::wstring result(sid);LocalFree(sid);return result;
}
// Driver access may need elevation; user-controlled backup paths must not gain
// administrator filesystem rights. Perform every journal operation with the
// original user's filtered token, including when manually launched elevated.
class UserFiles {
    bool impersonating_=false;
public:
    UserFiles() {
        if(!elevated())return;
        Handle token;win_check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)!=FALSE,"Read user file permissions");
        TOKEN_LINKED_TOKEN linked{};DWORD bytes{};
        win_check(GetTokenInformation(token.value,TokenLinkedToken,&linked,sizeof(linked),&bytes)!=FALSE,"Read original user file permissions");
        Handle original(linked.LinkedToken),impersonation;
        win_check(DuplicateToken(original.value,SecurityImpersonation,&impersonation.value)!=FALSE,"Prepare user file permissions");
        win_check(SetThreadToken(nullptr,impersonation.value)!=FALSE,"Use original user file permissions");
        impersonating_=true;
    }
    ~UserFiles(){if(impersonating_&&!RevertToSelf())TerminateProcess(GetCurrentProcess(),1);}
};
void put(NvAPI_UnicodeString& dest, const std::wstring& source) {
    if (source.size() >= std::size(dest)) throw std::runtime_error("Driver profile text exceeds its supported length.");
    std::fill(std::begin(dest), std::end(dest), NvU16{});
    std::copy(source.begin(), source.end(), dest);
}
std::wstring get(const NvAPI_UnicodeString& text) {
    const auto end = std::find(std::begin(text), std::end(text), 0);
    if (end == std::end(text)) throw std::runtime_error("Invalid driver profile string.");
    return {std::begin(text), end};
}
struct NvApi {
    HMODULE library{};
    using Query = void* (__cdecl*)(unsigned int);
    Query query{};
    bool initialized{};
    NvApi() {
        library = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) throw std::runtime_error("The NVIDIA driver API is unavailable on this computer.");
        query = reinterpret_cast<Query>(GetProcAddress(library, "nvapi_QueryInterface"));
        try { check(call<int (__cdecl*)()>(0x0150e828)(), "Initialize NVIDIA driver API"); initialized = true; }
        catch (...) { FreeLibrary(library); library = nullptr; throw; }
    }
    ~NvApi() {
        if (initialized) { if (auto fn = reinterpret_cast<int (__cdecl*)()>(query(0xd22bdd7e))) fn(); }
        if (library) FreeLibrary(library);
    }
    template<class T> T call(unsigned int id) {
        const auto fn = query ? reinterpret_cast<T>(query(id)) : nullptr;
        if (!fn) throw std::runtime_error("The NVIDIA driver does not expose a required profile API.");
        return fn;
    }
    void check(int result, const char* what) {
        if (!result) return;
        char text[64]{};
        if (query) {
            const auto error = reinterpret_cast<int (__cdecl*)(int, char*)>(query(0x6c2d048c));
            if (error) error(result, text);
        }
        if(result==-175)throw NvError(result,std::string(what)+": NVIDIA denied permission to save application settings (-175).");
        throw NvError(result,std::string(what) + ": " + (text[0]?text:"NVIDIA driver error") + " (" + std::to_string(result) + ").");
    }
};
class NvidiaDriver final : public Driver {
    NvApi api_;
    NvDRSSessionHandle session_{};
    using SessionOp = int (__cdecl*)(NvDRSSessionHandle);
    using ProfileOp = int (__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle);
    using FindProfile = int (__cdecl*)(NvDRSSessionHandle, NvAPI_UnicodeString, NvDRSProfileHandle*);
    NvDRSProfileHandle handle(const std::wstring& name) {
        NvAPI_UnicodeString text{}; put(text, name); NvDRSProfileHandle handle{};
        const auto result = api_.call<FindProfile>(0x7e4a9a0b)(session_, text, &handle);
        if (result == not_found_profile) return nullptr;
        api_.check(result, "Find application profile"); return handle;
    }
    Profile read(NvDRSProfileHandle handle) {
        NVDRS_PROFILE info{}; info.version = NVDRS_PROFILE_VER;
        using Info = int (__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_PROFILE*);
        api_.check(api_.call<Info>(0x61cd6fd6)(session_, handle, &info), "Read application profile");
        Profile profile; profile.name = get(info.profileName); profile.setting_count = info.numOfSettings;
        if (info.numOfApps > 64) throw std::runtime_error("The driver profile is shared with too many applications.");
        using Apps = int (__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NvU32*, NVDRS_APPLICATION*);
        for (NvU32 i = 0; i < info.numOfApps; ++i) {
            NVDRS_APPLICATION app{}; app.version = NVDRS_APPLICATION_VER; NvU32 count = 1;
            api_.check(api_.call<Apps>(0x7fa2173a)(session_, handle, i, &count, &app), "Read application registration");
            if (count != 1 || app.isCommandLine || app.isMetro || app.launcher[0] || app.fileInFolder[0])
                throw std::runtime_error("A conditional driver application profile cannot safely be modified.");
            profile.apps.push_back(get(app.appName));
        }
        NVDRS_SETTING setting{}; setting.version = NVDRS_SETTING_VER;
        using GetSetting = int (__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING*);
        const auto result = api_.call<GetSetting>(0x73bf8338)(session_, handle, power_setting, &setting);
        if (result != not_found_setting) {
            api_.check(result, "Read GPU power preference");
            if (setting.settingType != NVDRS_DWORD_TYPE) throw std::runtime_error("Unsupported GPU power setting type.");
            profile.effective_value = setting.u32CurrentValue;
            if (setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && !setting.isCurrentPredefined)
                profile.override_value = setting.u32CurrentValue;
        }
        return profile;
    }
public:
    NvidiaDriver() {
        using Create = int (__cdecl*)(NvDRSSessionHandle*);
        api_.check(api_.call<Create>(0x0694d52e)(&session_), "Create driver settings session");
    }
    ~NvidiaDriver() override {
        if (session_) {
            try { api_.call<SessionOp>(0xdad9cff8)(session_); } catch (...) {}
        }
    }
    void reload() override { api_.check(api_.call<SessionOp>(0x375dbd6b)(session_), "Load driver settings"); }
    std::optional<Profile> find_profile(const std::wstring& name) override {
        const auto h = handle(name); return h ? std::optional<Profile>(read(h)) : std::nullopt;
    }
    std::optional<Profile> find_app(const std::wstring& path) override {
        NvAPI_UnicodeString name{}; put(name, path); NvDRSProfileHandle profile{};
        NVDRS_APPLICATION app{}; app.version = NVDRS_APPLICATION_VER;
        using Find = int (__cdecl*)(NvDRSSessionHandle, NvAPI_UnicodeString, NvDRSProfileHandle*, NVDRS_APPLICATION*);
        const auto result = api_.call<Find>(0xeee566b2)(session_, name, &profile, &app);
        if (result == not_found_app) return {};
        api_.check(result, "Find video executable profile"); return read(profile);
    }
    void create(const std::wstring& name, const std::vector<std::wstring>& apps) override {
        NVDRS_PROFILE info{}; info.version = NVDRS_PROFILE_VER; put(info.profileName, name);
        NvDRSProfileHandle profile{};
        using Create = int (__cdecl*)(NvDRSSessionHandle, NVDRS_PROFILE*, NvDRSProfileHandle*);
        api_.check(api_.call<Create>(0xcc176068)(session_, &info, &profile), "Create dedicated video profile");
        using App = int (__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION*);
        for (const auto& path : apps) {
            NVDRS_APPLICATION app{}; app.version = NVDRS_APPLICATION_VER;
            auto normalized = path;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            put(app.appName, normalized); put(app.userFriendlyName, L"HiveDesk video");
            api_.check(api_.call<App>(0x4347a9de)(session_, profile, &app), "Register video executable");
        }
    }
    void set(const std::wstring& name, std::optional<std::uint32_t> value) override {
        const auto profile = handle(name);
        if (!profile) throw std::runtime_error("The target driver profile disappeared.");
        if (value) {
            NVDRS_SETTING setting{}; setting.version = NVDRS_SETTING_VER;
            setting.settingId = power_setting; setting.settingType = NVDRS_DWORD_TYPE; setting.u32CurrentValue = *value;
            using Set = int (__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING*);
            api_.check(api_.call<Set>(0x577dd202)(session_, profile, &setting), "Set application GPU power preference");
        } else {
            using Delete = int (__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32);
            api_.check(api_.call<Delete>(0xe4a26362)(session_, profile, power_setting), "Restore inherited GPU power preference");
        }
    }
    void erase(const std::wstring& name) override {
        if (const auto profile = handle(name)) api_.check(api_.call<ProfileOp>(0x17093206)(session_, profile), "Remove unused video profile");
    }
    void commit() override { api_.check(api_.call<SessionOp>(0xfcbc7e14)(session_), "Save driver settings"); }
};

class FileStorage final : public Storage {
    std::filesystem::path path_;
    static void number(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    static void text(std::vector<std::uint8_t>& bytes, const std::wstring& value) {
        if (value.size() > 2047) throw std::runtime_error("GPU backup text is too long.");
        number(bytes, static_cast<std::uint32_t>(value.size()));
        for (const auto c : value) { bytes.push_back(static_cast<std::uint8_t>(c)); bytes.push_back(static_cast<std::uint8_t>(c >> 8)); }
    }
    static void strings(std::vector<std::uint8_t>& bytes, const std::vector<std::wstring>& value) {
        number(bytes, static_cast<std::uint32_t>(value.size())); for (const auto& item : value) text(bytes, item);
    }
    struct Reader {
        const std::vector<std::uint8_t>& bytes; std::size_t offset{};
        std::uint32_t number() {
            if (bytes.size() - offset < 4) throw std::runtime_error("The GPU policy backup is truncated.");
            std::uint32_t result = 0; for (int i = 0; i < 4; ++i) result |= std::uint32_t(bytes[offset++]) << (i * 8); return result;
        }
        std::wstring text() {
            const auto size = number();
            if (size > 2047 || size * 2 > bytes.size() - offset) throw std::runtime_error("The GPU policy backup contains invalid text.");
            std::wstring value; value.reserve(size);
            for (std::uint32_t i = 0; i < size; ++i) { const auto lo = bytes[offset++]; const auto hi = bytes[offset++]; const auto c = wchar_t(lo | (hi << 8)); if (!c) throw std::runtime_error("Invalid backup string."); value.push_back(c); }
            return value;
        }
        std::vector<std::wstring> strings(std::uint32_t maximum) {
            const auto size = number(); if (!size || size > maximum) throw std::runtime_error("Invalid GPU backup item count.");
            std::vector<std::wstring> value; for (std::uint32_t i = 0; i < size; ++i) value.push_back(text()); return value;
        }
    };
public:
    explicit FileStorage(std::filesystem::path path) : path_(std::move(path)) {}
    std::optional<Journal> read() override {
        UserFiles permissions;
        Handle file(CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return {};
            win_check(false, "Read GPU policy backup");
        }
        LARGE_INTEGER size{}; win_check(GetFileSizeEx(file.value, &size), "Read backup size");
        if (size.QuadPart < 16 || size.QuadPart > 1024 * 1024) throw std::runtime_error("Invalid GPU policy backup size.");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart)); DWORD count{};
        win_check(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size(), "Read GPU policy backup");
        Reader reader{bytes};
        if (reader.number() != 0x31504448 || reader.number() != 1) throw std::runtime_error("Unsupported GPU policy backup format.");
        Journal journal; const auto phase = reader.number();
        if (phase > 2) throw std::runtime_error("Invalid GPU policy recovery phase.");
        journal.phase = static_cast<Phase>(phase); journal.targets = reader.strings(2);
        const auto entries = reader.number(); if (!entries || entries > 2) throw std::runtime_error("Invalid GPU policy entry count.");
        for (std::uint32_t i = 0; i < entries; ++i) {
            Entry entry; entry.name = reader.text(); entry.apps = reader.strings(64);
            const auto created = reader.number(), has_original = reader.number(), original = reader.number();
            if (created > 1 || has_original > 1 || (created && has_original)) throw std::runtime_error("Invalid GPU policy original value.");
            entry.created = created != 0; if (has_original) entry.original = original;
            journal.entries.push_back(std::move(entry));
        }
        if (reader.offset != bytes.size()) throw std::runtime_error("Unexpected data in GPU policy backup.");
        return journal;
    }
    void write(const Journal& journal) override {
        UserFiles permissions;
        std::filesystem::create_directories(path_.parent_path());
        std::vector<std::uint8_t> bytes; number(bytes, 0x31504448); number(bytes, 1); number(bytes, static_cast<std::uint32_t>(journal.phase));
        strings(bytes, journal.targets); number(bytes, static_cast<std::uint32_t>(journal.entries.size()));
        for (const auto& entry : journal.entries) {
            text(bytes, entry.name); strings(bytes, entry.apps); number(bytes, entry.created ? 1 : 0);
            number(bytes, entry.original ? 1 : 0); number(bytes, entry.original.value_or(0));
        }
        auto temporary = path_; temporary += L".new";
        {
            Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
            win_check(file.value != INVALID_HANDLE_VALUE, "Create GPU policy backup"); DWORD written{};
            win_check(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), "Write GPU policy backup");
            win_check(FlushFileBuffers(file.value), "Flush GPU policy backup");
        }
        win_check(MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Commit GPU policy backup");
    }
    void clear() override {
        UserFiles permissions;
        if (!DeleteFileW(path_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) win_check(false, "Remove restored GPU policy backup");
    }
};
void print(const Result& result) {
    std::string message = result.message.substr(0, 900);
    for (auto& c : message) if (c < ' ' || c > '~') c = ' ';
    std::printf("HiveDesk-Gpu-Policy-1\nstatus=%s\nrestart=%d\ncode=%s\nmessage=%s\n",
        result.status.c_str(), result.restart ? 1 : 0, result.code.c_str(), message.c_str());
}
Result execute(Action action) {
        wchar_t module[32768]{}; const auto length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
        win_check(length && length < std::size(module), "Locate GPU policy helper");
        const auto directory = std::filesystem::path(module).parent_path();
        std::vector<std::wstring> targets{(directory / L"HiveDeskHost.exe").wstring(), (directory / L"HiveDesk.exe").wstring()};
        for (const auto& target : targets) if (!std::filesystem::is_regular_file(target)) throw std::runtime_error("The sibling HiveDesk video executables are missing.");
        PWSTR appdata{}; const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &appdata);
        if (FAILED(hr)) throw std::runtime_error("The user GPU policy backup folder is unavailable.");
        const std::filesystem::path root(appdata); CoTaskMemFree(appdata);
        const auto id = install_id(targets);
        // Include LocalAppData identity so another logged-in user's journal is
        // never used to interpret this user's policy transaction.
        const auto lock_name = L"Local\\HiveDesk.GpuPolicy." + id + L"." + install_id({root.wstring()});
        Handle mutex(CreateMutexW(nullptr, FALSE, lock_name.c_str())); win_check(mutex.value != nullptr, "Create GPU policy lock");
        const auto wait = WaitForSingleObject(mutex.value, 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) throw std::runtime_error("Another GPU policy change is still running.");
        MutexOwner ownership{mutex.value};
        FileStorage storage(root / L"HiveDesk" / (L"gpu-policy-" + id + L".bin"));
        NvidiaDriver driver;
        const auto result = run(driver, storage, targets, action);
        return result;
}
Result elevate_change(Action action) {
    wchar_t module[32768]{};const auto length=GetModuleFileNameW(nullptr,module,static_cast<DWORD>(std::size(module)));
    win_check(length&&length<std::size(module),"Locate GPU policy helper");
    // No arbitrary commands, executable paths, profile names or journal paths
    // cross this boundary. Alternate administrator accounts are rejected.
    const auto arguments=std::wstring(action==Action::enable?L"--elevated-enable ":L"--elevated-restore ")+user_sid();
    SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
    launch.lpVerb=L"runas";launch.lpFile=module;launch.lpParameters=arguments.c_str();launch.nShow=SW_HIDE;
    if(!ShellExecuteExW(&launch)) {
        if(GetLastError()==ERROR_CANCELLED)return {"unavailable","elevation_cancelled","Administrator permission was cancelled. GPU policy was not confirmed.",false,false};
        win_check(false,"Request administrator permission");
    }
    Handle child(launch.hProcess);
    win_check(child.value&&child.value!=INVALID_HANDLE_VALUE,"Open elevated helper");
    win_check(WaitForSingleObject(child.value,INFINITE)==WAIT_OBJECT_0,"Wait for GPU policy save");
    DWORD code{};win_check(GetExitCodeProcess(child.value,&code)!=FALSE,"Read GPU policy save result");
    if(code==11)return {"unavailable","access_denied","NVIDIA still denied saving with administrator permission (-175). Restarting HiveDesk will not fix this permission error.",false,false};
    if(code==12)return {"unavailable","wrong_user","Approve with the same Windows account; another administrator cannot use your policy backup.",false,false};
    if(code!=0&&code!=10)return {"unavailable","elevated_save_failed","The elevated GPU policy change failed. Its actual state will be checked again.",false,false};
    auto result=execute(Action::status);
    if(!result.success||result.status!=(action==Action::enable?"enabled":"disabled"))
        return {"unavailable","verify_failed","The driver did not confirm the requested GPU policy after administrator approval.",false,false};
    result.restart=result.restart||code==10;return result;
}
Result with_permissions(Action action,bool admin,const std::function<Result(Action)>& operation,
    const std::function<Result(Action)>& retry) {
    try{return operation(action);}
    catch(const NvError& error){
        if(error.status!=-175||action==Action::status||admin)throw;
        return retry(action);
    }
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if(argc!=2&&argc!=3)throw std::runtime_error("Expected --status, --enable or --restore.");
        const std::wstring_view argument(argv[1]);
        const bool child=argc==3&&(argument==L"--elevated-enable"||argument==L"--elevated-restore");
        if((argc==3&&!child)||(child&&(!elevated()||user_sid()!=argv[2])))return 12;
        const auto action=argument==L"--status"?Action::status:
            argument==L"--enable"||(child&&argument==L"--elevated-enable")?Action::enable:
            argument==L"--restore"||(child&&argument==L"--elevated-restore")?Action::restore:
            throw std::runtime_error("Unknown GPU policy command.");
        Result result;
        try {result=with_permissions(action,elevated(),execute,elevate_change);}
        catch(const NvError& error) {
            if(error.status!=-175)throw;
            if(child)return 11;
            print({"unavailable","access_denied",error.what(),false,false});return 1;
        }
        print(result);return !result.success?2:child&&result.restart?10:0;
    } catch (const std::exception& error) {
        print({"unavailable", "operation_failed", error.what(), false, false}); return 1;
    }
}
