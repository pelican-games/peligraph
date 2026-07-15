#include "../src/core/watch/contentdigest.hpp"
#include "../src/core/watch/filewatcher.hpp"
#include "../src/core/watch/reloadgate.hpp"
#include "../src/core/launchconfig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Pelican::watch {
namespace {

using namespace std::chrono_literals;

struct Sandbox {
    std::filesystem::path root;
    Sandbox() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_wp96_" + std::to_string(GetCurrentProcessId()) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    ~Sandbox() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    REQUIRE(output.good());
}

template <class Predicate> bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 8s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

struct GateFixture {
    mutable std::mutex mutex;
    ReloadGateSnapshot value{true, 10, {}};
    ReloadGateSnapshot snapshot() const { std::scoped_lock lock{mutex}; return value; }
    void set(bool enabled) {
        std::scoped_lock lock{mutex};
        value.enabled = enabled;
        ++value.epoch;
        value.reason = enabled ? "" : "fixture gate";
    }
};

std::vector<ReloadRequest> applyAll(FileWatcher &watcher, bool succeed = true) {
    std::vector<ReloadRequest> requests;
    watcher.applyFrame([&](const ReloadRequest &request) {
        requests.push_back(request);
        return succeed;
    });
    return requests;
}

#ifdef _WIN32
bool createDirectoryJunction(const std::filesystem::path &link, const std::filesystem::path &target) {
    std::wstring command = L"cmd.exe /d /c mklink /J \"" + link.native() + L"\" \"" + target.native() + L"\"";
    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, writable.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return false;
    WaitForSingleObject(process.hProcess, 10000);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}
#endif

} // namespace

TEST_CASE("ContentDigest streams bytes and separates observed live pending and self-write", "[wp96][digest]") {
    Sandbox box;
    const auto path = box.root / "large.bin";
    writeText(path, std::string(180000, 'x'));
    const auto digest = readStableContentDigest(path);
    REQUIRE(digest.status == DigestReadStatus::stable);
    REQUIRE(digest.byte_count == 180000);
    REQUIRE(digest.sha256.size() == 64);

    const AssetKey key{"large.bin", {}};
    ContentDigestState state;
    state.seedLive(key, "old");

    SECTION("editor apply and write consumes exactly once and survives resume") {
        state.registerSelfWrite(key, "new", 7, true);
        REQUIRE(state.observe(key, "new", 7) == ObserveDisposition::consumed_self_write);
        const auto snapshot = state.snapshot(key);
        REQUIRE(snapshot.observed_digest == "new");
        REQUIRE(snapshot.live_digest == "new");
        REQUIRE_FALSE(snapshot.pending_digest);
        REQUIRE(state.observe(key, "new", 8) == ObserveDisposition::unchanged);
    }
    SECTION("write success but runtime failure follows normal reload") {
        state.registerSelfWrite(key, "new", 7, false);
        REQUIRE(state.observe(key, "new", 7) == ObserveDisposition::queue_reload);
        REQUIRE(state.snapshot(key).live_digest == "old");
    }
    SECTION("an external hash invalidates the token") {
        state.registerSelfWrite(key, "new", 7, true);
        REQUIRE(state.observe(key, "external", 7) == ObserveDisposition::queue_reload);
    }
    SECTION("an old epoch token cannot suppress work") {
        state.registerSelfWrite(key, "new", 7, true);
        REQUIRE(state.observe(key, "new", 8) == ObserveDisposition::queue_reload);
    }
}

TEST_CASE("ReloadQueue merges an asset and publishes only at a matching frame epoch", "[wp96][queue]") {
    ReloadQueue queue;
    queue.push({{"a.txt", {}}, ReloadKind::modified, "one", 3});
    queue.push({{"a.txt", {}}, ReloadKind::modified, "two", 3});
    queue.push({{"b.txt", {}}, ReloadKind::removed, std::nullopt, 2});
    REQUIRE(queue.size() == 2);
    const auto frame = queue.takeForFrame(3);
    REQUIRE(frame.size() == 1);
    REQUIRE(frame.front().digest == "two");
    REQUIRE(queue.size() == 0);
}

TEST_CASE("FileWatcher batch commits each digest only with its transaction result",
          "[hr2-s][watcher][batch]") {
    Sandbox box;
    writeText(box.root / "accepted.txt", "v1");
    writeText(box.root / "rejected.txt", "v1");
    GateFixture gate;
    FileWatcherOptions options;
    options.manual_clock = true;
    options.poll_interval = 20ms;
    options.watch_arm_override = [](const WatchStore &, unsigned) { return false; };
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); }, options);
    const auto t0 = FileWatcher::Clock::now();
    watcher.runControlCycleForTesting(t0);

    writeText(box.root / "accepted.txt", "v2");
    writeText(box.root / "rejected.txt", "v2");
    watcher.runControlCycleForTesting(t0 + 25ms);

    std::vector<AssetKey> batch;
    REQUIRE(watcher.applyFrameBatch([&](std::span<const ReloadRequest> requests) {
                std::vector<bool> results;
                for (const auto &request : requests) {
                    batch.push_back(request.key);
                    results.push_back(request.key.path == "accepted.txt");
                }
                return results;
            }) == 2);
    REQUIRE(batch.size() == 2);

    watcher.runControlCycleForTesting(t0 + 50ms);
    const auto retry = applyAll(watcher);
    REQUIRE(retry.size() == 1);
    REQUIRE(retry.front().key.path == "rejected.txt");
    watcher.stop();
}

TEST_CASE("FileWatcher local override changes location without changing AssetKey",
          "[wp98][assetkey][override]") {
    Sandbox box;
    const auto project = box.root / "project";
    const auto override_root = box.root / "override";
    writeText(project / "assets" / "same.txt", "project bytes");
    writeText(override_root / "same.txt", "override bytes");

    GateFixture gate;
    FileWatcherOptions options;
    options.manual_clock = true;
    options.poll_interval = 20ms;
    options.watch_arm_override = [](const WatchStore &, unsigned) { return false; };
    FileWatcher watcher({{"project", project, {}}, {"art", override_root, "assets"}},
                        [&] { return gate.snapshot(); }, options);
    const auto t0 = FileWatcher::Clock::now();
    watcher.runControlCycleForTesting(t0);

    writeText(project / "assets" / "same.txt", "shadowed project edit");
    watcher.runControlCycleForTesting(t0 + 25ms);
    REQUIRE(applyAll(watcher).empty());

    writeText(override_root / "same.txt", "selected override edit");
    watcher.runControlCycleForTesting(t0 + 50ms);
    const auto requests = applyAll(watcher);
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().key == makeAssetKey("project://assets/same.txt"));
    watcher.stop();
}

TEST_CASE("polling fallback uses fake-clock reconcile, gate cancellation, recovery and degraded state",
          "[wp96][polling]") {
    Sandbox box;
    writeText(box.root / "old.txt", "v1");
    GateFixture gate;
    unsigned attempts = 0;
    FileWatcherOptions options;
    options.manual_clock = true;
    options.poll_interval = 20ms;
    options.retry_backoff = 10ms;
    options.failures_before_degraded = 3;
    options.watch_arm_override = [&](const WatchStore &, unsigned) { return ++attempts >= 4; };
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); }, options);
    watcher.start();
    const auto t0 = FileWatcher::Clock::now();
    watcher.runControlCycleForTesting(t0);
    REQUIRE(watcher.status().state == WatcherState::polling);

    writeText(box.root / "old.txt", "v2");
    watcher.runControlCycleForTesting(t0 + 25ms);
    REQUIRE(applyAll(watcher).size() == 1);

    std::filesystem::rename(box.root / "old.txt", box.root / "renamed.txt");
    watcher.runControlCycleForTesting(t0 + 50ms);
    auto renamed = applyAll(watcher);
    REQUIRE(renamed.size() == 2);

    // Recovery is arm -> reconcile -> watching; a change in that scan window
    // is included before polling is stopped.
    std::atomic<bool> changed{false};
    watcher.setBeforeScanHook([&] {
        if (!changed.exchange(true)) writeText(box.root / "renamed.txt", "during recovery");
    });
    watcher.runControlCycleForTesting(t0 + 100ms);
    REQUIRE(watcher.status().state == WatcherState::watching);
    REQUIRE(applyAll(watcher).size() == 1);

    watcher.setBeforeScanHook([&] { gate.set(false); });
    gate.set(true);
    watcher.runControlCycleForTesting(t0 + 120ms);
    watcher.runControlCycleForTesting(t0 + 140ms);
    REQUIRE(watcher.status().state == WatcherState::disabled);
    REQUIRE(watcher.pendingCount() == 0);
    watcher.stop();

    GateFixture degraded_gate;
    FileWatcherOptions degraded_options;
    degraded_options.manual_clock = true;
    degraded_options.retry_backoff = 1ms;
    degraded_options.failures_before_degraded = 2;
    degraded_options.watch_arm_override = [](const WatchStore &, unsigned) { return false; };
    FileWatcher degraded({{"project", box.root}}, [&] { return degraded_gate.snapshot(); }, degraded_options);
    degraded.start();
    degraded.runControlCycleForTesting(t0);
    degraded.runControlCycleForTesting(t0 + 2ms);
    REQUIRE(degraded.status().state == WatcherState::degraded);
    degraded.stop();
}

#ifdef _WIN32
TEST_CASE("Win32 watcher covers real filesystem changes overflow rename Unicode and cancellation",
          "[wp96][watcher][integration]") {
    Sandbox box;
    writeText(box.root / "existing.txt", "initial");
    writeText(box.root / "rename-old.txt", "rename");
    GateFixture gate;
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); });
    watcher.start();
    REQUIRE(waitUntil([&] { return watcher.status().state == WatcherState::watching; }));

    SECTION("normal modify create delete and atomic-save rename") {
        writeText(box.root / "existing.txt", "modified");
        REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 1; }));
        REQUIRE(applyAll(watcher).size() == 1);

        writeText(box.root / "created.txt", "created");
        REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 1; }));
        REQUIRE(applyAll(watcher).size() == 1);
        std::filesystem::remove(box.root / "created.txt");
        REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 1; }));
        const auto removed = applyAll(watcher);
        REQUIRE(removed.size() == 1);
        REQUIRE(removed.front().kind == ReloadKind::removed);

        writeText(box.root / "atomic.tmp", "atomic final");
        REQUIRE(MoveFileExW((box.root / "atomic.tmp").c_str(), (box.root / "existing.txt").c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE);
        REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 1; }));
        const auto atomic = applyAll(watcher);
        REQUIRE(atomic.size() == 1);
        REQUIRE(atomic.front().key.path == "existing.txt");
    }

    SECTION("rename records are not paired and both inventory paths become dirty") {
        std::filesystem::rename(box.root / "rename-old.txt", box.root / "rename-new.txt");
        REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 2; }));
        const auto requests = applyAll(watcher);
        REQUIRE(requests.size() == 2);
        std::set<std::string> paths;
        for (const auto &request : requests) paths.insert(request.key.path);
        REQUIRE(paths == std::set<std::string>{"rename-new.txt", "rename-old.txt"});
    }

    SECTION("Unicode and long relative paths") {
        std::filesystem::path nested = box.root;
        for (int i = 0; i < 5; ++i) nested /= std::string(20, static_cast<char>('a' + i));
        const auto file = nested / L"日本語_asset.txt";
        writeText(file, "unicode");
        REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 1; }));
        const auto requests = applyAll(watcher);
        REQUIRE(requests.size() == 1);
        REQUIRE(requests.front().key.path.find("asset.txt") != std::string::npos);
    }

    SECTION("both overflow signals force deterministic and stress rescans") {
        writeText(box.root / "existing.txt", "overflow-zero");
        watcher.injectOverflowZeroBytes();
        REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 1; }));
        REQUIRE(applyAll(watcher).size() == 1);
        for (int i = 0; i < 64; ++i) writeText(box.root / ("stress-" + std::to_string(i)), "x");
        watcher.injectOverflowNotifyEnumDir();
        REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 64; }));
        REQUIRE(applyAll(watcher).size() == 64);
    }

    const auto start = std::chrono::steady_clock::now();
    watcher.stop(); // pending overlapped ReadDirectoryChangesW -> CancelIoEx -> join
    REQUIRE(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("disabled changes resume through arm-scan barrier exactly once", "[wp96][watcher][integration]") {
    Sandbox box;
    writeText(box.root / "race.txt", "live");
    GateFixture gate;
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); });
    watcher.start();
    REQUIRE(waitUntil([&] { return watcher.status().state == WatcherState::watching; }));
    gate.set(false);
    REQUIRE(waitUntil([&] { return watcher.status().state == WatcherState::disabled; }));
    writeText(box.root / "race.txt", "disabled-change");
    std::atomic<bool> once{false};
    watcher.setBeforeScanHook([&] {
        if (!once.exchange(true)) writeText(box.root / "race.txt", "arm-scan-change");
    });
    gate.set(true);
    REQUIRE(waitUntil([&] { return watcher.status().state == WatcherState::watching; }));
    REQUIRE(waitUntil([&] { return watcher.pendingCount() == 1; }));
    REQUIRE(applyAll(watcher).size() == 1);
    std::this_thread::sleep_for(400ms);
    REQUIRE(watcher.pendingCount() == 0);

    gate.set(false); // resume interrupted by another disable discards candidates
    REQUIRE(waitUntil([&] { return watcher.status().state == WatcherState::disabled; }));
    watcher.stop();
}

TEST_CASE("overlapping stores dedupe physical identity and reparse escape is rejected",
          "[wp96][watcher][integration]") {
    Sandbox box;
    const auto mounted = box.root / "mounted";
    writeText(mounted / "same.txt", "initial");
    const auto outside = box.root.parent_path() / (box.root.filename().string() + "_outside");
    std::filesystem::create_directories(outside);
    writeText(outside / "outside.txt", "outside");
    const auto link = box.root / "escape";
    REQUIRE(createDirectoryJunction(link, outside));

    GateFixture gate;
    FileWatcher watcher({{"project", box.root}, {"mounted", mounted}}, [&] { return gate.snapshot(); });
    watcher.start();
    REQUIRE(waitUntil([&] { return watcher.status().state == WatcherState::watching; }));
    writeText(mounted / "same.txt", "changed");
    REQUIRE(waitUntil([&] { return watcher.pendingCount() >= 1; }));
    REQUIRE(applyAll(watcher).size() == 1);

    writeText(outside / "outside.txt", "must not enter store");
    watcher.injectOverflowZeroBytes();
    std::this_thread::sleep_for(500ms);
    REQUIRE(watcher.pendingCount() == 0);
    watcher.stop();
    std::error_code ec;
    std::filesystem::remove_all(outside, ec);
}
#endif

TEST_CASE("central gate adapts legacy shader flag without a second apply path", "[wp96][gate]") {
    EngineLaunchConfig config;
    config.shader_hot_reload = true;
    ReloadGate gate;
    gate.configureFromLaunch(config);
    REQUIRE(gate.enabled());
    REQUIRE(gate.shaderReloadEnabled());
    const auto enabled_epoch = gate.snapshot().epoch;
    gate.setReason(ReloadGateReason::replay, true);
    REQUIRE_FALSE(gate.enabled());
    REQUIRE(gate.snapshot().epoch > enabled_epoch);
    gate.setReason(ReloadGateReason::replay, false);
    REQUIRE(gate.enabled());
    config.strict_assets = true;
    gate.configureFromLaunch(config);
    REQUIRE_FALSE(gate.shaderReloadEnabled());
}

} // namespace Pelican::watch
