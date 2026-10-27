// Explicit, opt-in driver integration probe. Never registered with CTest and
// never packaged. Only unique, nonexistent temporary executable names are used.
#define wmain gpu_policy_production_entry_unused
#include "../src/gpu_policy_main.cpp"
#undef wmain

namespace {
struct ProbeStorage final : Storage {
    std::optional<Journal> journal;
    std::optional<Journal> read() override { return journal; }
    void write(const Journal& value) override { journal = value; }
    void clear() override { journal.reset(); }
};
void require_probe(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "--isolated-driver-probe") {
        std::fputs("Explicit --isolated-driver-probe is required. No settings were touched.\n", stderr); return 2;
    }
    std::vector<std::wstring> names;
    std::unique_ptr<NvidiaDriver> driver;
    bool cleanup_allowed{};
    try {
        GUID unique{}; if (FAILED(CoCreateGuid(&unique))) throw std::runtime_error("Create unique test identity failed.");
        wchar_t text[64]{}; StringFromGUID2(unique, text, static_cast<int>(std::size(text)));
        wchar_t temporary[MAX_PATH]{};
        win_check(GetTempPathW(MAX_PATH, temporary) != 0, "Find probe temporary directory");
        const auto base = std::filesystem::path(temporary) / (L"GpuPolicyProbe-" + std::wstring(text));
        const std::vector<std::wstring> probe_targets{(base / L"uninstalled-host-probe.exe").wstring(), (base / L"uninstalled-viewer-probe.exe").wstring()};
        const auto id = install_id(probe_targets);
        names = {L"HiveDesk GPU power " + id + L" host", L"HiveDesk GPU power " + id + L" viewer"};
        driver = std::make_unique<NvidiaDriver>(); driver->reload();
        for (const auto& name : names) require_probe(!driver->find_profile(name), "Unexpected unique profile collision");
        for (const auto& target : probe_targets) require_probe(!driver->find_app(target), "Unexpected executable mapping collision");
        cleanup_allowed = true;
        ProbeStorage store;
        const auto enabled = run(*driver, store, probe_targets, Action::enable);
        require_probe(enabled.success && enabled.status == "enabled", "Isolated enable failed");
        for (const auto& name : names) {
            const auto profile = driver->find_profile(name);
            require_probe(profile && profile->override_value == 1u, "Driver failed to retain explicit maximum");
        }
        const auto restored = run(*driver, store, probe_targets, Action::restore);
        require_probe(restored.success && restored.status == "disabled", "Isolated restore failed");
        for (const auto& name : names) require_probe(!driver->find_profile(name), "New test profile was not cleaned up");
        // Seed two pre-existing test profiles to validate preserving an explicit
        // adaptive setting and removing the override to restore inheritance.
        driver->create(names[0], {probe_targets[0]}); driver->set(names[0], 0u);
        driver->create(names[1], {probe_targets[1]}); driver->commit();
        require_probe(run(*driver, store, probe_targets, Action::enable).success, "Existing-profile enable failed");
        require_probe(run(*driver, store, probe_targets, Action::restore).success, "Existing-profile restore failed");
        require_probe(driver->find_profile(names[0])->override_value == 0u, "Explicit adaptive original was not restored");
        require_probe(!driver->find_profile(names[1])->override_value, "Inherited original did not resume inheritance");
        for (const auto& name : names) driver->erase(name);
        driver->commit(); driver->reload();
        for (const auto& name : names) require_probe(!driver->find_profile(name), "Final probe profile cleanup failed");
        std::puts("PASS: real NVIDIA DRS isolated full-path create, maximum preference, explicit/inherited restore and profile cleanup");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        if (driver && cleanup_allowed) {
            try {
                driver->reload();
                for (const auto& name : names) if (driver->find_profile(name)) driver->erase(name);
                driver->commit(); driver->reload();
                for (const auto& name : names) require_probe(!driver->find_profile(name), "Recovery cleanup verification failed");
                std::fputs("Unique probe profiles cleaned up after failure.\n", stderr);
            } catch (const std::exception& cleanup) { std::fprintf(stderr, "PROBE CLEANUP FAILED: %s\n", cleanup.what()); }
        }
        return 1;
    }
}
