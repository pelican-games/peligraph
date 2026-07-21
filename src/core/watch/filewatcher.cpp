#include "filewatcher.hpp"

#include "../log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <map>
#include <set>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Pelican::watch {
namespace {

std::string pathUtf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

std::string physicalKey(const std::filesystem::path &path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) canonical = std::filesystem::absolute(path, ec).lexically_normal();
    auto result = pathUtf8(canonical);
#ifdef _WIN32
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return result;
}

bool withinRoot(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    const auto root_key = physicalKey(root);
    const auto candidate_key = physicalKey(candidate);
    if (candidate_key == root_key) return true;
    if (!candidate_key.starts_with(root_key)) return false;
    return candidate_key.size() > root_key.size() && candidate_key[root_key.size()] == '/';
}

#ifdef _WIN32
std::wstring extendedPath(const std::filesystem::path &input) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(input, ec);
    auto path = (ec ? input : absolute).native();
    if (path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

bool isNetworkStore(const std::filesystem::path &path) {
    std::error_code ec;
    const auto resolved = std::filesystem::absolute(path, ec);
    if (ec) return false;
    const auto absolute = resolved.native();
    if (absolute.starts_with(L"\\\\")) return true;
    std::array<wchar_t, MAX_PATH> volume{};
    if (!GetVolumePathNameW(absolute.c_str(), volume.data(), static_cast<DWORD>(volume.size()))) return false;
    return GetDriveTypeW(volume.data()) == DRIVE_REMOTE;
}

class NativeWatch {
  public:
    using Notify = std::function<void(bool)>;

    NativeWatch(WatchStore store, Notify notify) : store_(std::move(store)), notify_(std::move(notify)) {}
    ~NativeWatch() { stop(); }

    bool start(std::string &error) {
        if (running_) return true;
        directory_ = CreateFileW(extendedPath(store_.root).c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory_ == INVALID_HANDLE_VALUE) {
            error = "CreateFileW(directory) failed for " + pathUtf8(store_.root) + ": " +
                    std::to_string(GetLastError());
            return false;
        }
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event_) {
            error = "CreateEventW failed: " + std::to_string(GetLastError());
            CloseHandle(directory_);
            directory_ = INVALID_HANDLE_VALUE;
            return false;
        }
        buffer_.resize(64 * 1024); // network-safe upper bound; DWORD aligned allocation
        stopping_ = false;
        completion_collected_ = false;
        resources_released_ = false;
        if (!arm()) {
            error = "ReadDirectoryChangesW failed for " + pathUtf8(store_.root) + ": " +
                    std::to_string(GetLastError());
            buffer_.clear();
            CloseHandle(event_);
            event_ = nullptr;
            CloseHandle(directory_);
            directory_ = INVALID_HANDLE_VALUE;
            return false;
        }
        running_ = true;
        thread_ = std::thread([this] { threadMain(); });
        return true;
    }

    void stop() {
        if (!running_) return;
        stopping_ = true;
        {
            // Serialize cancellation with the completion thread's stop-check
            // and re-arm. Either stop observes the old request, or it cancels
            // the newly armed request; there is no request-free race window.
            std::scoped_lock lock{io_mutex_};
            CancelIoEx(directory_, &overlapped_);
        }
        {
            std::unique_lock lock{stop_mutex_};
            stop_cv_.wait(lock, [this] { return completion_collected_; });
        }
        // Completion has been collected; kernel no longer owns these objects.
        buffer_.clear();
        buffer_.shrink_to_fit();
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        CloseHandle(directory_);
        directory_ = INVALID_HANDLE_VALUE;
        {
            std::scoped_lock lock{stop_mutex_};
            resources_released_ = true;
        }
        stop_cv_.notify_all();
        thread_.join();
        running_ = false;
        overlapped_ = {};
    }

  private:
    bool arm() {
        ResetEvent(event_);
        overlapped_ = {};
        overlapped_.hEvent = event_;
        return ReadDirectoryChangesW(directory_, buffer_.data(), static_cast<DWORD>(buffer_.size()), TRUE,
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                         FILE_NOTIFY_CHANGE_CREATION,
                                     nullptr, &overlapped_, nullptr) != FALSE;
    }

    void signalCollectedAndAwaitRelease() {
        {
            std::scoped_lock lock{stop_mutex_};
            completion_collected_ = true;
        }
        stop_cv_.notify_all();
        std::unique_lock lock{stop_mutex_};
        stop_cv_.wait(lock, [this] { return resources_released_; });
    }

    void threadMainImpl() {
        for (;;) {
            WaitForSingleObject(event_, INFINITE);
            DWORD bytes = 0;
            const bool ok = GetOverlappedResult(directory_, &overlapped_, &bytes, FALSE) != FALSE;
            const auto error = ok ? ERROR_SUCCESS : GetLastError();
            if (stopping_ || error == ERROR_OPERATION_ABORTED) {
                signalCollectedAndAwaitRelease();
                return;
            }
            const bool overflow = (ok && bytes == 0) || error == ERROR_NOTIFY_ENUM_DIR;
            // Re-arm before handing work to the hash/reconcile worker.
            bool stop_now = false;
            bool armed = false;
            {
                std::scoped_lock lock{io_mutex_};
                stop_now = stopping_.load();
                if (!stop_now) armed = arm();
            }
            if (stop_now) {
                signalCollectedAndAwaitRelease();
                return;
            }
            if (!armed) {
                notify_(true);
                signalCollectedAndAwaitRelease();
                return;
            }
            notify_(overflow || !ok);
        }
    }

    void threadMain() noexcept {
        try {
            threadMainImpl();
        } catch (const std::exception &error) {
            if (logger) {
                LOG_ERROR(logger, "native file watcher failed: {}", error.what());
            }
            try {
                notify_(true);
            } catch (...) {
            }
            signalCollectedAndAwaitRelease();
        } catch (...) {
            if (logger) LOG_ERROR(logger, "native file watcher failed");
            try {
                notify_(true);
            } catch (...) {
            }
            signalCollectedAndAwaitRelease();
        }
    }

    WatchStore store_;
    Notify notify_;
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    OVERLAPPED overlapped_{};
    std::vector<std::byte> buffer_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    bool running_ = false;
    std::mutex io_mutex_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool completion_collected_ = false;
    bool resources_released_ = false;
};
#else
class NativeWatch {
  public:
    using Notify = std::function<void(bool)>;
    NativeWatch(WatchStore, Notify) {}
    bool start(std::string &error) { error = "native watcher unavailable"; return false; }
    void stop() {}
};
#endif

struct InventoryEntry {
    AssetKey key;
    std::filesystem::path path;
    std::string digest;
};

bool keyWithinRoot(std::string_view root, std::string_view candidate) {
    return candidate == root ||
           (candidate.size() > root.size() && candidate.starts_with(root) &&
            candidate[root.size()] == '/');
}

struct InventoryScan {
    std::map<std::string, InventoryEntry> entries;
    std::set<std::string> unreadable_paths;
    std::set<std::string> unreadable_roots;

    bool incomplete() const noexcept {
        return !unreadable_paths.empty() || !unreadable_roots.empty();
    }

    bool unreadable(std::string_view physical) const {
        if (unreadable_paths.contains(std::string{physical})) return true;
        return std::any_of(
            unreadable_roots.begin(), unreadable_roots.end(),
            [physical](const auto &root) {
                return keyWithinRoot(root, physical);
            });
    }
};

} // namespace

std::string_view watcherStateName(WatcherState state) noexcept {
    switch (state) {
    case WatcherState::disabled: return "disabled";
    case WatcherState::reconciling: return "reconciling";
    case WatcherState::watching: return "watching";
    case WatcherState::polling: return "polling";
    case WatcherState::degraded: return "degraded";
    }
    return "disabled";
}

struct FileWatcher::Impl {
    std::vector<WatchStore> stores;
    GateProvider gate;
    FileWatcherOptions options;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool started = false;
    bool stopping = false;
    bool dirty = false;
    bool overflow = false;
    std::uint64_t notification_generation = 0;
    Clock::time_point dirty_since{};
    Clock::time_point next_poll{};
    Clock::time_point next_retry{};
    unsigned arm_attempt = 0;
    unsigned consecutive_failures = 0;
    WatcherStatus status;
    std::vector<std::unique_ptr<NativeWatch>> native;
    std::map<std::string, InventoryEntry> inventory; // canonical physical file -> entry
    ContentDigestState digests;
    ReloadQueue queue;
    BeforeScanHook before_scan;

    Impl(std::vector<WatchStore> input, GateProvider provider, FileWatcherOptions opts)
        : stores(std::move(input)), gate(std::move(provider)), options(std::move(opts)) {
        std::sort(stores.begin(), stores.end(), [](const auto &a, const auto &b) {
            // A declared mount shadows bytes at the same logical location in
            // the project tree. This also makes local override roots identity-
            // neutral: only the selected physical source feeds one AssetKey.
            return std::tuple{a.logical_mount.empty(), a.name, a.root} <
                   std::tuple{b.logical_mount.empty(), b.name, b.root};
        });
        const auto initial = gate();
        status = {WatcherState::disabled, initial.epoch, initial.reason};
    }

    void notify(bool was_overflow) {
        std::scoped_lock lock{mutex};
        dirty = true;
        overflow = overflow || was_overflow;
        dirty_since = Clock::now();
        ++notification_generation;
        cv.notify_all();
    }

    bool armAll(Clock::time_point now) {
        stopNative();
        ++arm_attempt;
        std::vector<std::unique_ptr<NativeWatch>> armed;
        std::string error;
        for (const auto &store : stores) {
#ifdef _WIN32
            if (isNetworkStore(store.root)) {
                error = "network store uses polling fallback: " + store.name;
                continue;
            }
#endif
            if (options.watch_arm_override && !options.watch_arm_override(store, arm_attempt)) {
                error = "watch start rejected by fixture for " + store.name;
                continue;
            }
            auto watch = std::make_unique<NativeWatch>(store, [this](bool ov) { notify(ov); });
            if (!watch->start(error)) continue;
            armed.push_back(std::move(watch));
        }
        if (armed.size() == stores.size()) {
            native = std::move(armed);
            consecutive_failures = 0;
            return true;
        }
        // Per-store fallback: successfully armed local stores remain active
        // while failed/network stores share the polling inventory path.
        native = std::move(armed);
        ++consecutive_failures;
        next_retry = now + options.retry_backoff * (1u << std::min(consecutive_failures - 1, 5u));
        {
            std::scoped_lock lock{mutex};
            status.error = std::move(error);
        }
        return false;
    }

    void stopNative() {
        auto watches = std::move(native);
        for (auto &watch : watches) watch->stop();
    }

    InventoryScan scan(std::uint64_t epoch) {
        BeforeScanHook hook;
        {
            std::scoped_lock lock{mutex};
            hook = before_scan;
        }
        if (hook) hook();
        InventoryScan result;
        std::set<std::string> seen_identities;
        std::set<AssetKey> seen_logical_keys;
        for (const auto &store : stores) {
            std::error_code ec;
            const auto root_key = physicalKey(store.root);
            std::filesystem::recursive_directory_iterator it{
                store.root, std::filesystem::directory_options::skip_permission_denied, ec};
            const std::filesystem::recursive_directory_iterator end;
            if (ec) {
                result.unreadable_roots.insert(root_key);
                continue;
            }
            while (it != end) {
                const auto path = it->path();
                std::error_code metadata_error;
                const bool symlink = it->is_symlink(metadata_error);
                bool directory = false;
                if (!metadata_error && symlink) {
                    directory = it->is_directory(metadata_error);
                    if (!metadata_error && directory) {
                        it.disable_recursion_pending();
                    }
                }
                const bool regular = !metadata_error &&
                                     it->is_regular_file(metadata_error);
                if (metadata_error) {
                    result.unreadable_roots.insert(root_key);
                } else if (regular && withinRoot(store.root, path)) {
                    const auto physical = physicalKey(path);
                    const auto relative =
                        std::filesystem::relative(path, store.root, ec);
                    if (ec || relative.empty() ||
                        relative.native().starts_with(L"..")) {
                        result.unreadable_paths.insert(physical);
                        ec.clear();
                    } else {
                        const auto key =
                            makeAssetKey(store.logical_mount, relative);
                        if (seen_logical_keys.insert(key).second) {
                            ContentDigestResult digest;
                            for (unsigned retry = 0; retry < 20; ++retry) {
                                digest = readStableContentDigest(
                                    path, [this, epoch] {
                                        const auto gate_now = gate();
                                        std::scoped_lock lock{mutex};
                                        return stopping || !gate_now.enabled ||
                                               gate_now.epoch != epoch;
                                    });
                                if (digest.status != DigestReadStatus::retry)
                                    break;
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds{25});
                            }
                            if (digest.status == DigestReadStatus::stable) {
                                std::string identity_key = physical;
                                if (digest.identity &&
                                    (digest.identity->volume != 0 ||
                                     digest.identity->file != 0)) {
                                    identity_key =
                                        "id:" +
                                        std::to_string(
                                            digest.identity->volume) +
                                        ":" +
                                        std::to_string(digest.identity->file);
                                }
                                if (seen_identities.insert(identity_key).second) {
                                    result.entries.emplace(
                                        physical,
                                        InventoryEntry{key, path,
                                                       std::move(digest.sha256)});
                                }
                            } else if (digest.status !=
                                       DigestReadStatus::cancelled) {
                                result.unreadable_paths.insert(physical);
                            }
                        }
                    }
                }

                it.increment(ec);
                if (ec) {
                    result.unreadable_roots.insert(root_key);
                    ec.clear();
                    break;
                }
            }
        }
        return result;
    }

    bool reconcile(std::uint64_t epoch, bool seed_live) {
        for (;;) {
            std::uint64_t generation = 0;
            {
                std::scoped_lock lock{mutex};
                generation = notification_generation;
            }
            auto scanned = scan(epoch);
            auto current = std::move(scanned.entries);
            const auto gate_now = gate();
            {
                std::scoped_lock lock{mutex};
                if (stopping || !gate_now.enabled || gate_now.epoch != epoch) return false;
            }
            if (seed_live) {
                for (const auto &[_, entry] : current) digests.seedLive(entry.key, entry.digest);
            } else {
                for (const auto &[physical, entry] : current) {
                    (void)physical;
                    // Compare against live_digest, not just the previous disk
                    // inventory. A failed candidate must be eligible again on
                    // the next save/reconcile even when its bytes are equal.
                    if (digests.observe(entry.key, entry.digest, epoch) == ObserveDisposition::queue_reload)
                        queue.push({entry.key, ReloadKind::modified, entry.digest, epoch});
                }
                for (const auto &[physical, entry] : inventory) {
                    if (current.contains(physical)) continue;
                    if (scanned.unreadable(physical)) {
                        // Preserve both the previous digest and physical source
                        // until this path can be read again. Unreadable is not
                        // evidence of deletion.
                        current.emplace(physical, entry);
                    } else if (digests.observeMissing(entry.key)) {
                        queue.push({entry.key, ReloadKind::removed,
                                    std::nullopt, epoch});
                    }
                }
            }
            inventory = std::move(current);
            std::scoped_lock lock{mutex};
            if (notification_generation == generation) {
                dirty = scanned.incomplete();
                if (dirty) {
                    dirty_since = Clock::now();
                    status.error =
                        "file watcher scan incomplete; retaining prior inventory";
                } else if (status.error.starts_with(
                               "file watcher scan incomplete")) {
                    status.error.clear();
                }
                overflow = false;
                return true;
            }
            // Events (including another overflow) arrived during scan: one
            // more complete hash pass closes the arm/scan race.
        }
    }

    void disable(const ReloadGateSnapshot &gate_now) {
        queue.discardAll();
        stopNative();
        std::scoped_lock lock{mutex};
        status = {WatcherState::disabled, gate_now.epoch, gate_now.reason};
    }

    void cycle(Clock::time_point now, bool first) {
        const auto gate_now = gate();
        {
            std::scoped_lock lock{mutex};
            if (status.epoch != gate_now.epoch && !gate_now.enabled) {
                // handled below without holding the watcher mutex
            } else if (!gate_now.enabled && status.state == WatcherState::disabled) return;
        }
        if (!gate_now.enabled) { disable(gate_now); return; }

        bool need_reconcile = first;
        {
            std::scoped_lock lock{mutex};
            if (status.state == WatcherState::disabled || status.epoch != gate_now.epoch) need_reconcile = true;
        }
        if (need_reconcile) {
            {
                std::scoped_lock lock{mutex};
                status = {WatcherState::reconciling, gate_now.epoch, {}};
            }
            const bool armed = armAll(now);
            if (!reconcile(gate_now.epoch, first && inventory.empty())) return;
            std::scoped_lock lock{mutex};
            status.state = armed ? WatcherState::watching
                                 : (consecutive_failures >= options.failures_before_degraded
                                        ? WatcherState::degraded : WatcherState::polling);
            status.epoch = gate_now.epoch;
            next_poll = now + options.poll_interval;
            return;
        }

        WatcherState state;
        bool is_dirty;
        Clock::time_point dirtied;
        {
            std::scoped_lock lock{mutex};
            state = status.state;
            is_dirty = dirty;
            dirtied = dirty_since;
        }
        if (state == WatcherState::watching && is_dirty && now - dirtied >= options.debounce) {
            reconcile(gate_now.epoch, false);
            return;
        }
        if (state == WatcherState::polling || state == WatcherState::degraded) {
            if (now >= next_retry) {
                // Required transition order: arm, reconcile while still in
                // polling state, then publish watching (polling stops last).
                if (armAll(now)) {
                    if (reconcile(gate_now.epoch, false)) {
                        std::scoped_lock lock{mutex};
                        status = {WatcherState::watching, gate_now.epoch, {}};
                    }
                    return;
                }
                std::scoped_lock lock{mutex};
                status.state = consecutive_failures >= options.failures_before_degraded
                                   ? WatcherState::degraded : WatcherState::polling;
            }
            if (now >= next_poll) {
                reconcile(gate_now.epoch, false);
                std::scoped_lock lock{mutex};
                next_poll = now + options.poll_interval; // no overlapping scans
            }
        }
    }

    void workerMain() noexcept {
        try {
            bool first = true;
            while (true) {
                const auto now = Clock::now();
                try {
                    cycle(now, first);
                } catch (const std::exception &error) {
                    if (logger) {
                        LOG_ERROR(logger, "file watcher cycle failed: {}",
                                  error.what());
                    }
                    ++consecutive_failures;
                    next_retry = now + options.retry_backoff;
                    next_poll = now + options.poll_interval;
                    std::scoped_lock lock{mutex};
                    status.state = WatcherState::degraded;
                    status.error = error.what();
                } catch (...) {
                    if (logger) LOG_ERROR(logger, "file watcher cycle failed");
                    ++consecutive_failures;
                    next_retry = now + options.retry_backoff;
                    next_poll = now + options.poll_interval;
                    std::scoped_lock lock{mutex};
                    status.state = WatcherState::degraded;
                    status.error = "unknown file watcher cycle failure";
                }
                first = false;
                std::unique_lock lock{mutex};
                if (stopping) break;
                cv.wait_for(lock, std::chrono::milliseconds{25});
                if (stopping) break;
            }
            stopNative();
        } catch (...) {
            // Last-resort thread boundary: no exception may escape a worker
            // entry point and terminate the process.
            try {
                std::scoped_lock lock{mutex};
                status.state = WatcherState::degraded;
            } catch (...) {
            }
            try {
                stopNative();
            } catch (...) {
            }
        }
    }
};

FileWatcher::FileWatcher(std::vector<WatchStore> stores, GateProvider gate, FileWatcherOptions options)
    : impl_(std::make_unique<Impl>(std::move(stores), std::move(gate), std::move(options))) {}
FileWatcher::~FileWatcher() { stop(); }

void FileWatcher::start() {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->started) return;
    impl_->started = true;
    impl_->stopping = false;
    if (!impl_->options.manual_clock) impl_->worker = std::thread([this] { impl_->workerMain(); });
}

void FileWatcher::stop() {
    {
        std::scoped_lock lock{impl_->mutex};
        if (!impl_->started) return;
        impl_->stopping = true;
        impl_->cv.notify_all();
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    else impl_->stopNative();
    impl_->queue.discardAll();
    std::scoped_lock lock{impl_->mutex};
    impl_->status.state = WatcherState::disabled;
    impl_->started = false;
}

WatcherStatus FileWatcher::status() const {
    std::scoped_lock lock{impl_->mutex};
    return impl_->status;
}

std::size_t FileWatcher::pendingCount() const { return impl_->queue.size(); }

void FileWatcher::registerSelfWrite(const AssetKey &key, std::string expected_digest,
                                    std::uint64_t epoch, bool runtime_apply_succeeded) {
    impl_->digests.registerSelfWrite(key, std::move(expected_digest), epoch, runtime_apply_succeeded);
}

std::size_t FileWatcher::applyFrame(const ReloadHandler &fake_handler) {
    return applyFrameBatch([&](std::span<const ReloadRequest> requests) {
        std::vector<bool> results;
        results.reserve(requests.size());
        for (const auto &request : requests) results.push_back(fake_handler(request));
        return results;
    });
}

std::size_t FileWatcher::applyFrameBatch(const BatchReloadHandler &handler) {
    const auto gate = impl_->gate();
    if (!gate.enabled) { impl_->queue.discardAll(); return 0; }
    auto requests = impl_->queue.takeForFrame(gate.epoch);
    if (requests.empty()) return 0;
    auto results = handler(requests);
    if (results.size() != requests.size()) {
        results.assign(requests.size(), false);
    }
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto &request = requests[index];
        if (results[index]) impl_->digests.commitSucceeded(request.key, request.digest);
        else impl_->digests.commitFailed(request.key);
    }
    return requests.size();
}

void FileWatcher::injectOverflowZeroBytes() { impl_->notify(true); }
void FileWatcher::injectOverflowNotifyEnumDir() { impl_->notify(true); }
void FileWatcher::notifyPathForTesting(const std::filesystem::path &) { impl_->notify(false); }

void FileWatcher::runControlCycleForTesting(Clock::time_point now) {
    bool first = false;
    {
        std::scoped_lock lock{impl_->mutex};
        if (!impl_->started) { impl_->started = true; first = true; }
    }
    impl_->cycle(now, first);
}

void FileWatcher::setBeforeScanHook(BeforeScanHook hook) {
    std::scoped_lock lock{impl_->mutex};
    impl_->before_scan = std::move(hook);
}

} // namespace Pelican::watch
