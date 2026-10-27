#include "gpu_policy.h"
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace hive::gpu_policy {
namespace {
std::wstring lower(std::wstring text) {
    for (auto& c : text) { if (c == L'/') c = L'\\'; c = static_cast<wchar_t>(std::towlower(c)); }
    return text;
}
std::wstring basename(const std::wstring& text) {
    const auto pos = text.find_last_of(L"\\/");
    return lower(pos == std::wstring::npos ? text : text.substr(pos + 1));
}
bool same_apps(std::vector<std::wstring> a, std::vector<std::wstring> b) {
    for (auto& x : a) x = lower(x);
    for (auto& x : b) x = lower(x);
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    return a == b;
}
bool safe_apps(const Profile& profile, const std::vector<std::wstring>& targets) {
    return !profile.apps.empty() && std::all_of(profile.apps.begin(), profile.apps.end(), [&](const auto& app) {
        return std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
            // Existing bare executable registrations are valid, but a full path
            // belonging to another installation must never be modified.
            return lower(app) == lower(target) ||
                (app.find_first_of(L"\\/") == std::wstring::npos && lower(app) == basename(target));
        });
    });
}
Result conflict(const char* code, const char* message) {
    return {"conflict", code, message, false, false};
}
bool is_original(const std::optional<Profile>& profile, const Entry& entry) {
    return entry.created ? (!profile || !profile->override_value) :
        (profile && profile->override_value == entry.original);
}
bool is_enabled(const std::optional<Profile>& profile) {
    return profile && profile->override_value == 1u;
}
std::optional<Result> validate(Driver& driver, const Journal& journal, const std::vector<std::wstring>& targets) {
    if (!same_apps(journal.targets, targets) || journal.entries.empty() || journal.entries.size() > 2)
        return conflict("journal_invalid", "The saved GPU policy backup does not match this installation.");
    for (const auto& entry : journal.entries) {
        const auto current = driver.find_profile(entry.name);
        if (current && !same_apps(current->apps, entry.apps))
            return conflict("external_change", "An application profile was changed externally; its backup has been retained.");
        if (!current && !entry.created)
            return conflict("external_change", "An original application profile was removed externally; its backup has been retained.");
    }
    for (const auto& target : targets) {
        const auto current = driver.find_app(target);
        if (current && std::none_of(journal.entries.begin(), journal.entries.end(), [&](const auto& e) {
            return lower(e.name) == lower(current->name);
        })) return conflict("external_change", "An executable was moved to another driver profile; no settings were changed.");
    }
    return {};
}
}
std::wstring install_id(const std::vector<std::wstring>& targets) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto& target : targets) {
        for (const auto c : lower(target)) { hash ^= static_cast<std::uint16_t>(c); hash *= 1099511628211ull; }
        hash ^= 0xff; hash *= 1099511628211ull;
    }
    std::wostringstream out; out << std::hex << std::setfill(L'0') << std::setw(16) << hash;
    return out.str();
}
Result run(Driver& driver, Storage& storage, const std::vector<std::wstring>& targets, Action action) {
    if (targets.size() != 2) throw std::runtime_error("Expected exactly two video executable paths.");
    driver.reload();
    auto journal = storage.read();
    const bool recovering_restore = journal && journal->phase == Phase::restoring;
    if (journal) {
        if (const auto error = validate(driver, *journal, targets)) return *error;
        bool all_enabled = true, all_original = true;
        for (const auto& entry : journal->entries) {
            const auto current = driver.find_profile(entry.name);
            all_enabled &= is_enabled(current);
            all_original &= is_original(current, entry);
            if (!is_enabled(current) && !is_original(current, entry))
                return conflict("external_change", "GPU power settings changed externally; automatic restoration was refused.");
        }
        if (action == Action::status) {
            if (all_enabled && journal->phase != Phase::restoring)
                return {"enabled", "enabled", "Maximum performance preference is set for both video executables.", journal->phase == Phase::enabling};
            if (all_original)
                return {"disabled", "restored", "The original GPU policy is active; a recovery backup is retained.", journal->phase == Phase::restoring};
            return conflict("interrupted", "An interrupted GPU policy change can be recovered by switching this option off.");
        }
        if (action == Action::enable) {
            if (all_enabled) {
                const bool interrupted = journal->phase != Phase::applied;
                journal->phase = Phase::applied; storage.write(*journal);
                return {"enabled", "enabled", "Maximum performance preference is already set.", interrupted};
            }
            // Only an interrupted initial apply may be resumed. Do not overwrite
            // an external restoration of a previously fully applied preference.
            if (journal->phase != Phase::enabling)
                return conflict("external_change", "The saved policy no longer matches the driver; switch it off to restore first.");
        }
    } else {
        if (action == Action::restore)
            return {"disabled", "unchanged", "No HiveDesk GPU power override is managed on this installation."};
        Journal fresh; fresh.targets = targets;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto current = driver.find_app(targets[i]);
            if (current) {
                if (!safe_apps(*current, targets))
                    return conflict("shared_profile", "The existing driver profile also contains other applications; it was left unchanged.");
                if (std::none_of(fresh.entries.begin(), fresh.entries.end(), [&](const auto& e) { return lower(e.name) == lower(current->name); }))
                    fresh.entries.push_back({current->name, current->apps, false, current->override_value});
            } else {
                const auto name = L"HiveDesk GPU power " + install_id(targets) + (i == 0 ? L" host" : L" viewer");
                if (driver.find_profile(name))
                    return conflict("profile_collision", "A reserved HiveDesk driver profile already exists without a backup.");
                fresh.entries.push_back({name, {targets[i]}, true, {}});
            }
        }
        if (action == Action::status)
            return {"disabled", "unmanaged", "Using the existing driver power policy."};
        journal = std::move(fresh);
    }
    // Persist the undo record BEFORE anything can reach the driver database.
    journal->phase = action == Action::enable ? Phase::enabling : Phase::restoring;
    storage.write(*journal);
    bool changed = false;
    for (const auto& entry : journal->entries) {
        auto current = driver.find_profile(entry.name);
        if (action == Action::enable) {
            if (!current) { driver.create(entry.name, entry.apps); changed = true; }
            if (!is_enabled(current)) { driver.set(entry.name, 1); changed = true; }
        } else if (current) {
            if (current->override_value != entry.original) {
                driver.set(entry.name, entry.original); changed = true;
            }
            // Delete only a profile created by us that has no remaining settings.
            // External settings survive, along with their application registration.
            current = driver.find_profile(entry.name);
            if (entry.created && current && current->setting_count == 0) {
                driver.erase(entry.name); changed = true;
            }
        }
    }
    if (changed) driver.commit();
    driver.reload();
    if (const auto error = validate(driver, *journal, targets)) return *error;
    for (const auto& entry : journal->entries) {
        const auto current = driver.find_profile(entry.name);
        if ((action == Action::enable && !is_enabled(current)) ||
            (action == Action::restore && !is_original(current, entry)))
            return conflict("verify_failed", "The driver did not retain the requested policy; recovery backup was retained.");
    }
    if (action == Action::enable) {
        journal->phase = Phase::applied; storage.write(*journal);
        return {"enabled", "applied", "Maximum performance preference is set for both video executables.", changed};
    }
    storage.clear();
    return {"disabled", "restored", "The previous GPU power settings have been restored.", changed || recovering_restore};
}
} // namespace hive::gpu_policy
