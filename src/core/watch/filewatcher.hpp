#pragma once

#include "contentdigest.hpp"
#include "reloadgate.hpp"
#include "reloadqueue.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Pelican::watch {

enum class WatcherState { disabled, reconciling, watching, polling, degraded };

std::string_view watcherStateName(WatcherState state) noexcept;

struct WatcherStatus {
    WatcherState state = WatcherState::disabled;
    std::uint64_t epoch = 1;
    std::string error;
    // FW-1: totals since construction. A scan compares each file's size and
    // mtime with the previous scan and hashes only the files that differ (and
    // none at start-up); `scans` counts completed inventory passes.
    std::uint64_t scans = 0;
    std::uint64_t hashed_files = 0;
    std::uint64_t hashed_bytes = 0;
};

// A store with an empty logical_mount is laid over the project root: its
// top-level `.pelican` directory (the engine's own caches and metadata) is
// neither scanned nor a reason to scan.
struct WatchStore {
    std::string name;
    std::filesystem::path root;
    std::string logical_mount;
};

struct FileWatcherOptions {
    std::chrono::milliseconds debounce{200};
    std::chrono::milliseconds poll_interval{2000};
    std::chrono::milliseconds retry_backoff{500};
    unsigned failures_before_degraded = 3;
    bool manual_clock = false;
    // A deterministic fixture may reject native watch arming. The production
    // path leaves this empty and uses ReadDirectoryChangesW.
    std::function<bool(const WatchStore &, unsigned attempt)> watch_arm_override;
    // HW-1: project.json `hot_reload`. nullopt watches every store. A list
    // watches only the files whose AssetKey path equals an entry or lies
    // under it as a folder (component-wise: `assets/tex` does not cover
    // `assets/textures/a.png`). Other files are neither listed into the
    // inventory nor hashed, a notification that names only them does not
    // scan, and a store whose mount neither covers nor lies under an entry
    // is not scanned or natively watched at all.
    std::optional<std::vector<std::string>> watch_paths;
};

// HW-1: true when `logical_path` (an AssetKey path) equals one of `entries`
// or lies under one of them as a folder.
bool watchPathCovers(const std::vector<std::string> &entries, std::string_view logical_path);

class FileWatcher {
  public:
    using Clock = std::chrono::steady_clock;
    using GateProvider = std::function<ReloadGateSnapshot()>;
    using BeforeScanHook = std::function<void()>;
    using BatchReloadHandler =
        std::function<std::vector<bool>(std::span<const ReloadRequest>)>;

    FileWatcher(std::vector<WatchStore> stores, GateProvider gate,
                FileWatcherOptions options = {});
    ~FileWatcher();
    FileWatcher(const FileWatcher &) = delete;
    FileWatcher &operator=(const FileWatcher &) = delete;

    void start();
    void stop();
    WatcherStatus status() const;
    std::size_t pendingCount() const;

    void registerSelfWrite(const AssetKey &key, std::string expected_digest,
                           std::uint64_t epoch, bool runtime_apply_succeeded);
    std::size_t applyFrame(const ReloadHandler &fake_handler);
    std::size_t applyFrameBatch(const BatchReloadHandler &handler);

    // Deterministic and integration-test controls. Both synthetic overflow
    // forms share the same full-inventory recovery path.
    void injectOverflowZeroBytes();
    void injectOverflowNotifyEnumDir();
    void notifyPathForTesting(const std::filesystem::path &path);
    void runControlCycleForTesting(Clock::time_point now);
    void setBeforeScanHook(BeforeScanHook hook);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Pelican::watch

