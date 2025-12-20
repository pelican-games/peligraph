#include "modelviewupdatesystem.hpp"

#include "../../asset/model.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

void SimpleModelViewUpdateSystem::process(QueryComponents components, size_t count) {
    auto mu = std::get<SimpleModelViewUpdateComponent *>(components);
    auto m = std::get<SimpleModelViewComponent *>(components);

    for (int i = 0; i < count; i++) {
        if (!mu[i].dirty)
            continue;
        if (m[i].model_instance_id.has_value())
            GET_MODULE(PolygonInstanceContainer).removeModelInstance(m[i].model_instance_id.value());

        auto &model_template = GET_MODULE(ModelAssetContainer).getModelTemplateByName(mu[i].model_name);
        m[i].model_instance_id = GET_MODULE(PolygonInstanceContainer).placeModelInstance(model_template);
        mu[i].dirty = false;
    }
}

} // namespace Pelican
