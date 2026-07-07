#include "gameobjects.hpp"
#include "components/predefined.hpp"
#include "../ecs/componentinfo.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../geomhelper/geomhelper.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

GameObjectId GameObjects::alloc(const ComponentId *ids, void **ptrs, uint32_t components_count) {
    return GET_MODULE(ECSCore).allocateEntity(std::span{ids, components_count}, std::span{ptrs, components_count}, 1);
}
void GameObjects::commit(const ComponentId *ids, void *const *ptrs, uint32_t components_count) {
    for (int i = 0; i < components_count; i++) {
        GET_MODULE(ComponentInfoManager).initComponent(ids[i], ptrs[i]);
    }
}

void GameObjects::remove(GameObjectId id) { GET_MODULE(ECSCore).remove(id); }

LocalTransformComponent GameObjects::localTransform(GameObjectId id) {
    return GET_MODULE(ECSCore).getTemplatePublicModule().component<LocalTransformComponent>(id);
}

void GameObjects::setLocalTransform(GameObjectId id, const LocalTransformComponent &transform) {
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    ecs.setComponent<LocalTransformComponent>(id, transform);

    auto *engine_transform = ecs.tryComponent<TransformComponent>(id);
    if (engine_transform != nullptr) {
        engine_transform->pos = to_glm(transform.pos);
        engine_transform->rotation = to_glm(transform.rotation);
        engine_transform->scale = to_glm(transform.scale);
        ecs.markComponentChanged(id, ComponentIdByType<TransformComponent>::value);
    }

    auto *model_view = ecs.tryComponent<SimpleModelViewComponent>(id);
    if (model_view != nullptr && model_view->model_instance_id.has_value()) {
        GET_MODULE(PolygonInstanceContainer)
            .setTrs(model_view->model_instance_id.value(), to_glm(transform.pos), to_glm(transform.rotation),
                    to_glm(transform.scale));
    }
}

} // namespace Pelican
