#include "gameobjects.hpp"

#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../geomhelper/geomhelper.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "components/predefined.hpp"

namespace Pelican {

GameObjectId GameObjects::create(std::span<const ComponentId> ids,
                                 const std::function<void(std::span<void *>)> &populate) {
    return GET_MODULE(ECSCore).createEntity(ids, populate);
}

GameObjectId GameObjects::createWithComponents(
    std::span<const ComponentId> ids, const std::function<void(std::span<void *>)> &populate) {
    return create(ids, populate);
}

bool GameObjects::remove(GameObjectId id) {
    return GET_MODULE(ECSCore).remove(id);
}

void GameObjects::removeAll() {
    GET_MODULE(ECSCore).clearEntities();
}

size_t GameObjects::liveCountForTesting() {
    return GET_MODULE(ECSCore).getTemplatePublicModule().liveCount();
}

LocalTransformComponent GameObjects::localTransform(GameObjectId id) {
    return GET_MODULE(ECSCore).getTemplatePublicModule().component<LocalTransformComponent>(id);
}

bool GameObjects::setLocalTransform(GameObjectId id, const LocalTransformComponent &transform) {
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    if (!ecs.setComponent<LocalTransformComponent>(id, transform)) {
        return false;
    }

    if (auto *engine_transform = ecs.tryComponent<TransformComponent>(id)) {
        engine_transform->pos = to_glm(transform.pos);
        engine_transform->rotation = to_glm(transform.rotation);
        engine_transform->scale = to_glm(transform.scale);
        (void)ecs.markComponentChanged(id, ComponentIdByType<TransformComponent>::value);
    }

    if (auto *model_view = ecs.tryComponent<SimpleModelViewComponent>(id);
        model_view != nullptr && model_view->model_instance_id.has_value()) {
        GET_MODULE(PolygonInstanceContainer)
            .setTrs(model_view->model_instance_id.value(), to_glm(transform.pos), to_glm(transform.rotation),
                    to_glm(transform.scale));
    }
    return true;
}

} // namespace Pelican
