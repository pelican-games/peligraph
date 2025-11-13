#include "materialrender.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "camera.hpp"
#include "polygoninstancecontainer.hpp"

namespace Pelican {

MaterialRenderer::MaterialRenderer(DependencyContainer &_con) : con{_con} {}

void MaterialRenderer::render(vk::CommandBuffer cmd_buf) const {
    auto &instance_container = con.get<PolygonInstanceContainer>();
    const auto &vert_buf_container = con.get<VertBufContainer>();
    const auto &material_container = con.get<MaterialContainer>();

    vert_buf_container.bindVertexBuffer(cmd_buf);

    const auto pipeline_layout = material_container.getPipelineLayout();
    PushConstantStruct push_constant;
    push_constant.mvp = con.get<Camera>().getVPMatrix();

    instance_container.triggerUpdate();

    const auto &indirect_buf = instance_container.getIndirectBuf();
    const auto &draw_calls = instance_container.getDrawCalls();

    cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push_constant), &push_constant);
    for (const auto &draw_call : draw_calls) {
        material_container.bindResource(cmd_buf, draw_call.material);
        cmd_buf.drawIndexedIndirect(indirect_buf.buffer.get(), draw_call.offset, draw_call.draw_count,
                                    draw_call.stride);
    }
}

} // namespace Pelican
