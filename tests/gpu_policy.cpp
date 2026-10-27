#include "gpu_policy.h"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>

using namespace hive::gpu_policy;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
const std::vector<std::wstring> targets{L"C:\\HiveDesk\\HiveDeskHost.exe", L"C:\\HiveDesk\\HiveDesk.exe"};
struct MemoryStorage final : Storage {
    std::optional<Journal> value;
    int writes{}, fail_write{};
    bool fail_clear{};
    std::optional<Journal> read() override { return value; }
    void write(const Journal& journal) override {
        if (++writes == fail_write) throw std::runtime_error("Simulated journal write failure");
        value = journal;
    }
    void clear() override { if (fail_clear) throw std::runtime_error("Simulated journal cleanup failure"); value.reset(); }
};
struct FakeDriver final : Driver {
    std::map<std::wstring, Profile> disk, working;
    int commits{}, sets{}, creates{};
    bool fail_commit{}, fail_after_commit{};
    std::function<void()> before_commit;
    void reload() override { working = disk; }
    std::optional<Profile> find_app(const std::wstring& app) override {
        for (const auto& [name, profile] : working) {
            for (const auto& registered : profile.apps) {
                if (registered == app || registered == app.substr(app.find_last_of(L'\\') + 1)) return profile;
            }
        }
        return {};
    }
    std::optional<Profile> find_profile(const std::wstring& name) override {
        const auto it = working.find(name); return it == working.end() ? std::nullopt : std::optional<Profile>(it->second);
    }
    void create(const std::wstring& name, const std::vector<std::wstring>& apps) override {
        require(!working.contains(name), "Creating existing profile"); ++creates;
        working.emplace(name, Profile{name, apps});
    }
    void set(const std::wstring& name, std::optional<std::uint32_t> value) override {
        ++sets; auto& profile = working.at(name);
        if (profile.override_value && !value) --profile.setting_count;
        if (!profile.override_value && value) ++profile.setting_count;
        profile.override_value = value; profile.effective_value = value.value_or(5);
    }
    void erase(const std::wstring& name) override { working.erase(name); }
    void commit() override {
        if (before_commit) before_commit();
        if (fail_commit) throw std::runtime_error("Simulated driver save failure");
        ++commits; disk = working;
        if (fail_after_commit) throw std::runtime_error("Simulated crash after driver commit");
    }
};
template<class Fn> void throws(Fn fn, const char* message) { bool threw{}; try { fn(); } catch (...) { threw = true; } require(threw, message); }
void new_profiles_roundtrip() {
    FakeDriver driver; MemoryStorage store;
    require(run(driver, store, targets, Action::status).status == "disabled", "Fresh status is disabled");
    require(!store.value && !driver.commits, "Status is read-only");
    driver.before_commit = [&] { require(store.value.has_value(), "Undo record must exist before committing driver writes"); };
    const auto enabled = run(driver, store, targets, Action::enable);
    require(enabled.status == "enabled" && enabled.restart && driver.disk.size() == 2, "Create and enable both profiles");
    require(store.value->phase == Phase::applied, "Applied journal state");
    require(!run(driver, store, targets, Action::enable).restart && driver.commits == 1, "Repeated enable must not change driver");
    require(run(driver, store, targets, Action::restore).restart, "Restore needs restart");
    require(driver.disk.empty() && !store.value, "Remove only dedicated empty profiles and clear backup");
    require(!run(driver, store, targets, Action::restore).restart, "Repeated restore is harmless");
}
void explicit_and_inherited_roundtrip() {
    FakeDriver driver; MemoryStorage store;
    driver.disk.emplace(L"host", Profile{L"host", {targets[0]}, 5, 5, 3});
    driver.disk.emplace(L"viewer", Profile{L"viewer", {targets[1]}, {}, 0, 2});
    run(driver, store, targets, Action::enable); run(driver, store, targets, Action::restore);
    require(driver.disk.at(L"host").override_value == 5u, "Restore exact explicit policy");
    require(!driver.disk.at(L"viewer").override_value, "Restore inheritance by deleting override");
    require(driver.disk.at(L"host").setting_count == 3 && driver.disk.at(L"viewer").setting_count == 2, "Preserve unrelated settings");
}
void shared_and_external_profiles() {
    FakeDriver driver; MemoryStorage store;
    driver.disk.emplace(L"shared", Profile{L"shared", {targets[0], L"game.exe"}, {}, 0, 0});
    require(run(driver, store, targets, Action::enable).code == "shared_profile", "Unrelated shared applications block policy");
    require(!store.value && driver.commits == 0, "Rejected enable is entirely read-only");
    driver.disk.clear(); driver.disk.emplace(L"both", Profile{L"both", {targets[0], targets[1]}, 0, 0, 1});
    run(driver, store, targets, Action::enable);
    require(store.value->entries.size() == 1, "Shared HiveDesk-only profile changes once");
    driver.disk.at(L"both").override_value = 2;
    require(run(driver, store, targets, Action::restore).code == "external_change", "Never overwrite newer external value");
    require(driver.disk.at(L"both").override_value == 2u && store.value, "Conflict retains external value and original backup");
    driver.disk.at(L"both").override_value = 0;
    require(run(driver, store, targets, Action::enable).code == "external_change", "External restoration after successful apply cannot be silently overwritten");
    require(run(driver, store, targets, Action::restore).status == "disabled", "Already-original can safely finish restore");
}
void new_profile_external_edits() {
    FakeDriver driver; MemoryStorage store; run(driver, store, targets, Action::enable);
    auto& first = driver.disk.begin()->second; const auto first_name = first.name;
    ++first.setting_count; // Another preference added externally.
    run(driver, store, targets, Action::restore);
    require(driver.disk.size() == 1 && driver.disk.at(first_name).setting_count == 1, "New profile with external settings must be retained");
    require(!driver.disk.at(first_name).override_value, "Our power override removed without deleting unrelated preference");
    run(driver, store, targets, Action::enable);
    driver.disk.begin()->second.apps.push_back(L"another.exe");
    require(run(driver, store, targets, Action::restore).code == "external_change", "New unrelated app blocks restoration of its now-shared profile");
}
void untrusted_backup_scope() {
    FakeDriver driver;MemoryStorage store;
    run(driver,store,targets,Action::enable);
    const auto commits=driver.commits;
    auto& entry=store.value->entries.front();
    entry.apps.push_back(L"unrelated.exe");
    driver.disk.at(entry.name).apps=entry.apps;
    require(run(driver,store,targets,Action::restore).code=="journal_invalid","A forged backup expanded the elevated write scope");
    require(driver.commits==commits,"Invalid backup changed driver settings");
}
void interrupted_transactions() {
    FakeDriver driver; MemoryStorage store; store.fail_write = 1;
    throws([&] { run(driver, store, targets, Action::enable); }, "Precommit journal failure must propagate");
    require(driver.commits == 0 && driver.creates == 0, "Journal failure must precede all driver mutations");
    store.fail_write = 0; driver.fail_commit = true;
    throws([&] { run(driver, store, targets, Action::enable); }, "Driver failure must propagate");
    require(store.value && store.value->phase == Phase::enabling && driver.disk.empty(), "Failed driver save retains original recovery record");
    driver.fail_commit = false;
    // Emulate a partial underlying driver database write at the crash boundary.
    const auto entry = store.value->entries.front();
    driver.disk.emplace(entry.name, Profile{entry.name, entry.apps, 1, 1, 1});
    require(run(driver, store, targets, Action::status).code == "interrupted", "Partial driver write reported as recoverable interruption");
    require(run(driver, store, targets, Action::restore).status == "disabled" && driver.disk.empty(), "Partial enable restores cleanly");
    driver.fail_after_commit = true;
    throws([&] { run(driver, store, targets, Action::enable); }, "Postcommit crash simulated");
    driver.fail_after_commit = false;
    const auto pending = run(driver, store, targets, Action::status);
    require(pending.status == "enabled" && pending.restart, "Applied driver state after crash is read back with restart pending");
    require(run(driver, store, targets, Action::enable).restart, "Repeated enable completes interrupted journal and requests restart");
    store.fail_clear = true;
    throws([&] { run(driver, store, targets, Action::restore); }, "Journal cleanup failure simulated");
    require(driver.disk.empty() && store.value->phase == Phase::restoring, "Original state committed before backup cleanup");
    const auto restore_pending = run(driver, store, targets, Action::status);
    require(restore_pending.status == "disabled" && restore_pending.restart, "Interrupted restore reports pending process restart");
    store.fail_clear = false;
    const auto recovered = run(driver, store, targets, Action::restore);
    require(recovered.status == "disabled" && recovered.restart && !store.value, "Interrupted restore completes idempotently with restart");
}
void after_commit_journal_failure() {
    FakeDriver driver; MemoryStorage store; store.fail_write = 2;
    throws([&] { run(driver, store, targets, Action::enable); }, "Postcommit journal failure must propagate");
    require(store.value && store.value->phase == Phase::enabling && driver.disk.size() == 2, "Durable original backup survives failed applied marker");
    const auto result = run(driver, store, targets, Action::status);
    require(result.status == "enabled" && result.restart, "Status reports verified driver state and pending restart");
    run(driver, store, targets, Action::restore);
    require(driver.disk.empty() && !store.value, "Postcommit journal failure can always restore");
}
void existing_max_and_scope() {
    FakeDriver driver; MemoryStorage store;
    driver.disk.emplace(L"both", Profile{L"both", {targets[0], targets[1]}, 1, 1, 1});
    require(run(driver, store, targets, Action::status).status == "disabled", "Unmanaged existing maximum is not our checkbox ownership");
    require(!run(driver, store, targets, Action::enable).restart, "Already maximum creates only backup without driver write");
    run(driver, store, targets, Action::restore);
    require(driver.disk.at(L"both").override_value == 1u, "Restore must retain pre-existing explicit maximum");
    run(driver, store, targets, Action::enable);
    auto other = targets; other[0] = L"C:\\Other\\HiveDeskHost.exe";
    require(run(driver, store, other, Action::restore).code == "journal_invalid", "Backup must be bound to the exact executable paths");
}
}
int main() {
    try {
        new_profiles_roundtrip(); explicit_and_inherited_roundtrip(); shared_and_external_profiles();
        new_profile_external_edits(); untrusted_backup_scope(); interrupted_transactions(); after_commit_journal_failure(); existing_max_and_scope();
        std::puts("PASS: GPU policy transaction, restoration, crash recovery and external-change isolation"); return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
