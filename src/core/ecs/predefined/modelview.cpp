#include "modelview.hpp"

#include "../../asset/model.hpp"
#include "../../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

void SimpleModelViewComponent::init() { dirty = !model_instance_id.has_value() && !model_name.empty(); }
void SimpleModelViewComponent::deinit() noexcept {
    if (model_instance_id)
        GET_MODULE(PolygonInstanceContainer).removeModelInstance(*model_instance_id);
}

} // namespace Pelican
