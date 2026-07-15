#include "modelviewupdatesystem.hpp"

#include "../../asset/model.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include <stdexcept>

namespace Pelican {

void SimpleModelViewUpdateSystem::prepareEcsWorkerDependencies(DependencyQuery chunks) {
    if (chunks.empty()) return;
    models = &GET_MODULE(ModelAssetContainer);
    instances = &GET_MODULE(PolygonInstanceContainer);
    for (const auto &chunk : chunks) {
        const auto model_views = std::get<SimpleModelViewComponent *>(chunk.components);
        for (std::size_t i = 0; i < chunk.count; ++i) {
            if (model_views[i].dirty)
                (void)models->getModelTemplateByName(model_views[i].model_name);
        }
    }
}

void SimpleModelViewUpdateSystem::process(QueryComponents components, size_t count) {
    if (count == 0) return;
    if (models == nullptr || instances == nullptr) {
        throw std::logic_error(
            "SimpleModelViewUpdateSystem dependencies were not prepared on the ECS owner thread");
    }
    auto m = std::get<SimpleModelViewComponent *>(components);

    for (int i = 0; i < count; i++) {
        if (!m[i].dirty)
            continue;
        if (m[i].model_instance_id.has_value())
            instances->removeModelInstance(m[i].model_instance_id.value());

        const auto &model_name = m[i].model_name;
        auto &model_template = models->getModelTemplateByName(model_name);
        m[i].model_instance_id = instances->placeModelInstance(model_template);
        m[i].dirty = false;
    }
}

} // namespace Pelican
