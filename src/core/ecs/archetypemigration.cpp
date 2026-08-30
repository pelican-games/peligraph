#include "archetypemigration.hpp"

#include "componentinfo.hpp"
#include <details/ecs/chunk.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/coretemplate.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {

struct ECSArchetypeMigrationToken::State {
    ECSCoreTemplatePublic *core = nullptr;
    EntityId entity{};
    ComponentId component = 0;
    ECSArchetypeMigrationKind kind = ECSArchetypeMigrationKind::add;
    std::unique_ptr<ECSCoreTemplatePublic::MutationScope> mutation;

    std::size_t source_chunk_index = 0;
    std::size_t source_row = 0;
    std::size_t changed_component_index = 0;
    std::size_t entity_component_index = 0;
    std::vector<std::pair<std::size_t, std::uint64_t>> source_versions;

    std::vector<ComponentId> target_component_ids;
    std::vector<std::size_t> target_component_indices;
    std::vector<ComponentId> target_key;
    std::unique_ptr<ECSComponentChunk> target_chunk;
    std::unique_ptr<ECSComponentChunk> removed_component_chunk;
    void *staged_component = nullptr;
    void *removed_live_component = nullptr;
    bool added_initialized = false;

    std::vector<ECSArchetypeMigrationAdapter *> adapters;
    bool inserted_target_archetype = false;
    std::optional<std::size_t> reusable_target_chunk_index;
    std::vector<std::pair<std::size_t, std::uint64_t>> target_versions;
    bool published = false;
    bool published_new_target_chunk = false;
    std::size_t published_target_chunk_index =
        std::numeric_limits<std::size_t>::max();
    std::size_t published_target_row = 0;
    EntityId backfilled_entity{};

    ECSArchetypeMigrationPrepareContext prepareContext() const noexcept {
        return ECSArchetypeMigrationPrepareContext{
            .core = *core,
            .entity = entity,
            .component = component,
            .kind = kind,
            .live_component = removed_live_component,
            .staged_component = staged_component,
        };
    }

    void eraseUnpublishedArchetype() noexcept {
        if (!inserted_target_archetype || core == nullptr) return;
        const auto found = core->archetype_to_chunks.find(target_key);
        if (found != core->archetype_to_chunks.end() && found->second.empty()) {
            core->archetype_to_chunks.erase(found);
        }
        inserted_target_archetype = false;
    }

    void discardPrepared() noexcept {
        const auto context = prepareContext();
        for (auto it = adapters.rbegin(); it != adapters.rend(); ++it) {
            (*it)->rollback(context);
        }
        if (added_initialized && staged_component != nullptr) {
            const auto &info = GET_MODULE(ComponentInfoManager)
                                   .getFromIndex(changed_component_index);
            if (info.cb_deinit != nullptr) info.cb_deinit(staged_component);
            added_initialized = false;
        }
        target_chunk.reset();
        removed_component_chunk.reset();
        staged_component = nullptr;
        removed_live_component = nullptr;
        eraseUnpublishedArchetype();
        mutation.reset();
    }

    void publish() noexcept {
        assert(core != nullptr && mutation != nullptr && target_chunk != nullptr &&
               !published);
        auto &source = core->chunks_storage[source_chunk_index];
        const auto source_last_row = source.size() - 1;
        backfilled_entity = *static_cast<EntityId *>(
            source.component_arrays[entity_component_index]->atUnchecked(
                source_last_row));

        published_new_target_chunk = !reusable_target_chunk_index.has_value();
        if (published_new_target_chunk) {
            published_target_chunk_index = core->chunks_storage.size();
            core->chunks_storage.emplace_back(std::move(*target_chunk));
            target_chunk.reset();
            auto &new_target = core->chunks_storage[published_target_chunk_index];
            core->archetype_to_chunks.find(target_key)->second.push_back(
                published_target_chunk_index);
            for (auto &[id, system] : core->systems) {
                (void)id;
                if ((new_target.getMask() & system.matching_mask) ==
                    system.matching_mask) {
                    system.matching_chunk_indices.push_back(
                        published_target_chunk_index);
                }
            }
            published_target_row = 0;
        } else {
            published_target_chunk_index = *reusable_target_chunk_index;
            auto &existing_target =
                core->chunks_storage[published_target_chunk_index];
            published_target_row = existing_target.size();

            // Staging constructed every target component so prepare could
            // fail without touching live storage. Existing chunks already own
            // raw tail capacity: keep only the newly added value and discard
            // the placeholder objects for components sourced from the entity.
            for (const auto component_index : target_chunk->indices) {
                auto &staged_array =
                    *target_chunk->component_arrays[component_index];
                if (kind == ECSArchetypeMigrationKind::add &&
                    component_index == changed_component_index) {
                    auto &target_array =
                        *existing_target.component_arrays[component_index];
                    target_array.relocate_one(
                        target_array.atUnchecked(published_target_row),
                        staged_array.atUnchecked(0));
                    ++target_array.count;
                    --staged_array.count;
                } else {
                    staged_array.destroy_one(staged_array.atUnchecked(0));
                    --staged_array.count;
                }
            }
            target_chunk->count = 0;
        }
        auto &target = core->chunks_storage[published_target_chunk_index];

        if (kind == ECSArchetypeMigrationKind::remove) {
            auto &source_array =
                *source.component_arrays[changed_component_index];
            auto &removed_array = *removed_component_chunk
                                       ->component_arrays[changed_component_index];
            auto *removed_target = removed_array.atUnchecked(0);
            removed_array.destroy_one(removed_target);
            removed_array.relocate_one(
                removed_target, source_array.atUnchecked(source_row));
            if (source_row != source_last_row) {
                source_array.relocate_one(
                    source_array.atUnchecked(source_row),
                    source_array.atUnchecked(source_last_row));
            }
            --source_array.count;
        }

        for (auto it = source.indices.rbegin(); it != source.indices.rend();
             ++it) {
            const auto component_index = *it;
            if (kind == ECSArchetypeMigrationKind::remove &&
                component_index == changed_component_index) {
                continue;
            }
            auto &source_array = *source.component_arrays[component_index];
            auto &target_array = *target.component_arrays[component_index];
            auto *target_ptr =
                target_array.atUnchecked(published_target_row);
            if (published_new_target_chunk) {
                target_array.destroy_one(target_ptr);
            }
            source_array.relocate_one(
                target_ptr, source_array.atUnchecked(source_row));
            if (!published_new_target_chunk) ++target_array.count;
            if (source_row != source_last_row) {
                source_array.relocate_one(
                    source_array.atUnchecked(source_row),
                    source_array.atUnchecked(source_last_row));
            }
            --source_array.count;
        }
        --source.count;
        if (!published_new_target_chunk) {
            ++target.count;
            target_chunk.reset();
        }

        core->id_table[entity.index].ref =
            ECSCoreTemplatePublic::EntityRef{published_target_chunk_index,
                                             published_target_row};
        if (backfilled_entity != entity) {
            core->id_table[backfilled_entity.index].ref =
                ECSCoreTemplatePublic::EntityRef{source_chunk_index, source_row};
        }
        for (const auto component_index : target.indices) {
            target.updateVersion(component_index, core->global_tick);
        }
        for (const auto component_index : source.indices) {
            source.updateVersion(component_index, core->global_tick);
        }

        const ECSArchetypeMigrationPublishContext context{
            .core = *core,
            .entity = entity,
            .component = component,
            .kind = kind,
            .live_component = kind == ECSArchetypeMigrationKind::add
                                  ? target.component_arrays[changed_component_index]
                                        ->atUnchecked(published_target_row)
                                  : nullptr,
        };
        for (auto *adapter : adapters) adapter->publish(context);
        published = true;
        // The inverse token now owns everything required for rollback. Do not
        // keep the core-wide structural guard across the rest of an aggregate
        // editor transaction; the next structural token may now prepare.
        mutation.reset();
    }

    void rollbackPublished() noexcept {
        assert(published && core != nullptr && mutation == nullptr);
        ECSCoreTemplatePublic::MutationScope rollback_mutation{*core};
        if (published_new_target_chunk) {
            assert(published_target_chunk_index + 1 ==
                   core->chunks_storage.size());
        }
        auto &source = core->chunks_storage[source_chunk_index];
        auto &target = core->chunks_storage[published_target_chunk_index];
        assert(published_target_row + 1 == target.size());
        const auto source_last_row = source.size();

        for (auto it = source.indices.rbegin(); it != source.indices.rend();
             ++it) {
            const auto component_index = *it;
            auto &source_array = *source.component_arrays[component_index];
            if (kind == ECSArchetypeMigrationKind::remove &&
                component_index == changed_component_index) {
                auto &removed_array = *removed_component_chunk
                                           ->component_arrays[component_index];
                if (source_row != source_last_row) {
                    source_array.relocate_one(
                        source_array.atUnchecked(source_last_row),
                        source_array.atUnchecked(source_row));
                }
                source_array.relocate_one(
                    source_array.atUnchecked(source_row),
                    removed_array.atUnchecked(0));
                ++source_array.count;
                --removed_array.count;
                continue;
            }
            auto &target_array = *target.component_arrays[component_index];
            if (source_row != source_last_row) {
                source_array.relocate_one(
                    source_array.atUnchecked(source_last_row),
                    source_array.atUnchecked(source_row));
            }
            source_array.relocate_one(source_array.atUnchecked(source_row),
                                      target_array.atUnchecked(
                                          published_target_row));
            ++source_array.count;
            --target_array.count;
        }
        ++source.count;

        core->id_table[entity.index].ref =
            ECSCoreTemplatePublic::EntityRef{source_chunk_index, source_row};
        if (backfilled_entity != entity) {
            core->id_table[backfilled_entity.index].ref =
                ECSCoreTemplatePublic::EntityRef{source_chunk_index,
                                                 source_last_row};
        }
        for (const auto [component_index, version] : source_versions) {
            source.component_versions[component_index] = version;
        }

        const auto inverse_kind =
            kind == ECSArchetypeMigrationKind::add
                ? ECSArchetypeMigrationKind::remove
                : ECSArchetypeMigrationKind::add;
        const ECSArchetypeMigrationPublishContext inverse_context{
            .core = *core,
            .entity = entity,
            .component = component,
            .kind = inverse_kind,
            .live_component = inverse_kind == ECSArchetypeMigrationKind::add
                                  ? source.component_arrays[changed_component_index]
                                        ->atUnchecked(source_row)
                                  : nullptr,
        };
        for (auto it = adapters.rbegin(); it != adapters.rend(); ++it) {
            (*it)->publish(inverse_context);
        }

        if (kind == ECSArchetypeMigrationKind::add) {
            auto *added = target.component_arrays[changed_component_index]
                              ->atUnchecked(published_target_row);
            const auto &info = GET_MODULE(ComponentInfoManager)
                                   .getFromIndex(changed_component_index);
            if (added_initialized && info.cb_deinit != nullptr) {
                info.cb_deinit(added);
            }
            target.component_arrays[changed_component_index]->destroy_one(
                added);
            --target.component_arrays[changed_component_index]->count;
            added_initialized = false;
        }
        --target.count;
        for (const auto [component_index, version] : target_versions) {
            target.component_versions[component_index] = version;
        }

        if (published_new_target_chunk) {
            auto &archetype_chunks =
                core->archetype_to_chunks.find(target_key)->second;
            assert(!archetype_chunks.empty() &&
                   archetype_chunks.back() == published_target_chunk_index);
            archetype_chunks.pop_back();
            for (auto &[id, system] : core->systems) {
                (void)id;
                if ((target.getMask() & system.matching_mask) ==
                    system.matching_mask) {
                    assert(!system.matching_chunk_indices.empty() &&
                           system.matching_chunk_indices.back() ==
                               published_target_chunk_index);
                    system.matching_chunk_indices.pop_back();
                }
            }
            assert(target.size() == 0);
            core->chunks_storage.pop_back();
        }
        published = false;
        published_new_target_chunk = false;
        published_target_chunk_index =
            std::numeric_limits<std::size_t>::max();
        published_target_row = 0;
        removed_component_chunk.reset();
        staged_component = nullptr;
        removed_live_component = nullptr;
        eraseUnpublishedArchetype();
    }

    void finishPublished() noexcept {
        assert(published && mutation == nullptr);
        ECSCoreTemplatePublic::MutationScope finish_mutation{*core};
        if (kind == ECSArchetypeMigrationKind::remove &&
            removed_component_chunk != nullptr) {
            auto &array = *removed_component_chunk
                               ->component_arrays[changed_component_index];
            auto *removed = array.atUnchecked(0);
            const auto &info = GET_MODULE(ComponentInfoManager)
                                   .getFromIndex(changed_component_index);
            if (info.cb_deinit != nullptr) info.cb_deinit(removed);
        }
        removed_component_chunk.reset();
        staged_component = nullptr;
        removed_live_component = nullptr;
    }
};

ECSArchetypeMigrationToken::ECSArchetypeMigrationToken() noexcept = default;
ECSArchetypeMigrationToken::ECSArchetypeMigrationToken(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
ECSArchetypeMigrationToken::~ECSArchetypeMigrationToken() { rollback(); }
ECSArchetypeMigrationToken::ECSArchetypeMigrationToken(
    ECSArchetypeMigrationToken &&) noexcept = default;
ECSArchetypeMigrationToken &ECSArchetypeMigrationToken::operator=(
    ECSArchetypeMigrationToken &&other) noexcept {
    if (this != &other) {
        rollback();
        state_ = std::move(other.state_);
    }
    return *this;
}

bool ECSArchetypeMigrationToken::published() const noexcept {
    return state_ != nullptr && state_->published;
}

void *ECSArchetypeMigrationToken::stagedComponent() const noexcept {
    return state_ == nullptr ? nullptr : state_->staged_component;
}

void *ECSArchetypeMigrationToken::removedComponent() const noexcept {
    return state_ == nullptr ? nullptr : state_->removed_live_component;
}

void ECSArchetypeMigrationToken::publish() noexcept {
    if (state_ != nullptr && !state_->published) state_->publish();
}

void ECSArchetypeMigrationToken::rollback() noexcept {
    if (state_ == nullptr) return;
    if (state_->published) {
        state_->rollbackPublished();
    } else {
        state_->discardPrepared();
    }
    state_.reset();
}

void ECSArchetypeMigrationToken::finish() noexcept {
    if (state_ == nullptr) return;
    if (state_->published) {
        state_->finishPublished();
        state_.reset();
    } else {
        rollback();
    }
}

ECSArchetypeMigrationToken ECSArchetypeMigration::prepareAdd(
    ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
    const Populate &populate, Adapters adapters) {
    return prepare(core, entity, component, ECSArchetypeMigrationKind::add,
                   populate, adapters);
}

ECSArchetypeMigrationToken ECSArchetypeMigration::prepareRemove(
    ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
    Adapters adapters) {
    return prepare(core, entity, component, ECSArchetypeMigrationKind::remove,
                   {}, adapters);
}

void ECSArchetypeMigration::add(ECSCoreTemplatePublic &core, EntityId entity,
                                ComponentId component, const Populate &populate,
                                Adapters adapters) {
    auto token = prepareAdd(core, entity, component, populate, adapters);
    token.publish();
    token.finish();
}

void ECSArchetypeMigration::remove(ECSCoreTemplatePublic &core,
                                   EntityId entity, ComponentId component,
                                   Adapters adapters) {
    auto token = prepareRemove(core, entity, component, adapters);
    token.publish();
    token.finish();
}

ECSArchetypeMigrationToken ECSArchetypeMigration::prepare(
    ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
    ECSArchetypeMigrationKind kind, const Populate &populate,
    Adapters adapters) {
    auto state = std::make_unique<ECSArchetypeMigrationToken::State>();
    state->core = &core;
    state->entity = entity;
    state->component = component;
    state->kind = kind;
    state->mutation =
        std::make_unique<ECSCoreTemplatePublic::MutationScope>(core);

    auto &manager = GET_MODULE(ComponentInfoManager);
    try {
        const auto source_ref = core.resolve(entity);
        if (!source_ref.has_value()) {
            throw std::runtime_error("ECS entity is not live: " +
                                     toString(entity));
        }
        state->source_chunk_index = source_ref->chunk_index;
        state->source_row = source_ref->array_index;

        try {
            state->changed_component_index =
                manager.getIndexFromComponentId(component);
            const auto &info =
                manager.getFromIndex(state->changed_component_index);
            if (info.id != component || info.size == 0 || info.alignment == 0 ||
                info.cb_construct == nullptr || info.cb_destroy == nullptr ||
                info.cb_relocate == nullptr) {
                throw std::out_of_range("unregistered component");
            }
        } catch (const std::out_of_range &) {
            throw std::invalid_argument(
                "ECS component " + std::to_string(component) +
                " is not registered");
        }

        state->entity_component_index = manager.getIndexFromComponentId(
            ComponentIdByType<EntityId>::value);
        auto &source = core.chunks_storage[state->source_chunk_index];
        const bool source_has_component =
            source.has(state->changed_component_index);
        if (kind == ECSArchetypeMigrationKind::add &&
            source_has_component) {
            throw std::invalid_argument(
                "ECS component '" + manager.get(component).name +
                "' already exists on entity " + toString(entity));
        }
        if (kind == ECSArchetypeMigrationKind::remove &&
            component == ComponentIdByType<EntityId>::value) {
            throw std::invalid_argument(
                "ECS EntityId component cannot be removed");
        }
        if (kind == ECSArchetypeMigrationKind::remove &&
            !source_has_component) {
            throw std::invalid_argument(
                "ECS component '" + manager.get(component).name +
                "' does not exist on entity " + toString(entity));
        }

        std::unordered_set<ECSArchetypeMigrationAdapter *> unique_adapters;
        unique_adapters.reserve(adapters.size());
        for (auto *adapter : adapters) {
            if (adapter == nullptr) {
                throw std::invalid_argument(
                    "ECS archetype migration adapter cannot be null");
            }
            if (!unique_adapters.insert(adapter).second) {
                throw std::invalid_argument(
                    "ECS archetype migration adapter is duplicated");
            }
        }
        state->adapters.reserve(adapters.size());

        state->target_component_ids.assign(source.getComponentList().begin(),
                                           source.getComponentList().end());
        if (kind == ECSArchetypeMigrationKind::add) {
            if (state->target_component_ids.size() >= MAX_COMPONENTS) {
                throw std::length_error(
                    "ECS entity has too many component types");
            }
            state->target_component_ids.push_back(component);
        } else {
            state->target_component_ids.erase(
                std::find(state->target_component_ids.begin(),
                          state->target_component_ids.end(), component));
        }
        state->target_component_indices.reserve(
            state->target_component_ids.size());
        for (const auto target_component : state->target_component_ids) {
            state->target_component_indices.push_back(
                manager.getIndexFromComponentId(target_component));
        }
        state->target_key = state->target_component_ids;
        std::sort(state->target_key.begin(), state->target_key.end());

        state->target_chunk = std::make_unique<ECSComponentChunk>(
            state->target_component_indices, state->target_component_ids);
        std::vector<void *> target_ptrs(
            state->target_component_indices.size());
        state->target_chunk->allocate(state->target_component_indices,
                                      target_ptrs, 1);
        if (kind == ECSArchetypeMigrationKind::add) {
            const auto target_position = static_cast<std::size_t>(
                std::find(state->target_component_indices.begin(),
                          state->target_component_indices.end(),
                          state->changed_component_index) -
                state->target_component_indices.begin());
            state->staged_component = target_ptrs[target_position];
            if (populate) populate(state->staged_component);
            const auto &info =
                manager.getFromIndex(state->changed_component_index);
            if (info.cb_init != nullptr) {
                info.cb_init(state->staged_component);
                state->added_initialized = true;
            }
        } else {
            state->removed_live_component = source.component_arrays
                                                 [state->changed_component_index]
                                                     ->atUnchecked(
                                                         state->source_row);
            const std::array removed_ids{component};
            const std::array removed_indices{
                state->changed_component_index};
            state->removed_component_chunk =
                std::make_unique<ECSComponentChunk>(removed_indices,
                                                    removed_ids);
            std::array<void *, 1> removed_ptr{};
            state->removed_component_chunk->allocate(removed_indices,
                                                      removed_ptr, 1);
        }

        state->source_versions.reserve(source.indices.size());
        for (const auto index : source.indices) {
            state->source_versions.emplace_back(index,
                                                source.getVersion(index));
        }

        auto [archetype, inserted] =
            core.archetype_to_chunks.try_emplace(state->target_key);
        state->inserted_target_archetype = inserted;
        for (auto it = archetype->second.rbegin();
             it != archetype->second.rend(); ++it) {
            auto &candidate = core.chunks_storage[*it];
            if (candidate.remainingCapacity() == 0) continue;
            state->reusable_target_chunk_index = *it;
            state->target_versions.reserve(candidate.indices.size());
            for (const auto index : candidate.indices) {
                state->target_versions.emplace_back(
                    index, candidate.getVersion(index));
            }
            break;
        }
        if (!state->reusable_target_chunk_index) {
            core.chunks_storage.reserve(core.chunks_storage.size() + 1);
            archetype->second.reserve(archetype->second.size() + 1);
            const auto target_mask = state->target_chunk->getMask();
            for (auto &[id, system] : core.systems) {
                (void)id;
                if ((target_mask & system.matching_mask) ==
                    system.matching_mask) {
                    system.matching_chunk_indices.reserve(
                        system.matching_chunk_indices.size() + 1);
                }
            }
        }

        const auto context = state->prepareContext();
        for (auto *adapter : adapters) {
            state->adapters.push_back(adapter);
            adapter->prepare(context);
        }
        return ECSArchetypeMigrationToken{std::move(state)};
    } catch (...) {
        state->discardPrepared();
        throw;
    }
}

struct ECSEntityMutationToken::State {
    ECSCoreTemplatePublic *core = nullptr;
    ECSEntityMutationKind kind = ECSEntityMutationKind::create;
    EntityId entity{};
    std::unique_ptr<ECSCoreTemplatePublic::MutationScope> mutation;

    std::vector<ComponentId> component_ids;
    std::vector<std::size_t> component_indices;
    std::vector<ComponentId> archetype_key;
    std::unique_ptr<ECSComponentChunk> staged_chunk;
    std::vector<std::size_t> initialized_indices;
    bool reuses_id = false;
    bool inserted_archetype = false;
    std::optional<std::size_t> reusable_chunk_index;
    std::vector<std::pair<std::size_t, std::uint64_t>> target_versions;

    std::size_t source_chunk_index = 0;
    std::size_t source_row = 0;
    std::vector<std::pair<std::size_t, std::uint64_t>> source_versions;
    std::optional<ECSCoreTemplatePublic::IdEntry> old_id_entry;
    EntityId backfilled_entity{};
    bool free_index_published = false;

    bool published = false;
    bool published_new_chunk = false;
    std::size_t published_chunk_index =
        std::numeric_limits<std::size_t>::max();
    std::size_t published_row = 0;

    void eraseEmptyArchetype() noexcept {
        if (!inserted_archetype) return;
        const auto found = core->archetype_to_chunks.find(archetype_key);
        if (found != core->archetype_to_chunks.end() && found->second.empty()) {
            core->archetype_to_chunks.erase(found);
        }
        inserted_archetype = false;
    }

    void deinitializeStaged() noexcept {
        if (staged_chunk == nullptr) return;
        auto &manager = GET_MODULE(ComponentInfoManager);
        for (auto it = initialized_indices.rbegin();
             it != initialized_indices.rend(); ++it) {
            const auto &info = manager.getFromIndex(*it);
            if (info.cb_deinit != nullptr) {
                info.cb_deinit(staged_chunk->component_arrays[*it]
                                   ->atUnchecked(0));
            }
        }
        initialized_indices.clear();
    }

    void registerPublishedChunk(ECSComponentChunk &chunk,
                                std::size_t index) noexcept {
        core->archetype_to_chunks.find(archetype_key)->second.push_back(index);
        for (auto &[id, system] : core->systems) {
            (void)id;
            if ((chunk.getMask() & system.matching_mask) ==
                system.matching_mask) {
                system.matching_chunk_indices.push_back(index);
            }
        }
    }

    void unregisterPublishedChunk(ECSComponentChunk &chunk,
                                  std::size_t index) noexcept {
        auto &chunks = core->archetype_to_chunks.find(archetype_key)->second;
        assert(!chunks.empty() && chunks.back() == index);
        chunks.pop_back();
        for (auto &[id, system] : core->systems) {
            (void)id;
            if ((chunk.getMask() & system.matching_mask) ==
                system.matching_mask) {
                assert(!system.matching_chunk_indices.empty() &&
                       system.matching_chunk_indices.back() == index);
                system.matching_chunk_indices.pop_back();
            }
        }
    }

    void publishCreate() noexcept {
        published_new_chunk = !reusable_chunk_index.has_value();
        if (published_new_chunk) {
            published_chunk_index = core->chunks_storage.size();
            core->chunks_storage.emplace_back(std::move(*staged_chunk));
            staged_chunk.reset();
            registerPublishedChunk(core->chunks_storage[published_chunk_index],
                                   published_chunk_index);
            published_row = 0;
        } else {
            published_chunk_index = *reusable_chunk_index;
            auto &existing = core->chunks_storage[published_chunk_index];
            published_row = existing.size();
            for (const auto index : component_indices) {
                auto &target_array = *existing.component_arrays[index];
                auto &staged_array = *staged_chunk->component_arrays[index];
                target_array.relocate_one(
                    target_array.atUnchecked(published_row),
                    staged_array.atUnchecked(0));
                ++target_array.count;
                --staged_array.count;
            }
            ++existing.count;
            staged_chunk->count = 0;
            staged_chunk.reset();
        }
        auto &chunk = core->chunks_storage[published_chunk_index];

        if (reuses_id) {
            assert(!core->free_indices.empty() &&
                   core->free_indices.back() == entity.index);
            core->free_indices.pop_back();
        } else {
            assert(entity.index == core->id_table.size());
            core->id_table.emplace_back();
        }
        auto &entry = core->id_table[entity.index];
        assert(entry.generation == entity.generation && !entry.live);
        entry.ref = ECSCoreTemplatePublic::EntityRef{published_chunk_index,
                                                     published_row};
        entry.live = true;
        for (const auto index : chunk.indices) {
            chunk.updateVersion(index, core->global_tick);
        }
        published = true;
        mutation.reset();
    }

    void publishDestroy() noexcept {
        auto &source = core->chunks_storage[source_chunk_index];
        const auto source_last_row = source.size() - 1;
        const auto entity_component_index =
            GET_MODULE(ComponentInfoManager).getIndexFromComponentId(
                ComponentIdByType<EntityId>::value);
        backfilled_entity = *static_cast<EntityId *>(
            source.component_arrays[entity_component_index]->atUnchecked(
                source_last_row));
        for (auto it = source.indices.rbegin(); it != source.indices.rend();
             ++it) {
            const auto index = *it;
            auto &source_array = *source.component_arrays[index];
            auto &inverse_array = *staged_chunk->component_arrays[index];
            auto *inverse = inverse_array.atUnchecked(0);
            inverse_array.destroy_one(inverse);
            inverse_array.relocate_one(
                inverse, source_array.atUnchecked(source_row));
            if (source_row != source_last_row) {
                source_array.relocate_one(
                    source_array.atUnchecked(source_row),
                    source_array.atUnchecked(source_last_row));
            }
            --source_array.count;
        }
        --source.count;
        if (backfilled_entity != entity) {
            core->id_table[backfilled_entity.index].ref =
                ECSCoreTemplatePublic::EntityRef{source_chunk_index, source_row};
        }
        auto &entry = core->id_table[entity.index];
        entry.ref.reset();
        entry.live = false;
        if (entry.generation != std::numeric_limits<std::uint32_t>::max()) {
            ++entry.generation;
            core->free_indices.push_back(entity.index);
            free_index_published = true;
        }
        for (const auto index : source.indices) {
            source.updateVersion(index, core->global_tick);
        }
        published = true;
        mutation.reset();
    }

    void publish() noexcept {
        assert(!published && mutation != nullptr && staged_chunk != nullptr);
        if (kind == ECSEntityMutationKind::create) {
            publishCreate();
        } else {
            publishDestroy();
        }
    }

    void rollbackCreate() noexcept {
        assert(mutation == nullptr);
        ECSCoreTemplatePublic::MutationScope rollback_mutation{*core};
        if (published_new_chunk) {
            assert(published_chunk_index + 1 == core->chunks_storage.size());
        }
        auto &chunk = core->chunks_storage[published_chunk_index];
        assert(published_row + 1 == chunk.size());
        auto &entry = core->id_table[entity.index];
        entry.ref.reset();
        entry.live = false;
        if (reuses_id) {
            core->free_indices.push_back(entity.index);
        } else {
            core->id_table.pop_back();
        }
        for (auto it = initialized_indices.rbegin();
             it != initialized_indices.rend(); ++it) {
            const auto &info = GET_MODULE(ComponentInfoManager)
                                   .getFromIndex(*it);
            if (info.cb_deinit != nullptr) {
                info.cb_deinit(
                    chunk.component_arrays[*it]->atUnchecked(published_row));
            }
        }
        initialized_indices.clear();
        for (auto it = component_indices.rbegin();
             it != component_indices.rend(); ++it) {
            auto &array = *chunk.component_arrays[*it];
            array.destroy_one(array.atUnchecked(published_row));
            --array.count;
        }
        --chunk.count;
        for (const auto [index, version] : target_versions) {
            chunk.component_versions[index] = version;
        }
        if (published_new_chunk) {
            unregisterPublishedChunk(chunk, published_chunk_index);
            core->chunks_storage.pop_back();
        }
        published_new_chunk = false;
        published_chunk_index = std::numeric_limits<std::size_t>::max();
        published_row = 0;
        published = false;
        eraseEmptyArchetype();
    }

    void rollbackDestroy() noexcept {
        assert(mutation == nullptr);
        ECSCoreTemplatePublic::MutationScope rollback_mutation{*core};
        auto &source = core->chunks_storage[source_chunk_index];
        const auto source_last_row = source.size();
        for (auto it = source.indices.rbegin(); it != source.indices.rend();
             ++it) {
            const auto index = *it;
            auto &source_array = *source.component_arrays[index];
            auto &inverse_array = *staged_chunk->component_arrays[index];
            if (source_row != source_last_row) {
                source_array.relocate_one(
                    source_array.atUnchecked(source_last_row),
                    source_array.atUnchecked(source_row));
            }
            source_array.relocate_one(
                source_array.atUnchecked(source_row),
                inverse_array.atUnchecked(0));
            ++source_array.count;
            --inverse_array.count;
        }
        ++source.count;
        if (free_index_published) {
            assert(!core->free_indices.empty() &&
                   core->free_indices.back() == entity.index);
            core->free_indices.pop_back();
            free_index_published = false;
        }
        core->id_table[entity.index] = *old_id_entry;
        if (backfilled_entity != entity) {
            core->id_table[backfilled_entity.index].ref =
                ECSCoreTemplatePublic::EntityRef{source_chunk_index,
                                                 source_last_row};
        }
        for (const auto [index, version] : source_versions) {
            source.component_versions[index] = version;
        }
        staged_chunk.reset();
        published = false;
    }

    void rollback() noexcept {
        if (!published) {
            deinitializeStaged();
            staged_chunk.reset();
            eraseEmptyArchetype();
            mutation.reset();
            return;
        }
        if (kind == ECSEntityMutationKind::create) {
            rollbackCreate();
        } else {
            rollbackDestroy();
        }
    }

    void finish() noexcept {
        assert(published && mutation == nullptr);
        ECSCoreTemplatePublic::MutationScope finish_mutation{*core};
        if (kind == ECSEntityMutationKind::destroy) {
            auto &manager = GET_MODULE(ComponentInfoManager);
            for (auto it = component_indices.rbegin();
                 it != component_indices.rend(); ++it) {
                const auto &info = manager.getFromIndex(*it);
                if (info.cb_deinit != nullptr) {
                    info.cb_deinit(staged_chunk->component_arrays[*it]
                                       ->atUnchecked(0));
                }
            }
            staged_chunk.reset();
        }
    }
};

ECSEntityMutationToken::ECSEntityMutationToken() noexcept = default;
ECSEntityMutationToken::ECSEntityMutationToken(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
ECSEntityMutationToken::~ECSEntityMutationToken() { rollback(); }
ECSEntityMutationToken::ECSEntityMutationToken(
    ECSEntityMutationToken &&) noexcept = default;
ECSEntityMutationToken &ECSEntityMutationToken::operator=(
    ECSEntityMutationToken &&other) noexcept {
    if (this != &other) {
        rollback();
        state_ = std::move(other.state_);
    }
    return *this;
}
EntityId ECSEntityMutationToken::entity() const noexcept {
    return state_ == nullptr ? invalidEntityId : state_->entity;
}
ECSEntityMutationKind ECSEntityMutationToken::kind() const noexcept {
    return state_ == nullptr ? ECSEntityMutationKind::create : state_->kind;
}
bool ECSEntityMutationToken::published() const noexcept {
    return state_ != nullptr && state_->published;
}
void ECSEntityMutationToken::publish() noexcept {
    if (state_ != nullptr && !state_->published) state_->publish();
}
void ECSEntityMutationToken::rollback() noexcept {
    if (state_ == nullptr) return;
    state_->rollback();
    state_.reset();
}
void ECSEntityMutationToken::finish() noexcept {
    if (state_ == nullptr) return;
    if (!state_->published) {
        rollback();
        return;
    }
    state_->finish();
    state_.reset();
}

ECSEntityMutationToken ECSEntityMutation::prepareCreate(
    ECSCoreTemplatePublic &core, std::span<const ComponentId> component_ids,
    const Populate &populate) {
    auto state = std::make_unique<ECSEntityMutationToken::State>();
    state->core = &core;
    state->kind = ECSEntityMutationKind::create;
    state->mutation =
        std::make_unique<ECSCoreTemplatePublic::MutationScope>(core);
    try {
        if (component_ids.size() >= MAX_COMPONENTS) {
            throw std::length_error("ECS entity has too many component types");
        }
        state->component_ids.reserve(component_ids.size() + 1);
        state->component_ids.push_back(ComponentIdByType<EntityId>::value);
        state->component_ids.insert(state->component_ids.end(),
                                    component_ids.begin(), component_ids.end());
        state->archetype_key = state->component_ids;
        std::sort(state->archetype_key.begin(), state->archetype_key.end());
        if (std::adjacent_find(state->archetype_key.begin(),
                               state->archetype_key.end()) !=
            state->archetype_key.end()) {
            throw std::invalid_argument(
                "ECS entity component list contains a duplicate");
        }
        auto &manager = GET_MODULE(ComponentInfoManager);
        state->component_indices.reserve(state->component_ids.size());
        for (const auto id : state->component_ids) {
            const auto index = manager.getIndexFromComponentId(id);
            (void)manager.getFromIndex(index);
            state->component_indices.push_back(index);
        }

        state->reuses_id = !core.free_indices.empty();
        if (state->reuses_id) {
            const auto index = core.free_indices.back();
            state->entity = EntityId{index, core.id_table[index].generation};
        } else {
            ECSCoreTemplatePublic::validateFreshIndexCapacityForTesting(
                core.id_table.size());
            state->entity = EntityId{
                static_cast<std::uint32_t>(core.id_table.size()), 0};
            core.id_table.reserve(core.id_table.size() + 1);
        }
        core.free_indices.reserve(core.free_indices.size() + 1);

        state->staged_chunk = std::make_unique<ECSComponentChunk>(
            state->component_indices, state->component_ids);
        std::vector<void *> pointers(state->component_indices.size());
        state->staged_chunk->allocate(state->component_indices, pointers, 1);
        *static_cast<EntityId *>(pointers.front()) = state->entity;
        if (populate) {
            populate(std::span<void *>{pointers}.subspan(1));
        }
        state->initialized_indices.reserve(state->component_indices.size());
        for (const auto index : state->component_indices) {
            const auto &info = manager.getFromIndex(index);
            if (info.cb_init != nullptr) {
                info.cb_init(state->staged_chunk->component_arrays[index]
                                 ->atUnchecked(0));
                state->initialized_indices.push_back(index);
            }
        }

        auto [archetype, inserted] =
            core.archetype_to_chunks.try_emplace(state->archetype_key);
        state->inserted_archetype = inserted;
        for (auto it = archetype->second.rbegin();
             it != archetype->second.rend(); ++it) {
            auto &candidate = core.chunks_storage[*it];
            if (candidate.remainingCapacity() == 0) continue;
            state->reusable_chunk_index = *it;
            state->target_versions.reserve(candidate.indices.size());
            for (const auto index : candidate.indices) {
                state->target_versions.emplace_back(
                    index, candidate.getVersion(index));
            }
            break;
        }
        if (!state->reusable_chunk_index) {
            core.chunks_storage.reserve(core.chunks_storage.size() + 1);
            archetype->second.reserve(archetype->second.size() + 1);
            const auto mask = state->staged_chunk->getMask();
            for (auto &[id, system] : core.systems) {
                (void)id;
                if ((mask & system.matching_mask) == system.matching_mask) {
                    system.matching_chunk_indices.reserve(
                        system.matching_chunk_indices.size() + 1);
                }
            }
        }
        return ECSEntityMutationToken{std::move(state)};
    } catch (...) {
        state->rollback();
        throw;
    }
}

ECSEntityMutationToken ECSEntityMutation::prepareDestroy(
    ECSCoreTemplatePublic &core, EntityId entity) {
    auto state = std::make_unique<ECSEntityMutationToken::State>();
    state->core = &core;
    state->kind = ECSEntityMutationKind::destroy;
    state->entity = entity;
    state->mutation =
        std::make_unique<ECSCoreTemplatePublic::MutationScope>(core);
    try {
        const auto ref = core.resolve(entity);
        if (!ref) {
            throw std::runtime_error("ECS entity is not live: " +
                                     toString(entity));
        }
        state->source_chunk_index = ref->chunk_index;
        state->source_row = ref->array_index;
        auto &source = core.chunks_storage[ref->chunk_index];
        state->component_ids.assign(source.component_ids.begin(),
                                    source.component_ids.end());
        state->component_indices.assign(source.indices.begin(),
                                        source.indices.end());
        state->staged_chunk = std::make_unique<ECSComponentChunk>(
            state->component_indices, state->component_ids);
        std::vector<void *> pointers(state->component_indices.size());
        state->staged_chunk->allocate(state->component_indices, pointers, 1);
        state->source_versions.reserve(source.indices.size());
        for (const auto index : source.indices) {
            state->source_versions.emplace_back(index,
                                                source.getVersion(index));
        }
        state->old_id_entry = core.id_table[entity.index];
        core.free_indices.reserve(core.free_indices.size() + 1);
        return ECSEntityMutationToken{std::move(state)};
    } catch (...) {
        state->rollback();
        throw;
    }
}

} // namespace Pelican
