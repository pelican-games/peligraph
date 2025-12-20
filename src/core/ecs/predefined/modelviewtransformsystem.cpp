#include "modelviewtransoformsystem.hpp"

#include "../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

void SimpleModelViewTransformSystem::process(QueryComponents components, size_t count) {
    auto transforms = std::get<TransformComponent *>(components);
    auto models = std::get<SimpleModelViewComponent *>(components);

    auto &pic = GET_MODULE(PolygonInstanceContainer);
    for (int i = 0; i < count; i++) {
        pic.setTrs(models[i].model_instance_id, transforms[i].pos, transforms[i].rotation, transforms[i].scale);
    }
}

} // namespace Pelican
