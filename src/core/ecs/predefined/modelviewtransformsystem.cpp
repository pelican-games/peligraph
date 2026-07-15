#include "modelviewtransoformsystem.hpp"

#include "../renderer/polygoninstancecontainer.hpp"

#include <stdexcept>

namespace Pelican {

void SimpleModelViewTransformSystem::prepareEcsWorkerDependencies(bool has_matching_chunks) {
    if (has_matching_chunks) instances = &GET_MODULE(PolygonInstanceContainer);
}

void SimpleModelViewTransformSystem::process(QueryComponents components, size_t count) {
    if (count == 0) return;
    if (instances == nullptr) {
        throw std::logic_error(
            "SimpleModelViewTransformSystem dependencies were not prepared on the ECS owner thread");
    }
    auto transforms = std::get<TransformComponent *>(components);
    auto models = std::get<SimpleModelViewComponent *>(components);

    for (int i = 0; i < count; i++) {
        if (models[i].model_instance_id.has_value()) {
            instances->setTrs(models[i].model_instance_id.value(), transforms[i].pos,
                              transforms[i].rotation, transforms[i].scale);
        }
    }
}

} // namespace Pelican
