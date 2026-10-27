#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hive::gpu_policy {
struct Profile {
    std::wstring name;
    std::vector<std::wstring> apps;
    std::optional<std::uint32_t> override_value;
    std::uint32_t effective_value{};
    std::uint32_t setting_count{};
};
struct Entry {
    std::wstring name;
    std::vector<std::wstring> apps;
    bool created{};
    std::optional<std::uint32_t> original;
};
enum class Phase { enabling, applied, restoring };
struct Journal {
    std::vector<std::wstring> targets;
    std::vector<Entry> entries;
    Phase phase{Phase::enabling};
};
struct Driver {
    virtual ~Driver() = default;
    virtual void reload() = 0;
    virtual std::optional<Profile> find_app(const std::wstring&) = 0;
    virtual std::optional<Profile> find_profile(const std::wstring&) = 0;
    virtual void create(const std::wstring&, const std::vector<std::wstring>&) = 0;
    virtual void set(const std::wstring&, std::optional<std::uint32_t>) = 0;
    virtual void erase(const std::wstring&) = 0;
    virtual void commit() = 0;
};
struct Storage {
    virtual ~Storage() = default;
    virtual std::optional<Journal> read() = 0;
    virtual void write(const Journal&) = 0; // Must durably replace before returning.
    virtual void clear() = 0;
};
struct Result {
    std::string status, code, message;
    bool restart{};
    bool success{true};
};
enum class Action { status, enable, restore };
Result run(Driver&, Storage&, const std::vector<std::wstring>& targets, Action);
std::wstring install_id(const std::vector<std::wstring>& targets);
} // namespace hive::gpu_policy
