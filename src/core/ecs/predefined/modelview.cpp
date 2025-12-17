#include "modelview.hpp"

#include "../../asset/model.hpp"
#include "../../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

void SimpleModelViewComponent::init() {
    auto &pic = GET_MODULE(PolygonInstanceContainer);
    auto &asset_con = GET_MODULE(ModelAssetContainer);
    model_instance_id = pic.placeModelInstance(asset_con.getModelTemplateByName(model_name));
}
void SimpleModelViewComponent::deinit() { GET_MODULE(PolygonInstanceContainer).removeModelInstance(model_instance_id); }

} // namespace Pelican
