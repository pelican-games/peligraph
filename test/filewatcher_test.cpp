#include "../src/core/watch/contentdigest.hpp"
#include "../src/core/watch/filewatcher.hpp"
#include "../src/core/watch/reloadgate.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/project/assetsmanifest.hpp"
#include "../src/project/sha256stream.hpp"

#include <catch2/catch_test_macros.hpp>
#include <picosha2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>
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

// FW-1 fixtures. Files whose mtime is well in the past, as a project's assets
// are when the watcher starts (a file written in the last 2 s is hashed: its
// mtime could still change within one timestamp tick).
void setAge(const std::filesystem::path &path, std::chrono::minutes age) {
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - age);
}

std::vector<std::filesystem::path> agedFiles(const std::filesystem::path &root, int count) {
    std::vector<std::filesystem::path> files;
    for (int index = 0; index < count; ++index) {
        const auto path = root / ("asset-" + std::to_string(index) + ".txt");
        writeText(path, "v1-" + std::to_string(index));
        setAge(path, 60min);
        files.push_back(path);
    }
    return files;
}

// A save whose mtime is set back again, so a second pass over the same scan
// (a late notification) finds it settled and does not read it twice.
void rewriteAged(const std::filesystem::path &path, std::string_view text, std::chrono::minutes age) {
    writeText(path, text);
    setAge(path, age);
}

FileWatcherOptions watchingOptions() {
    FileWatcherOptions options;
    options.manual_clock = true; // native watch armed; the debounce is passed by the fake clock
    return options;
}

// Lets the native notifications of the writes so far arrive before a cycle.
void settleNotifications() { std::this_thread::sleep_for(300ms); }

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

TEST_CASE("Unreadable watcher roots retain live inventory instead of emitting removals",
          "[wp96][watcher][unreadable]") {
    Sandbox box;
    const auto watched = box.root / "watched";
    const auto unavailable = box.root / "temporarily-unavailable";
    writeText(watched / "asset.txt", "v1");
    GateFixture gate;
    FileWatcherOptions options;
    options.manual_clock = true;
    options.poll_interval = 20ms;
    options.watch_arm_override =
        [](const WatchStore &, unsigned) { return false; };
    FileWatcher watcher({{"project", watched}},
                        [&] { return gate.snapshot(); }, options);
    const auto t0 = FileWatcher::Clock::now();
    watcher.runControlCycleForTesting(t0);

    std::filesystem::rename(watched, unavailable);
    watcher.runControlCycleForTesting(t0 + 25ms);
    REQUIRE(applyAll(watcher).empty());
    REQUIRE(watcher.status().error.find("retaining prior inventory") !=
            std::string::npos);

    std::filesystem::rename(unavailable, watched);
    writeText(watched / "asset.txt", "v2");
    watcher.runControlCycleForTesting(t0 + 50ms);
    const auto requests = applyAll(watcher);
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().kind == ReloadKind::modified);
    REQUIRE(requests.front().key.path == "asset.txt");
    watcher.stop();
}

TEST_CASE("FileWatcher worker contains scan exceptions and keeps running",
          "[wp96][watcher][thread-boundary]") {
    Sandbox box;
    writeText(box.root / "asset.txt", "v1");
    GateFixture gate;
    FileWatcherOptions options;
    options.retry_backoff = 1ms;
    options.poll_interval = 1ms;
    options.watch_arm_override =
        [](const WatchStore &, unsigned) { return false; };
    FileWatcher watcher({{"project", box.root}},
                        [&] { return gate.snapshot(); }, options);
    std::atomic<unsigned> scans{0};
    watcher.setBeforeScanHook([&] {
        if (++scans == 1) {
            throw std::runtime_error{"injected scan failure"};
        }
    });
    watcher.start();
    REQUIRE(waitUntil([&] { return scans.load() >= 2; }));
    watcher.stop();
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

TEST_CASE("FW-1 an unknown live digest equals nothing, still reports removal and accepts a self-write",
          "[fw1][digest]") {
    const AssetKey key{"asset.txt", {}};
    ContentDigestState state;

    SECTION("the first observation queues even the start-up bytes, the next equal one does not") {
        state.seedLiveUnknown(key);
        REQUIRE(state.snapshot(key).live_unknown);
        REQUIRE(state.observe(key, "same", 1) == ObserveDisposition::queue_reload);
        state.commitSucceeded(key, std::string{"same"});
        REQUIRE_FALSE(state.snapshot(key).live_unknown);
        REQUIRE(state.observe(key, "same", 1) == ObserveDisposition::unchanged);
    }
    SECTION("a failed first reload is queued again by the next observation") {
        state.seedLiveUnknown(key);
        REQUIRE(state.observe(key, "same", 1) == ObserveDisposition::queue_reload);
        state.commitFailed(key);
        REQUIRE(state.observe(key, "same", 1) == ObserveDisposition::queue_reload);
    }
    SECTION("a removal of a never-hashed source is reported") {
        state.seedLiveUnknown(key);
        REQUIRE(state.observeMissing(key));
    }
    SECTION("an applied self-write is consumed without a known live digest") {
        state.seedLiveUnknown(key);
        state.registerSelfWrite(key, "new", 4, true);
        REQUIRE(state.observe(key, "new", 4) == ObserveDisposition::consumed_self_write);
    }
    SECTION("a self-write whose runtime apply failed follows the normal reload") {
        state.seedLiveUnknown(key);
        state.registerSelfWrite(key, "new", 4, false);
        REQUIRE(state.observe(key, "new", 4) == ObserveDisposition::queue_reload);
    }
}

TEST_CASE("FW-1 start-up hashes nothing and one rewritten file of N is hashed once and queued once",
          "[fw1][watcher]") {
    Sandbox box;
    const auto files = agedFiles(box.root, 8);
    GateFixture gate;
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); }, watchingOptions());
    watcher.runControlCycleForTesting(FileWatcher::Clock::now());
    REQUIRE(watcher.status().state == WatcherState::watching);
    REQUIRE(watcher.status().scans >= 1);
    REQUIRE(watcher.status().hashed_files == 0);
    REQUIRE(watcher.status().hashed_bytes == 0);

    rewriteAged(files[3], "v2-3 changed", 30min);
    watcher.notifyPathForTesting(files[3]);
    settleNotifications();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    REQUIRE(watcher.status().hashed_files == 1);
    REQUIRE(watcher.status().hashed_bytes == std::string_view{"v2-3 changed"}.size());
    REQUIRE(watcher.pendingCount() == 1);
    const auto requests = applyAll(watcher);
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().key.path == "asset-3.txt");
    REQUIRE(requests.front().kind == ReloadKind::modified);
    watcher.stop();
}

TEST_CASE("FW-1 a same-bytes resave reloads once while the live bytes are unknown, then is ignored",
          "[fw1][watcher]") {
    Sandbox box;
    const auto files = agedFiles(box.root, 4);
    GateFixture gate;
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); }, watchingOptions());
    watcher.runControlCycleForTesting(FileWatcher::Clock::now());
    REQUIRE(watcher.status().hashed_files == 0);

    // First save of a file the watcher never hashed: by contract it reloads
    // once, since the bytes the engine loaded at start-up are not known.
    rewriteAged(files[0], "v1-0", 30min);
    settleNotifications();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    REQUIRE(watcher.status().hashed_files == 1);
    REQUIRE(watcher.pendingCount() == 1);
    REQUIRE(applyAll(watcher).size() == 1);

    // From then on the live digest is known: only the mtime moved.
    rewriteAged(files[0], "v1-0", 20min);
    settleNotifications();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    REQUIRE(watcher.status().hashed_files == 2);
    REQUIRE(watcher.pendingCount() == 0);
    watcher.stop();
}

TEST_CASE("FW-1 polling ticks without a change hash nothing", "[fw1][watcher][polling]") {
    Sandbox box;
    agedFiles(box.root, 6);
    GateFixture gate;
    FileWatcherOptions options;
    options.manual_clock = true;
    options.poll_interval = 20ms;
    options.watch_arm_override = [](const WatchStore &, unsigned) { return false; };
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); }, options);
    const auto t0 = FileWatcher::Clock::now();
    watcher.runControlCycleForTesting(t0);
    REQUIRE(watcher.status().state == WatcherState::polling);
    for (int tick = 1; tick <= 3; ++tick) {
        watcher.runControlCycleForTesting(t0 + tick * 25ms);
    }
    REQUIRE(watcher.status().scans >= 4);
    REQUIRE(watcher.status().hashed_files == 0);
    REQUIRE(watcher.pendingCount() == 0);
    watcher.stop();
}

TEST_CASE("FW-1 an overflow rescan hashes only the changed file", "[fw1][watcher][overflow]") {
    Sandbox box;
    const auto files = agedFiles(box.root, 8);
    GateFixture gate;
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); }, watchingOptions());
    watcher.runControlCycleForTesting(FileWatcher::Clock::now());
    REQUIRE(watcher.status().state == WatcherState::watching);

    rewriteAged(files[5], "v2-5 after overflow", 30min);
    settleNotifications();
    watcher.injectOverflowZeroBytes();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    REQUIRE(watcher.status().hashed_files == 1);
    REQUIRE(watcher.pendingCount() == 1);
    REQUIRE(applyAll(watcher).front().key.path == "asset-5.txt");

    watcher.injectOverflowNotifyEnumDir();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    REQUIRE(watcher.status().hashed_files == 1);
    REQUIRE(watcher.pendingCount() == 0);
    watcher.stop();
}

TEST_CASE("FW-1 the project's .pelican directory is neither scanned nor a reason to scan",
          "[fw1][watcher][metadata]") {
    Sandbox box;
    const auto files = agedFiles(box.root, 2);
    const auto cache = box.root / ".pelican" / "shader_cache" / "unit.spv";
    writeText(cache, "spirv v1");
    setAge(cache, 60min);
    GateFixture gate;
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); }, watchingOptions());
    watcher.runControlCycleForTesting(FileWatcher::Clock::now());
    REQUIRE(watcher.status().state == WatcherState::watching);
    const auto seeded = watcher.status();

    // The engine writing its caches: the notifications are dropped.
    writeText(cache, "spirv v2 longer");
    writeText(box.root / ".pelican" / "assets-hash-cache.json", "{}");
    settleNotifications();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    REQUIRE(watcher.status().scans == seeded.scans);
    REQUIRE(watcher.pendingCount() == 0);

    // A full rescan does not enter .pelican either.
    watcher.injectOverflowZeroBytes();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    REQUIRE(watcher.status().scans > seeded.scans);
    REQUIRE(watcher.status().hashed_files == 0);
    REQUIRE(watcher.pendingCount() == 0);

    // A project file next to it is still watched.
    rewriteAged(files[1], "v2-1 project edit", 30min);
    settleNotifications();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    REQUIRE(watcher.pendingCount() == 1);
    REQUIRE(applyAll(watcher).front().key.path == "asset-1.txt");
    watcher.stop();
}

TEST_CASE("FW-1 Sha256Stream, assetFileSha256 and the watcher digest equal picosha2 around 1 MiB",
          "[fw1][digest][sha256]") {
    Sandbox box;
    const std::size_t mib = Sha256Stream::read_chunk_bytes;
    for (const std::size_t length : {std::size_t{0}, std::size_t{3}, mib - 1, mib, mib + 1, 2 * mib + 7}) {
        std::string bytes(length, char{});
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i * 131 + 7) & 0xff);
        if (length == 3) bytes = "abc";
        const auto expected = picosha2::hash256_hex_string(bytes.begin(), bytes.end());
        CHECK(sha256Hex(bytes) == expected);
        Sha256Stream split;
        std::size_t offset = 0;
        std::size_t step = 1;
        while (offset < bytes.size()) {
            const auto count = std::min(step, bytes.size() - offset);
            split.update(bytes.data() + offset, count);
            offset += count;
            step = step * 3 + 1;
        }
        CHECK(split.finishHex() == expected);
        const auto file = box.root / ("hash-" + std::to_string(length) + ".bin");
        writeText(file, bytes);
        CHECK(assetFileSha256(file) == expected);
        const auto digest = readStableContentDigest(file);
        REQUIRE(digest.status == DigestReadStatus::stable);
        CHECK(digest.sha256 == expected);
        CHECK(digest.byte_count == length);
    }
}

// FW-1 measurement, hidden from the default ctest run. Copies
// $PELICAN_TEST_PROJECTS_DIR/example (the corpus test/CMakeLists.txt names)
// into a sandbox and watches it as the project store:
//   (a) the first reconcile, (b) one rewritten file until it is queued (the
//   write and the reconcile; the 200 ms debounce is not counted), (d) a
//   reconcile with no change, and (c) the bytes the watcher hashed.
TEST_CASE("FileWatcher bench on the example project", "[.][bench][fw1]") {
    std::string corpus;
    {
        char *value = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&value, &size, "PELICAN_TEST_PROJECTS_DIR") == 0 && value != nullptr) corpus = value;
        std::free(value);
    }
    if (corpus.empty() || !std::filesystem::exists(std::filesystem::path{corpus} / "example" / "project.json")) {
        SKIP("PELICAN_TEST_PROJECTS_DIR/example is not available");
    }
    Sandbox box;
    const auto root = box.root / "example";
    std::filesystem::copy(std::filesystem::path{corpus} / "example", root,
                          std::filesystem::copy_options::recursive);
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::filesystem::path target;
    std::uint64_t target_size = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file()) continue;
        ++files;
        bytes += entry.file_size();
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.starts_with("assets/") && entry.file_size() > target_size) {
            target = entry.path();
            target_size = entry.file_size();
        }
    }
    REQUIRE_FALSE(target.empty());
    const auto ms = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };

    GateFixture gate;
    FileWatcherOptions options;
    options.manual_clock = true;
    FileWatcher watcher({{"project", root}}, [&] { return gate.snapshot(); }, options);
    auto begin = std::chrono::steady_clock::now();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now());
    const auto first = std::chrono::steady_clock::now() - begin;
    REQUIRE(watcher.status().state == WatcherState::watching);
    const auto after_first = watcher.status();

    std::string content;
    {
        std::ifstream input(target, std::ios::binary);
        content.assign(std::istreambuf_iterator<char>{input}, {});
    }
    content[content.size() / 2] = static_cast<char>(content[content.size() / 2] ^ 0x5a);
    begin = std::chrono::steady_clock::now();
    writeText(target, content);
    const auto wrote = std::chrono::steady_clock::now() - begin;
    // The debounce a real save waits before its scan (not counted below).
    std::this_thread::sleep_for(250ms);
    watcher.notifyPathForTesting(target);
    begin = std::chrono::steady_clock::now();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    const auto changed = wrote + (std::chrono::steady_clock::now() - begin);
    REQUIRE(watcher.pendingCount() == 1);
    REQUIRE(applyAll(watcher).size() == 1);
    const auto after_change = watcher.status();

    begin = std::chrono::steady_clock::now();
    watcher.injectOverflowZeroBytes();
    watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
    const auto unchanged = std::chrono::steady_clock::now() - begin;
    REQUIRE(watcher.pendingCount() == 0);
    const auto after_unchanged = watcher.status();

    // (f) the stat walk alone over 3000 settled files in 30 directories.
    {
        Sandbox many;
        for (int index = 0; index < 3000; ++index) {
            const auto path = many.root / ("dir-" + std::to_string(index % 30)) /
                              ("file-" + std::to_string(index) + ".bin");
            writeText(path, "x");
            setAge(path, 60min);
        }
        FileWatcher walker({{"project", many.root}}, [&] { return gate.snapshot(); }, options);
        begin = std::chrono::steady_clock::now();
        walker.runControlCycleForTesting(FileWatcher::Clock::now());
        const auto seeded = std::chrono::steady_clock::now() - begin;
        begin = std::chrono::steady_clock::now();
        walker.injectOverflowZeroBytes();
        walker.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
        const auto walked = std::chrono::steady_clock::now() - begin;
        REQUIRE(walker.status().hashed_files == 0);
        // For scale: listing the same tree with size and mtime, nothing else.
        begin = std::chrono::steady_clock::now();
        std::uint64_t listed = 0;
        for (const auto &entry : std::filesystem::recursive_directory_iterator{many.root}) {
            if (entry.is_regular_file()) listed += entry.file_size() + (entry.last_write_time().time_since_epoch().count() & 1);
        }
        const auto bare = std::chrono::steady_clock::now() - begin;
        REQUIRE(listed >= 3000);
        std::printf("fw1-bench: (f) 3000 files: start-up %.1f ms, reconcile with no change %.1f ms "
                    "(the bare listing %.1f ms)\n",
                    ms(seeded), ms(walked), ms(bare));
        walker.stop();
    }
    std::printf("fw1-bench: (c) hashed files/bytes: first %llu/%llu, rewrite %llu/%llu, no change %llu/%llu "
                "(scans %llu)\n",
                static_cast<unsigned long long>(after_first.hashed_files),
                static_cast<unsigned long long>(after_first.hashed_bytes),
                static_cast<unsigned long long>(after_change.hashed_files - after_first.hashed_files),
                static_cast<unsigned long long>(after_change.hashed_bytes - after_first.hashed_bytes),
                static_cast<unsigned long long>(after_unchanged.hashed_files - after_change.hashed_files),
                static_cast<unsigned long long>(after_unchanged.hashed_bytes - after_change.hashed_bytes),
                static_cast<unsigned long long>(after_unchanged.scans));

    std::printf("fw1-bench: store files=%llu bytes=%llu; rewritten %s (%llu bytes)\n",
                static_cast<unsigned long long>(files), static_cast<unsigned long long>(bytes),
                std::filesystem::relative(target, root).generic_string().c_str(),
                static_cast<unsigned long long>(target_size));
    std::printf("fw1-bench: (a) first reconcile %.1f ms; (b) rewrite to queued %.1f ms; "
                "(d) reconcile with no change %.1f ms\n",
                ms(first), ms(changed), ms(unchanged));
    watcher.stop();
}

TEST_CASE("HW-1 a declared watch covers its folders and files only, component-wise, and hashes nothing else",
          "[hw1][watcher]") {
    Sandbox box;
    const auto in_folder = box.root / "assets" / "textures" / "a.png";
    const auto nested = box.root / "assets" / "textures" / "deep" / "n.png";
    const auto sibling = box.root / "assets" / "tex" / "b.png";
    const auto file = box.root / "assets" / "models" / "hero.glb";
    const auto other_model = box.root / "assets" / "models" / "other.glb";
    const auto loose = box.root / "root.txt";
    for (const auto &path : {in_folder, nested, sibling, file, other_model, loose}) {
        writeText(path, "v1");
        setAge(path, 60min);
    }
    GateFixture gate;
    auto options = watchingOptions();
    options.watch_paths = std::vector<std::string>{"assets/textures", "assets/models/hero.glb"};
    FileWatcher watcher({{"project", box.root}}, [&] { return gate.snapshot(); }, options);
    watcher.runControlCycleForTesting(FileWatcher::Clock::now());
    REQUIRE(watcher.status().state == WatcherState::watching);
    REQUIRE(watcher.status().hashed_files == 0);

    SECTION("only the covered files are queued, and only they are hashed") {
        for (const auto &path : {in_folder, nested, sibling, file, other_model, loose}) {
            rewriteAged(path, "v2 changed", 30min);
        }
        settleNotifications();
        watcher.injectOverflowZeroBytes(); // a full rescan, whatever was notified
        watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
        CHECK(watcher.status().hashed_files == 3);
        std::set<std::string> keys;
        for (const auto &request : applyAll(watcher)) keys.insert(request.key.path);
        CHECK(keys == std::set<std::string>{"assets/models/hero.glb", "assets/textures/a.png",
                                            "assets/textures/deep/n.png"});
    }
    SECTION("a notification naming only files outside the declaration does not scan") {
        const auto seeded = watcher.status();
        rewriteAged(sibling, "v2 sibling", 30min);
        rewriteAged(loose, "v2 loose", 30min);
        settleNotifications();
        watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
        CHECK(watcher.status().scans == seeded.scans);
        CHECK(watcher.pendingCount() == 0);

        rewriteAged(in_folder, "v2 inside", 30min);
        settleNotifications();
        watcher.runControlCycleForTesting(FileWatcher::Clock::now() + 1s);
        CHECK(watcher.status().scans > seeded.scans);
        CHECK(watcher.status().hashed_files == 1);
        const auto requests = applyAll(watcher);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().key.path == "assets/textures/a.png");
    }
    watcher.stop();
}

TEST_CASE("HW-1 a store the declaration cannot reach is neither scanned nor watched",
          "[hw1][watcher][stores]") {
    Sandbox box;
    const auto project = box.root / "project";
    const auto art = box.root / "art_store";
    const auto music = box.root / "music_store";
    writeText(project / "readme.txt", "v1");
    writeText(art / "characters" / "hero.png", "v1");
    writeText(art / "props" / "box.png", "v1");
    writeText(music / "theme.ogg", "v1");
    GateFixture gate;
    std::set<std::string> armed;
    FileWatcherOptions options;
    options.manual_clock = true;
    options.poll_interval = 20ms;
    options.watch_arm_override = [&](const WatchStore &store, unsigned) {
        armed.insert(store.name);
        return false;
    };
    options.watch_paths = std::vector<std::string>{"art/characters"};
    FileWatcher watcher({{"project", project, {}}, {"art", art, "art"}, {"music", music, "music"}},
                        [&] { return gate.snapshot(); }, options);
    const auto t0 = FileWatcher::Clock::now();
    watcher.runControlCycleForTesting(t0);
    CHECK(armed == std::set<std::string>{"art", "project"});

    writeText(project / "readme.txt", "v2");
    writeText(art / "characters" / "hero.png", "v2");
    writeText(art / "props" / "box.png", "v2");
    writeText(music / "theme.ogg", "v2");
    watcher.runControlCycleForTesting(t0 + 25ms);
    const auto requests = applyAll(watcher);
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().key == makeAssetKey("project://art/characters/hero.png"));

    // An empty declaration watches nothing at all.
    armed.clear();
    options.watch_paths = std::vector<std::string>{};
    FileWatcher nothing({{"project", project, {}}, {"art", art, "art"}},
                        [&] { return gate.snapshot(); }, options);
    nothing.runControlCycleForTesting(t0);
    CHECK(armed.empty());
    writeText(art / "characters" / "hero.png", "v3");
    nothing.runControlCycleForTesting(t0 + 25ms);
    CHECK(applyAll(nothing).empty());
    nothing.stop();
    watcher.stop();
}

TEST_CASE("HW-1 central gate: a declaration opens the watch under an RPC driver, nothing else does",
          "[hw1][gate]") {
    EngineLaunchConfig config;
    config.shader_hot_reload = true;
    config.rpc = true;
    ReloadGate gate;
    gate.configureFromLaunch(config);
    // Undeclared: as before.
    CHECK_FALSE(gate.enabled());
    CHECK(gate.snapshot().reason == "rpc driver");
    CHECK(gate.rpcDriven());
    const auto shut_epoch = gate.snapshot().epoch;

    gate.setWatchDeclaration(WatchDeclaration::listed);
    CHECK(gate.enabled());
    CHECK(gate.snapshot().reason.empty());
    CHECK(gate.snapshot().epoch > shut_epoch);
    // A launch reconfiguration (the renderer's per-frame sync) keeps it.
    gate.configureFromLaunch(config);
    CHECK(gate.enabled());
    // The explicit reload_shaders request is refused by the same reasons only.
    CHECK(gate.explicitShaderReloadRefusal().empty());

    gate.setWatchDeclaration(WatchDeclaration::empty);
    CHECK_FALSE(gate.enabled());
    CHECK(gate.snapshot().reason == "hot_reload empty");

    gate.setWatchDeclaration(WatchDeclaration::listed);
    gate.setReason(ReloadGateReason::replay, true);
    CHECK_FALSE(gate.enabled());
    CHECK(gate.snapshot().reason == "replay");
    gate.setReason(ReloadGateReason::replay, false);
    config.strict_assets = true;
    gate.configureFromLaunch(config);
    CHECK_FALSE(gate.enabled());
    CHECK(gate.snapshot().reason == "strict assets");
    config.strict_assets = false;

    // Headless (shader_hot_reload off) stays shut with a declaration.
    config.shader_hot_reload = false;
    gate.configureFromLaunch(config);
    CHECK_FALSE(gate.enabled());
    CHECK(gate.snapshot().reason == "shader_hot_reload disabled");

    // Without an RPC driver: undeclared watches, an empty list does not.
    config.shader_hot_reload = true;
    config.rpc = false;
    gate.configureFromLaunch(config);
    gate.setWatchDeclaration(WatchDeclaration::undeclared);
    CHECK(gate.enabled());
    CHECK_FALSE(gate.rpcDriven());
    gate.setWatchDeclaration(WatchDeclaration::empty);
    CHECK_FALSE(gate.enabled());
    CHECK(gate.snapshot().reason == "hot_reload empty");
    gate.setWatchDeclaration(WatchDeclaration::listed);
    CHECK(gate.enabled());
}

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
