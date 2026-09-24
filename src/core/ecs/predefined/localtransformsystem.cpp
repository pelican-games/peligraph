#include "localtransformsystem.hpp"

#include "../core.hpp"
#include "../../geomhelper/geomhelper.hpp"
#include "../../loader/componentcodec.hpp"

#include <stdexcept>
#include <unordered_map>

namespace Pelican {

namespace {

TransformComponent composeFromLocal(const TransformComponent *parent_world,
                                    const LocalTransformComponent &local) noexcept {
    return composeWorldTransform(parent_world,
                                 TransformComponent{.pos = to_glm(local.pos),
                                                    .rotation = to_glm(local.rotation),
                                                    .scale = to_glm(local.scale)});
}

void updateTransformRecursively(EntityId entity_id, TransformComponent &world,
                                LocalTransformComponent &local,
                                ECSCoreTemplatePublic &ecs,
                                std::unordered_map<EntityId, uint8_t> &state) {
    const auto current_state = state[entity_id];
    if (current_state == 2) {
        return;
    }
    if (current_state == 1) {
        throw std::runtime_error("LocalTransform parent cycle includes entity " + toString(entity_id));
    }
    state[entity_id] = 1;

    const TransformComponent *parent_world = nullptr;
    if (local.parent != invalidEntityId) {
        if (auto *parent = ecs.tryComponent<TransformComponent>(local.parent)) {
            if (auto *parent_local = ecs.tryComponent<LocalTransformComponent>(local.parent)) {
                updateTransformRecursively(local.parent, *parent, *parent_local, ecs, state);
            }
            parent_world = parent;
        }
    }

    // GP-1: every scene object with a transform now carries a localtransform,
    // so this pass decides the world transform of roots too. It composes
    // through the same function scene load uses, so a root recomputed here
    // lands on the exact world its load produced (a near-unit authored
    // rotation included) instead of drifting on the first frame.
    world = composeFromLocal(parent_world, local);
    state[entity_id] = 2;
}

} // namespace

void LocalTransformSystem::prepareEcsWorkerDependencies(bool has_matching_chunks) {
    if (has_matching_chunks) ecs = &GET_MODULE(ECSCore).getTemplatePublicModule();
}

void LocalTransformSystem::process(Query chunks) {
    if (chunks.empty()) return;
    if (ecs == nullptr)
        throw std::logic_error("LocalTransformSystem dependencies were not prepared on the ECS owner thread");
    std::unordered_map<EntityId, uint8_t> state;
    for (auto &chunk : chunks) {
        auto entity_ids = std::get<EntityId *>(chunk.components);
        auto transforms = std::get<TransformComponent *>(chunk.components);
        auto localtransforms = std::get<LocalTransformComponent *>(chunk.components);

        for (size_t i = 0; i < chunk.count; ++i) {
            // A root needs no parent walk and no visit bookkeeping: most scene
            // objects are roots now that every one carries a localtransform,
            // and recomputing one again as some child's parent is idempotent.
            if (localtransforms[i].parent == invalidEntityId) {
                transforms[i] = composeFromLocal(nullptr, localtransforms[i]);
                continue;
            }
            updateTransformRecursively(entity_ids[i], transforms[i], localtransforms[i], *ecs, state);
        }
    }
}

} // namespace Pelican
