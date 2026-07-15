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
};

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
};

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

