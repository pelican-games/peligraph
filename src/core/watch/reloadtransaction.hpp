#pragma once

#include "../handle.hpp"
#include "assetkey.hpp"

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Pelican::watch {

PELICAN_DEFINE_HANDLE(LogicalAssetId, std::uint64_t)

std::uint32_t logicalAssetIndex(LogicalAssetId id) noexcept;
std::uint32_t logicalAssetGeneration(LogicalAssetId id) noexcept;

struct LogicalResourceRef {
    std::string table;
    LogicalAssetId id{};
    auto operator<=>(const LogicalResourceRef &) const = default;
};

struct ResourceView {
    LogicalResourceRef ref;
    AssetKey source;
    std::uint64_t content_revision = 0;
    // Compatibility/layout/rig changes are intentionally independent from
    // the slot generation used for stale-handle rejection.
    std::uint64_t compatibility_revision = 0;
    std::size_t live_bytes = 0;
    std::shared_ptr<const void> payload;
    std::vector<AssetKey> dependencies;

    template <class T> std::shared_ptr<const T> payloadAs() const {
        return std::static_pointer_cast<const T>(payload);
    }
};

struct ResourceRegistryState;

class ResourceSnapshot {
  public:
    std::optional<ResourceView> find(const LogicalResourceRef &ref) const;
    std::optional<ResourceView> find(std::string_view table, const AssetKey &source) const;
    std::vector<LogicalResourceRef> reverseDependents(const AssetKey &dependency) const;
    std::size_t resourceCount() const noexcept;
    std::size_t liveBytes() const noexcept;

  private:
    friend class ResourceRegistry;
    explicit ResourceSnapshot(std::shared_ptr<const ResourceRegistryState> state);
    std::shared_ptr<const ResourceRegistryState> state_;
};

struct StagedResource {
    LogicalResourceRef target;
    std::shared_ptr<const void> payload;
    std::vector<AssetKey> dependencies;
    std::uint64_t compatibility_revision = 0;
    std::size_t live_bytes = 0;
};

class ResourceRegistry {
  public:
    ResourceRegistry();
    ~ResourceRegistry();
    ResourceRegistry(const ResourceRegistry &) = delete;
    ResourceRegistry &operator=(const ResourceRegistry &) = delete;

    LogicalResourceRef declareResource(std::string table, AssetKey source,
                                       std::shared_ptr<const void> payload,
                                       std::vector<AssetKey> dependencies = {},
                                       std::uint64_t compatibility_revision = 0,
                                       std::size_t live_bytes = 0);
    std::shared_ptr<const void> destroyResource(const LogicalResourceRef &ref);
    ResourceSnapshot snapshot() const;

    // Publishes handle-table changes and reverse-edge changes through one
    // immutable snapshot. The returned old payloads are the DeletionQueue
    // connection point for GPU-backed handlers.
    std::vector<std::shared_ptr<const void>> publish(const std::vector<StagedResource> &resources);

  private:
    mutable std::mutex write_mutex_;
    std::atomic<std::shared_ptr<const ResourceRegistryState>> state_;
};

struct ResourceReloadError {
    std::string path;
    std::string kind;
    std::string message;
};

struct ResourceReloadStatus {
    std::uint64_t applied = 0;
    std::uint64_t failed = 0;
    std::optional<ResourceReloadError> last_reload_error;
};

struct StagedResourceData {
    std::shared_ptr<const void> payload;
    std::vector<AssetKey> dependencies;
    std::uint64_t compatibility_revision = 0;
    std::size_t live_bytes = 0;
};

struct ReloadActor {
    std::string name;
    LogicalResourceRef target;
    // Targets in the same group that must be committed before this actor.
    std::vector<LogicalResourceRef> after;
    std::function<void()> parse;
    std::function<void()> validate;
    std::function<StagedResourceData()> stage;
};

class ReloadTransactionGroup {
  public:
    explicit ReloadTransactionGroup(std::string name);
    void add(ReloadActor actor);
    const std::string &name() const noexcept { return name_; }

  private:
    friend class ReloadCoordinator;
    std::string name_;
    std::vector<ReloadActor> actors_;
};

class ReloadCoordinator {
  public:
    // GPU handlers adapt this to DeletionQueue::defer. The sink is a
    // post-publication ownership transfer and must not throw.
    using RetireSink = std::function<void(std::shared_ptr<const void>)>;

    ResourceRegistry &registry() noexcept { return registry_; }
    const ResourceRegistry &registry() const noexcept { return registry_; }
    void enqueue(ReloadTransactionGroup group);
    // Called once at the frame-start apply phase. Each group is atomic; an
    // unrelated failing group does not prevent later groups from running.
    std::size_t applyFrame(const RetireSink &retire = {});
    ResourceReloadStatus status() const;

  private:
    bool execute(ReloadTransactionGroup &group, const RetireSink &retire);
    void recordFailure(std::string path, std::string message);

    ResourceRegistry registry_;
    mutable std::mutex mutex_;
    std::vector<ReloadTransactionGroup> pending_;
    ResourceReloadStatus status_;
};

} // namespace Pelican::watch
