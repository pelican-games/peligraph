#include "localtransformsystem.hpp"

#include "../core.hpp"
#include "../../geomhelper/geomhelper.hpp"

#include <stdexcept>
#include <unordered_map>

namespace Pelican {

namespace {

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

    glm::vec3 parent_pos{0.0f};
    glm::quat parent_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 parent_scale{1.0f};
    if (local.parent != invalidEntityId) {
        if (auto *parent_world = ecs.tryComponent<TransformComponent>(local.parent)) {
            if (auto *parent_local = ecs.tryComponent<LocalTransformComponent>(local.parent)) {
                updateTransformRecursively(local.parent, *parent_world, *parent_local, ecs, state);
            }
            parent_pos = parent_world->pos;
            parent_rotation = parent_world->rotation;
            parent_scale = parent_world->scale;
        }
    }

    world.scale = parent_scale * to_glm(local.scale);
    world.rotation = parent_rotation * to_glm(local.rotation);
    world.pos = parent_pos + parent_rotation * (parent_scale * to_glm(local.pos));
    state[entity_id] = 2;
}

} // namespace

void LocalTransformSystem::process(Query chunks) {
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    std::unordered_map<EntityId, uint8_t> state;
    for (auto &chunk : chunks) {
        auto entity_ids = std::get<EntityId *>(chunk.components);
        auto transforms = std::get<TransformComponent *>(chunk.components);
        auto localtransforms = std::get<LocalTransformComponent *>(chunk.components);

        for (size_t i = 0; i < chunk.count; ++i) {
            updateTransformRecursively(entity_ids[i], transforms[i], localtransforms[i], ecs, state);
        }
    }
}

} // namespace Pelican
