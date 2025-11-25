#include "materialrender.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "camera.hpp"
#include "polygoninstancecontainer.hpp"

namespace Pelican {

MaterialRenderer::MaterialRenderer() {
    const auto &instance_container = GET_MODULE(PolygonInstanceContainer);
    auto &material_container = GET_MODULE(MaterialContainer);

    material_container.setModelMatBuf(instance_container.getObjectBuf());
}

void MaterialRenderer::render(vk::CommandBuffer cmd_buf, int pass_id) const {
    auto &instance_container = GET_MODULE(PolygonInstanceContainer);
    const auto &vert_buf_container = GET_MODULE(VertBufContainer);
    const auto &material_container = GET_MODULE(MaterialContainer);

    vert_buf_container.bindVertexBuffer(cmd_buf);

    const auto pipeline_layout = material_container.getPipelineLayout();
    PushConstantStruct push_constant;
    push_constant.mvp = GET_MODULE(Camera).getVPMatrix();

    instance_container.triggerUpdate();

    const auto &indirect_buf = instance_container.getIndirectBuf();
    const auto &draw_calls = instance_container.getDrawCalls();

    cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push_constant), &push_constant);

    GlobalMaterialId current_material_id{-1};
    for (const auto &draw_call : draw_calls) {
        material_container.bindResource(cmd_buf, pass_id, draw_call.material, current_material_id);
        current_material_id = draw_call.material;
        cmd_buf.drawIndexedIndirect(indirect_buf.buffer.get(), draw_call.offset, draw_call.draw_count,
                                    draw_call.stride);
    }
}

} // namespace Pelican
