#include "polygoninstancecontainer.hpp"
#include "../vkcore/core.hpp"

namespace Pelican {

static BufferWrapper createIndirectBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(PolygonInstance) * num,
                           vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

PolygonInstanceContainer::PolygonInstanceContainer(DependencyContainer &_con)
    : con{_con}, indirect_buf{createIndirectBuf(con.get<VulkanManageCore>(), 1024)} {}

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
void PolygonInstanceContainer::triggerUpdate() {
    // clear previous frame
    draw_calls.clear();

    if (instances.empty())
        return;

    // prepare indirect buffer
    std::sort(instances.begin(), instances.end(),
              [](const PolygonInstance &p, const PolygonInstance &q) { return p.material.value < q.material.value; });
    con.get<VulkanManageCore>().writeBuf(indirect_buf, instances.data(), 0, sizeof(PolygonInstance) * instances.size());

    // prepare drawindirect information
    DrawIndirectInfo draw_call{.stride = sizeof(PolygonInstance)};
    size_t prev_offset_index;

    prev_offset_index = 0;
    draw_call.material = instances[0].material;
    draw_call.offset = prev_offset_index * sizeof(PolygonInstance);
    for (int i = 1; i < instances.size(); i++) {
        if (instances[i].material.value != instances[i - 1].material.value) {
            draw_call.draw_count = i - prev_offset_index;
            draw_calls.push_back(draw_call);

            prev_offset_index = i;
            draw_call.material = instances[i].material;
            draw_call.offset = prev_offset_index * sizeof(PolygonInstance);
        }
    }
    draw_call.draw_count = instances.size() - prev_offset_index;
    draw_calls.push_back(draw_call);
}
const std::vector<PolygonInstance> &PolygonInstanceContainer::getPolygons() const { return instances; }

const BufferWrapper &PolygonInstanceContainer::getIndirectBuf() const { return indirect_buf; }
const std::vector<DrawIndirectInfo> &PolygonInstanceContainer::getDrawCalls() const { return draw_calls; }

} // namespace Pelican
