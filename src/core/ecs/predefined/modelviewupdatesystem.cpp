#include "modelviewupdatesystem.hpp"

#include "../../asset/model.hpp"
#include "../core.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../../shader/raygrouplayout.hpp"

#include <stdexcept>

namespace Pelican {

void SimpleModelViewUpdateSystem::prepareEcsWorkerDependencies(DependencyQuery chunks) {
    if (chunks.empty()) return;
    models = &GET_MODULE(ModelAssetContainer);
    instances = &GET_MODULE(PolygonInstanceContainer);
    prepared_ray_instances.clear();
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    const auto *ray_layout =
        FastModuleContainer::tryGet<RayLayoutRuntimeState>();
    const bool ray_instance_enabled =
        ray_layout != nullptr && ray_layout->builtinRayInstanceEnabled();
    for (const auto &chunk : chunks) {
        const auto entities = std::get<EntityId *>(chunk.components);
        const auto model_views =
            std::get<SimpleModelViewComponent *>(chunk.components);
        for (std::size_t i = 0; i < chunk.count; ++i) {
            if (model_views[i].dirty)
                (void)models->getModelTemplateByName(model_views[i].model_name);
            std::optional<RayInstanceComponent> declaration;
            if (ray_instance_enabled) {
                if (const auto *component =
                        ecs.tryComponent<RayInstanceComponent>(entities[i])) {
                    declaration = *component;
                }
            }
            prepared_ray_instances.emplace(entities[i],
                                           std::move(declaration));
        }
    }
}

void SimpleModelViewUpdateSystem::process(QueryComponents components, size_t count) {
    if (count == 0) return;
    if (models == nullptr || instances == nullptr) {
        throw std::logic_error(
            "SimpleModelViewUpdateSystem dependencies were not prepared on the ECS owner thread");
    }
    const auto entities = std::get<EntityId *>(components);
    auto m = std::get<SimpleModelViewComponent *>(components);

    for (int i = 0; i < count; i++) {
        if (m[i].dirty) {
            if (m[i].model_instance_id.has_value())
                instances->removeModelInstance(m[i].model_instance_id.value());

            const auto &model_name = m[i].model_name;
            auto &model_template = models->getModelTemplateByName(model_name);
            m[i].model_instance_id = instances->placeModelInstance(
                model_template, instanceMaterialOverrides(m[i].materials));
            m[i].dirty = false;
        }
        if (m[i].model_instance_id) {
            const auto declaration = prepared_ray_instances.find(entities[i]);
            if (declaration == prepared_ray_instances.end()) {
                throw std::logic_error(
                    "SimpleModelViewUpdateSystem ray declaration was not prepared");
            }
            instances->setRayInstanceDeclaration(
                *m[i].model_instance_id, m[i].authoring_object_id,
                declaration->second);
        }
    }
}

} // namespace Pelican
