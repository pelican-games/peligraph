#include "polygoninstancecontainer.hpp"

namespace Pelican {

PolygonInstanceContainer::PolygonInstanceContainer(DependencyContainer &_con) : con{_con} {}

ModelInstanceId PolygonInstanceContainer::placeModelInstance(ModelTemplate &model) {
    auto &instance_container = con.get<PolygonInstanceContainer>();
    for (const auto &material : model.material_primitives) {
        for (const auto &primitive : material.primitives) {
            PolygonInstance instance{
                .command =
                    vk::DrawIndexedIndirectCommand{
                        primitive.index_count,
                        1,
                        primitive.index_offset,
                        primitive.vert_offset,
                        0,
                    },
                .material = material.material,
            };
            instances.push_back(instance);
        }
    }
    return {}; // TODO
}
void PolygonInstanceContainer::removeModelInstance(ModelInstanceId id) {}
void PolygonInstanceContainer::triggerOptimize() {}
const std::vector<PolygonInstance> &PolygonInstanceContainer::getPolygons() const { return instances; }

} // namespace Pelican
