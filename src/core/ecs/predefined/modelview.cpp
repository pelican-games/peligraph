#include "modelview.hpp"

#include "../../asset/model.hpp"
#include "../../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

void SimpleModelViewComponent::init() {}
void SimpleModelViewComponent::deinit() noexcept {
    if (model_instance_id)
        GET_MODULE(PolygonInstanceContainer).removeModelInstance(*model_instance_id);
}

} // namespace Pelican
