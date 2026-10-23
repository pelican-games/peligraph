#include "modelview.hpp"

#include "../../asset/model.hpp"
#include "../../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

void SimpleModelViewComponent::init() { dirty = !model_instance_id.has_value() && !model_name.empty(); }
void SimpleModelViewComponent::deinit() noexcept {
    if (model_instance_id)
        GET_MODULE(PolygonInstanceContainer).removeModelInstance(*model_instance_id);
}

std::shared_ptr<const InstanceMaterialOverrides>
instanceMaterialOverrides(const std::optional<List<ModelMaterialOverride>> &materials) {
    if (!materials || materials->rows.empty()) return nullptr;
    auto overrides = std::make_shared<InstanceMaterialOverrides>();
    overrides->reserve(materials->rows.size());
    for (const auto &row : materials->rows) {
        InstanceMaterialOverride entry{.primitive = std::nullopt, .material = row.value.material};
        if (row.value.mesh && row.value.primitive)
            entry.primitive = std::pair{*row.value.mesh, *row.value.primitive};
        overrides->push_back(std::move(entry));
    }
    return overrides;
}

} // namespace Pelican
