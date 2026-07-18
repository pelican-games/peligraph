#include "archetypemigration.hpp"

#include "componentinfo.hpp"
#include <details/ecs/chunk.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/coretemplate.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {

void ECSArchetypeMigration::add(ECSCoreTemplatePublic &core, EntityId entity,
                                ComponentId component, const Populate &populate,
                                Adapters adapters) {
    migrate(core, entity, component, ECSArchetypeMigrationKind::add, populate, adapters);
}

void ECSArchetypeMigration::remove(ECSCoreTemplatePublic &core, EntityId entity,
                                   ComponentId component, Adapters adapters) {
    migrate(core, entity, component, ECSArchetypeMigrationKind::remove, {}, adapters);
}

void ECSArchetypeMigration::migrate(ECSCoreTemplatePublic &core, EntityId entity,
                                    ComponentId component, ECSArchetypeMigrationKind kind,
                                    const Populate &populate, Adapters adapters) {
    ECSCoreTemplatePublic::MutationScope mutation{core};

    const auto source_ref_value = core.resolve(entity);
    if (!source_ref_value.has_value()) {
        throw std::runtime_error("ECS entity is not live: " + toString(entity));
    }
    const auto source_ref = *source_ref_value;
    const auto source_chunk_index = source_ref.chunk_index;
    const auto source_row = source_ref.array_index;
    auto &manager = GET_MODULE(ComponentInfoManager);

    size_t changed_component_index = 0;
    try {
        changed_component_index = manager.getIndexFromComponentId(component);
        const auto &info = manager.getFromIndex(changed_component_index);
        if (info.id != component || info.size == 0 || info.alignment == 0 ||
            info.cb_construct == nullptr || info.cb_destroy == nullptr ||
            info.cb_relocate == nullptr) {
            throw std::out_of_range("unregistered component");
        }
    } catch (const std::out_of_range &) {
        throw std::invalid_argument("ECS component " + std::to_string(component) +
                                    " is not registered");
    }

    const auto entity_component_index = manager.getIndexFromComponentId(
        ComponentIdByType<EntityId>::value);
    auto &source_chunk = core.chunks_storage[source_chunk_index];
    const bool source_has_component = source_chunk.has(changed_component_index);
    if (kind == ECSArchetypeMigrationKind::add && source_has_component) {
        throw std::invalid_argument("ECS component '" + manager.get(component).name +
                                    "' already exists on entity " + toString(entity));
    }
    if (kind == ECSArchetypeMigrationKind::remove && component == ComponentIdByType<EntityId>::value) {
        throw std::invalid_argument("ECS EntityId component cannot be removed");
    }
    if (kind == ECSArchetypeMigrationKind::remove && !source_has_component) {
        throw std::invalid_argument("ECS component '" + manager.get(component).name +
                                    "' does not exist on entity " + toString(entity));
    }

    std::unordered_set<ECSArchetypeMigrationAdapter *> unique_adapters;
    unique_adapters.reserve(adapters.size());
    for (auto *adapter : adapters) {
        if (adapter == nullptr) {
            throw std::invalid_argument("ECS archetype migration adapter cannot be null");
        }
        if (!unique_adapters.insert(adapter).second) {
            throw std::invalid_argument("ECS archetype migration adapter is duplicated");
        }
    }
    std::vector<ECSArchetypeMigrationAdapter *> attempted_adapters;
    attempted_adapters.reserve(adapters.size());

    std::vector<ComponentId> target_component_ids{
        source_chunk.getComponentList().begin(), source_chunk.getComponentList().end()};
    if (kind == ECSArchetypeMigrationKind::add) {
        if (target_component_ids.size() >= MAX_COMPONENTS) {
            throw std::length_error("ECS entity has too many component types");
        }
        target_component_ids.push_back(component);
    } else {
        target_component_ids.erase(
            std::find(target_component_ids.begin(), target_component_ids.end(), component));
    }

    std::vector<size_t> target_component_indices;
    target_component_indices.reserve(target_component_ids.size());
    for (const auto target_component : target_component_ids) {
        target_component_indices.push_back(manager.getIndexFromComponentId(target_component));
    }
    auto target_key = target_component_ids;
    std::sort(target_key.begin(), target_key.end());

    ECSCoreTemplatePublic::ChunkIndex target_chunk_index =
        std::numeric_limits<ECSCoreTemplatePublic::ChunkIndex>::max();
    size_t target_row = 0;
    bool created_target_chunk = false;
    bool target_allocated = false;
    bool added_initialized = false;
    void *staged_component = nullptr;
    void *removed_live_component = nullptr;

    try {
        const auto found = core.archetype_to_chunks.find(target_key);
        if (found != core.archetype_to_chunks.end()) {
            for (const auto candidate : found->second) {
                if (core.chunks_storage[candidate].remainingCapacity() != 0) {
                    target_chunk_index = candidate;
                    break;
                }
            }
        }
        if (target_chunk_index == std::numeric_limits<ECSCoreTemplatePublic::ChunkIndex>::max()) {
            target_chunk_index = core.chunks_storage.size();
            core.chunks_storage.emplace_back(target_component_indices, target_component_ids);
            created_target_chunk = true;
            core.archetype_to_chunks[target_key].push_back(target_chunk_index);
            core.updateSystemChunkCache(target_chunk_index);
        }

        auto &target_chunk = core.chunks_storage[target_chunk_index];
        // Appending a new target chunk may have reallocated chunks_storage.
        auto &live_source_chunk = core.chunks_storage[source_chunk_index];
        target_row = target_chunk.size();
        std::vector<void *> target_component_ptrs(target_component_indices.size());
        target_chunk.allocate(target_component_indices, target_component_ptrs, 1);
        target_allocated = true;

        if (kind == ECSArchetypeMigrationKind::add) {
            const auto target_position = static_cast<size_t>(
                std::find(target_component_indices.begin(), target_component_indices.end(),
                          changed_component_index) -
                target_component_indices.begin());
            staged_component = target_component_ptrs[target_position];
            if (populate) {
                populate(staged_component);
            }
            const auto &added_info = manager.getFromIndex(changed_component_index);
            if (added_info.cb_init != nullptr) {
                added_info.cb_init(staged_component);
                added_initialized = true;
            }
        } else {
            removed_live_component = live_source_chunk.at(changed_component_index, source_row);
        }

        ECSArchetypeMigrationPrepareContext prepare_context{
            .core = core,
            .entity = entity,
            .component = component,
            .kind = kind,
            .live_component = removed_live_component,
            .staged_component = staged_component,
        };
        for (auto *adapter : adapters) {
            attempted_adapters.push_back(adapter);
            adapter->prepare(prepare_context);
        }

        // No operation below this line may fail. Retained move-only values are relocated only
        // after every constructor, population callback, init, allocation, and adapter prepare.
        const auto source_last_row = live_source_chunk.size() - 1;
        const auto backfilled_entity = *static_cast<EntityId *>(
            live_source_chunk.component_arrays[entity_component_index]->atUnchecked(source_last_row));

        if (kind == ECSArchetypeMigrationKind::remove) {
            live_source_chunk.component_arrays[changed_component_index]->removeAt(source_row);
        }

        for (auto it = live_source_chunk.indices.rbegin(); it != live_source_chunk.indices.rend(); ++it) {
            const auto component_index = *it;
            if (kind == ECSArchetypeMigrationKind::remove &&
                component_index == changed_component_index) {
                continue;
            }
            auto &source_array = *live_source_chunk.component_arrays[component_index];
            auto &target_array = *target_chunk.component_arrays[component_index];
            auto *target_ptr = target_array.atUnchecked(target_row);
            auto *source_ptr = source_array.atUnchecked(source_row);
            target_array.destroy_one(target_ptr);
            source_array.relocate_one(target_ptr, source_ptr);
            if (source_row != source_last_row) {
                source_array.relocate_one(source_ptr, source_array.atUnchecked(source_last_row));
            }
            --source_array.count;
        }
        --live_source_chunk.count;

        core.id_table[entity.index].ref =
            ECSCoreTemplatePublic::EntityRef{target_chunk_index, target_row};
        if (backfilled_entity != entity) {
            core.id_table[backfilled_entity.index].ref =
                ECSCoreTemplatePublic::EntityRef{source_chunk_index, source_row};
        }
        for (const auto component_index : target_chunk.indices) {
            target_chunk.updateVersion(component_index, core.global_tick);
        }
        for (const auto component_index : live_source_chunk.indices) {
            live_source_chunk.updateVersion(component_index, core.global_tick);
        }

        const ECSArchetypeMigrationPublishContext publish_context{
            .core = core,
            .entity = entity,
            .component = component,
            .kind = kind,
            .live_component = kind == ECSArchetypeMigrationKind::add ? staged_component : nullptr,
        };
        for (auto *adapter : adapters) {
            adapter->publish(publish_context);
        }
    } catch (...) {
        ECSArchetypeMigrationPrepareContext rollback_context{
            .core = core,
            .entity = entity,
            .component = component,
            .kind = kind,
            .live_component = removed_live_component,
            .staged_component = staged_component,
        };
        for (auto it = attempted_adapters.rbegin(); it != attempted_adapters.rend(); ++it) {
            (*it)->rollback(rollback_context);
        }
        if (added_initialized) {
            const auto &added_info = manager.getFromIndex(changed_component_index);
            if (added_info.cb_deinit != nullptr) {
                added_info.cb_deinit(staged_component);
            }
        }
        if (target_allocated) {
            core.chunks_storage[target_chunk_index].rollbackTail(1);
        }
        if (created_target_chunk) {
            core.chunks_storage.pop_back();
            core.rebuildChunkCaches();
        }
        throw;
    }
}

} // namespace Pelican
