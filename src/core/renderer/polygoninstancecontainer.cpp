#include "polygoninstancecontainer.hpp"
#include "../vkcore/core.hpp"

namespace Pelican {

static BufferWrapper createIndirectBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(RenderCommand) * num,
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
            RenderCommand instance{
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
            render_commands.push_back(instance);
        }
    }
    return {}; // TODO
}
void PolygonInstanceContainer::removeModelInstance(ModelInstanceId id) {}
void PolygonInstanceContainer::triggerUpdate() {
    // clear previous frame
    draw_calls.clear();

    if (render_commands.empty())
        return;

    // prepare indirect buffer
    std::sort(render_commands.begin(), render_commands.end(),
              [](const RenderCommand &p, const RenderCommand &q) { return p.material.value < q.material.value; });
    con.get<VulkanManageCore>().writeBuf(indirect_buf, render_commands.data(), 0, sizeof(RenderCommand) * render_commands.size());

    // prepare drawindirect information
    DrawIndirectInfo draw_call{.stride = sizeof(RenderCommand)};
    size_t prev_offset_index;

    prev_offset_index = 0;
    draw_call.material = render_commands[0].material;
    draw_call.offset = prev_offset_index * sizeof(RenderCommand);
    for (int i = 1; i < render_commands.size(); i++) {
        if (render_commands[i].material.value != render_commands[i - 1].material.value) {
            draw_call.draw_count = i - prev_offset_index;
            draw_calls.push_back(draw_call);

            prev_offset_index = i;
            draw_call.material = render_commands[i].material;
            draw_call.offset = prev_offset_index * sizeof(RenderCommand);
        }
    }
    draw_call.draw_count = render_commands.size() - prev_offset_index;
    draw_calls.push_back(draw_call);
}

const BufferWrapper &PolygonInstanceContainer::getIndirectBuf() const { return indirect_buf; }
const std::vector<DrawIndirectInfo> &PolygonInstanceContainer::getDrawCalls() const { return draw_calls; }

} // namespace Pelican
