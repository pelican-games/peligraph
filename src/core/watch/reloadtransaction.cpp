#include "reloadtransaction.hpp"

#include "../log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican::watch {
namespace {

constexpr std::uint64_t index_mask = 0xffffffffull;

LogicalAssetId makeLogicalAssetId(std::uint32_t index, std::uint32_t generation) {
    return LogicalAssetId{(static_cast<std::uint64_t>(generation) << 32) | index};
}

std::vector<AssetKey> canonicalEdges(std::vector<AssetKey> edges) {
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return edges;
}

struct ResourceRecord {
    AssetKey source;
    std::uint64_t content_revision = 1;
    std::uint64_t compatibility_revision = 0;
    std::size_t live_bytes = 0;
    std::shared_ptr<const void> payload;
    std::vector<AssetKey> dependencies;
};

struct ResourceSlot {
    std::uint32_t generation = 0;
    std::optional<ResourceRecord> record;
};

struct ResourceTable {
    std::vector<ResourceSlot> slots;
    std::vector<std::uint32_t> free_slots;
    std::map<AssetKey, std::uint32_t> by_source;
};

void eraseEdges(ResourceRegistryState &state, const LogicalResourceRef &ref,
                const std::vector<AssetKey> &dependencies);
void addEdges(ResourceRegistryState &state, const LogicalResourceRef &ref,
              const std::vector<AssetKey> &dependencies);

ResourceRecord &liveRecord(ResourceRegistryState &state, const LogicalResourceRef &ref);
const ResourceRecord *findRecord(const ResourceRegistryState &state, const LogicalResourceRef &ref);

} // namespace

struct ResourceRegistryState {
    std::map<std::string, ResourceTable> tables;
    std::map<AssetKey, std::set<LogicalResourceRef>> reverse;
    std::size_t resource_count = 0;
    std::size_t live_bytes = 0;
};

namespace {

const ResourceRecord *findRecord(const ResourceRegistryState &state, const LogicalResourceRef &ref) {
    const auto table = state.tables.find(ref.table);
    const auto index = logicalAssetIndex(ref.id);
    if (table == state.tables.end() || index >= table->second.slots.size()) return nullptr;
    const auto &slot = table->second.slots[index];
    if (!slot.record || slot.generation != logicalAssetGeneration(ref.id)) return nullptr;
    return &*slot.record;
}

ResourceRecord &liveRecord(ResourceRegistryState &state, const LogicalResourceRef &ref) {
    auto table = state.tables.find(ref.table);
    const auto index = logicalAssetIndex(ref.id);
    if (table == state.tables.end() || index >= table->second.slots.size()) {
        throw std::runtime_error("stale logical resource handle");
    }
    auto &slot = table->second.slots[index];
    if (!slot.record || slot.generation != logicalAssetGeneration(ref.id)) {
        throw std::runtime_error("stale logical resource handle");
    }
    return *slot.record;
}

void eraseEdges(ResourceRegistryState &state, const LogicalResourceRef &ref,
                const std::vector<AssetKey> &dependencies) {
    for (const auto &key : dependencies) {
        const auto found = state.reverse.find(key);
        if (found == state.reverse.end()) continue;
        found->second.erase(ref);
        if (found->second.empty()) state.reverse.erase(found);
    }
}

void addEdges(ResourceRegistryState &state, const LogicalResourceRef &ref,
              const std::vector<AssetKey> &dependencies) {
    for (const auto &key : dependencies) state.reverse[key].insert(ref);
}

std::vector<std::size_t> topologicalOrder(const std::vector<ReloadActor> &actors) {
    std::map<LogicalResourceRef, std::size_t> actor_for;
    for (std::size_t i = 0; i < actors.size(); ++i) {
        if (!actor_for.emplace(actors[i].target, i).second) {
            throw std::runtime_error("transaction contains duplicate logical target");
        }
    }

    std::vector<std::vector<std::size_t>> outgoing(actors.size());
    std::vector<std::size_t> incoming(actors.size(), 0);
    for (std::size_t i = 0; i < actors.size(); ++i) {
        for (const auto &dependency : actors[i].after) {
            const auto found = actor_for.find(dependency);
            if (found == actor_for.end()) continue;
            outgoing[found->second].push_back(i);
            ++incoming[i];
        }
    }

    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < incoming.size(); ++i) {
        if (incoming[i] == 0) ready.push_back(i);
    }
    std::vector<std::size_t> result;
    while (!ready.empty()) {
        const auto current = ready.front();
        ready.erase(ready.begin());
        result.push_back(current);
        for (const auto next : outgoing[current]) {
            if (--incoming[next] == 0) ready.push_back(next);
        }
    }
    if (result.size() != actors.size()) {
        throw std::runtime_error("reload transaction dependency cycle");
    }
    return result;
}

} // namespace

std::uint32_t logicalAssetIndex(LogicalAssetId id) noexcept {
    return static_cast<std::uint32_t>(id.value & index_mask);
}

std::uint32_t logicalAssetGeneration(LogicalAssetId id) noexcept {
    return static_cast<std::uint32_t>(id.value >> 32);
}

ResourceSnapshot::ResourceSnapshot(std::shared_ptr<const ResourceRegistryState> state)
    : state_{std::move(state)} {}

std::optional<ResourceView> ResourceSnapshot::find(const LogicalResourceRef &ref) const {
    const auto *record = findRecord(*state_, ref);
    if (!record) return std::nullopt;
    return ResourceView{ref, record->source, record->content_revision,
                        record->compatibility_revision, record->live_bytes,
                        record->payload, record->dependencies};
}

std::optional<ResourceView> ResourceSnapshot::find(std::string_view table,
                                                    const AssetKey &source) const {
    const auto found_table = state_->tables.find(std::string{table});
    if (found_table == state_->tables.end()) return std::nullopt;
    const auto found = found_table->second.by_source.find(source);
    if (found == found_table->second.by_source.end()) return std::nullopt;
    const auto &slot = found_table->second.slots[found->second];
    const LogicalResourceRef ref{std::string{table}, makeLogicalAssetId(found->second, slot.generation)};
    return find(ref);
}

std::vector<LogicalResourceRef> ResourceSnapshot::reverseDependents(const AssetKey &dependency) const {
    const auto found = state_->reverse.find(dependency);
    if (found == state_->reverse.end()) return {};
    return {found->second.begin(), found->second.end()};
}

std::size_t ResourceSnapshot::resourceCount() const noexcept { return state_->resource_count; }
std::size_t ResourceSnapshot::liveBytes() const noexcept { return state_->live_bytes; }

ResourceRegistry::ResourceRegistry() : state_{std::make_shared<ResourceRegistryState>()} {}
ResourceRegistry::~ResourceRegistry() = default;

LogicalResourceRef ResourceRegistry::declareResource(
    std::string table_name, AssetKey source, std::shared_ptr<const void> payload,
    std::vector<AssetKey> dependencies, std::uint64_t compatibility_revision,
    std::size_t live_bytes) {
    if (table_name.empty() || !payload) throw std::runtime_error("resource declaration is incomplete");
    dependencies = canonicalEdges(std::move(dependencies));
    std::scoped_lock lock{write_mutex_};
    auto next = std::make_shared<ResourceRegistryState>(*state_.load());
    auto &table = next->tables[table_name];
    if (table.by_source.contains(source)) throw std::runtime_error("resource is already declared");

    std::uint32_t index = 0;
    if (table.free_slots.empty()) {
        if (table.slots.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("logical resource table exhausted");
        }
        index = static_cast<std::uint32_t>(table.slots.size());
        table.slots.push_back(ResourceSlot{1, std::nullopt});
    } else {
        index = table.free_slots.back();
        table.free_slots.pop_back();
        auto &generation = table.slots[index].generation;
        if (generation == std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("logical resource generation exhausted");
        }
        ++generation;
    }
    auto &slot = table.slots[index];
    slot.record = ResourceRecord{source, 1, compatibility_revision, live_bytes,
                                 std::move(payload), std::move(dependencies)};
    table.by_source.emplace(slot.record->source, index);
    const LogicalResourceRef ref{table_name, makeLogicalAssetId(index, slot.generation)};
    addEdges(*next, ref, slot.record->dependencies);
    ++next->resource_count;
    next->live_bytes += live_bytes;
    state_.store(std::shared_ptr<const ResourceRegistryState>{std::move(next)});
    return ref;
}

std::shared_ptr<const void> ResourceRegistry::destroyResource(const LogicalResourceRef &ref) {
    std::scoped_lock lock{write_mutex_};
    auto next = std::make_shared<ResourceRegistryState>(*state_.load());
    auto &record = liveRecord(*next, ref);
    auto &table = next->tables.at(ref.table);
    const auto index = logicalAssetIndex(ref.id);
    eraseEdges(*next, ref, record.dependencies);
    table.by_source.erase(record.source);
    next->live_bytes -= record.live_bytes;
    --next->resource_count;
    auto retired = std::move(record.payload);
    table.slots[index].record.reset();
    table.free_slots.push_back(index);
    state_.store(std::shared_ptr<const ResourceRegistryState>{std::move(next)});
    return retired;
}

ResourceSnapshot ResourceRegistry::snapshot() const {
    return ResourceSnapshot{state_.load()};
}

std::vector<std::shared_ptr<const void>> ResourceRegistry::publish(
    const std::vector<StagedResource> &resources) {
    if (resources.empty()) throw std::runtime_error("reload transaction has no resources");
    std::scoped_lock lock{write_mutex_};
    auto next = std::make_shared<ResourceRegistryState>(*state_.load());
    std::set<LogicalResourceRef> unique;
    for (const auto &resource : resources) {
        if (!resource.payload || !unique.insert(resource.target).second) {
            throw std::runtime_error("staged resource is null or duplicated");
        }
        (void)liveRecord(*next, resource.target);
    }

    std::vector<std::shared_ptr<const void>> retired;
    retired.reserve(resources.size());
    for (const auto &resource : resources) {
        auto &record = liveRecord(*next, resource.target);
        eraseEdges(*next, resource.target, record.dependencies);
        next->live_bytes -= record.live_bytes;
        if (record.content_revision == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("content revision exhausted");
        }
        retired.push_back(std::move(record.payload));
        record.payload = resource.payload;
        record.dependencies = canonicalEdges(resource.dependencies);
        record.compatibility_revision = resource.compatibility_revision;
        record.live_bytes = resource.live_bytes;
        ++record.content_revision;
        next->live_bytes += record.live_bytes;
        addEdges(*next, resource.target, record.dependencies);
    }
    state_.store(std::shared_ptr<const ResourceRegistryState>{std::move(next)});
    return retired;
}

ReloadTransactionGroup::ReloadTransactionGroup(std::string name) : name_{std::move(name)} {
    if (name_.empty()) throw std::runtime_error("reload transaction group requires a name");
}

void ReloadTransactionGroup::add(ReloadActor actor) {
    if (actor.name.empty() || !actor.stage) throw std::runtime_error("reload actor is incomplete");
    actors_.push_back(std::move(actor));
}

void ReloadCoordinator::enqueue(ReloadTransactionGroup group) {
    std::scoped_lock lock{mutex_};
    pending_.push_back(std::move(group));
}

std::size_t ReloadCoordinator::applyFrame(const RetireSink &retire) {
    std::vector<ReloadTransactionGroup> groups;
    {
        std::scoped_lock lock{mutex_};
        groups.swap(pending_);
    }
    for (auto &group : groups) execute(group, retire);
    return groups.size();
}

ResourceReloadStatus ReloadCoordinator::status() const {
    std::scoped_lock lock{mutex_};
    return status_;
}

void ReloadCoordinator::recordFailure(std::string path, std::string message) {
    std::scoped_lock lock{mutex_};
    ++status_.failed;
    status_.last_reload_error = ResourceReloadError{std::move(path), "transaction", std::move(message)};
}

bool ReloadCoordinator::execute(ReloadTransactionGroup &group, const RetireSink &retire) {
    const auto started = std::chrono::steady_clock::now();
    try {
        if (group.actors_.empty()) throw std::runtime_error("reload transaction group is empty");
        for (auto &actor : group.actors_) if (actor.parse) actor.parse();
        for (auto &actor : group.actors_) if (actor.validate) actor.validate();

        std::vector<StagedResource> staged;
        staged.reserve(group.actors_.size());
        for (auto &actor : group.actors_) {
            auto data = actor.stage();
            staged.push_back(StagedResource{actor.target, std::move(data.payload),
                                            std::move(data.dependencies),
                                            data.compatibility_revision, data.live_bytes});
        }

        const auto order = topologicalOrder(group.actors_);
        std::vector<StagedResource> ordered;
        ordered.reserve(staged.size());
        for (const auto index : order) ordered.push_back(std::move(staged[index]));
        auto retired = registry_.publish(ordered);
        if (retire) for (auto &payload : retired) retire(std::move(payload));
        {
            std::scoped_lock lock{mutex_};
            ++status_.applied;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (logger) LOG_INFO(logger, "reload: {} ok ({}ms)", group.name_, elapsed);
        return true;
    } catch (const std::exception &error) {
        recordFailure(group.name_, error.what());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (logger) LOG_WARNING(logger, "reload: {} failed ({}ms): {}", group.name_, elapsed, error.what());
        return false;
    }
}

} // namespace Pelican::watch
