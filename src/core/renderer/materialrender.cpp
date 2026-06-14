#include "materialrender.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "camera.hpp"
#include "polygoninstancecontainer.hpp"
#include <optional>

namespace Pelican {

namespace {
struct MaterialRange {
    uint32_t start;
    uint32_t count;
};

bool isOutsideMaterialRange(uint32_t material_index, const MaterialRange &range) {
    return material_index < range.start || material_index - range.start >= range.count;
}

void renderMaterialDraws(vk::CommandBuffer cmd_buf, PassId pass_id,
                         const MaterialRendererDependencies &dependencies,
                         std::optional<MaterialRange> material_range) {
    auto &instance_container = dependencies.instance_container;
    const auto &vert_buf_container = dependencies.vert_buf_container;
    const auto &material_container = dependencies.material_container;

    vert_buf_container.bindVertexBuffer(cmd_buf);

    instance_container.triggerUpdate();
    const auto &draw_calls = instance_container.getDrawCalls();
    if (draw_calls.empty()) {
        return;
    }

    const auto pipeline_layout = material_container.getPipelineLayout();
    PushConstantStruct push_constant;
    push_constant.mvp = dependencies.camera.getVPMatrix();
    cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push_constant), &push_constant);

    const auto &indirect_buf = instance_container.getIndirectBuf();
    GlobalMaterialId current_material_id = invalidMaterialId();
    uint32_t material_index = 0;
    for (const auto &draw_call : draw_calls) {
        if (!material_container.isRenderRequired(pass_id, draw_call.material)) {
            continue;
        }

        if (material_range.has_value() && isOutsideMaterialRange(material_index, *material_range)) {
            material_index++;
            continue;
        }

        material_container.bindResource(cmd_buf, pass_id, draw_call.material, current_material_id);
        current_material_id = draw_call.material;
        cmd_buf.drawIndexedIndirect(indirect_buf.buffer.get(), draw_call.offset, draw_call.draw_count,
                                    draw_call.stride);
        material_index++;
    }
}
}

MaterialRenderer::MaterialRenderer() {
    const auto &instance_container = GET_MODULE(PolygonInstanceContainer);
    auto &material_container = GET_MODULE(MaterialContainer);

    material_container.setModelMatBuf(instance_container.getObjectBuf());
}

void MaterialRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id,
                              const MaterialRendererDependencies &dependencies) const {
    renderMaterialDraws(cmd_buf, pass_id, dependencies, std::nullopt);
}

void MaterialRenderer::renderWithMaterialRange(vk::CommandBuffer cmd_buf, PassId pass_id,
                                               uint32_t material_start, uint32_t material_count,
                                               const MaterialRendererDependencies &dependencies) const {
    if (material_count == 0) {
        renderMaterialDraws(cmd_buf, pass_id, dependencies, std::nullopt);
        return;
    }

    renderMaterialDraws(cmd_buf, pass_id, dependencies, MaterialRange{material_start, material_count});
}

} // namespace Pelican
