#include "materialrender.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "polygoninstancecontainer.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {

MaterialRenderer::MaterialRenderer(DependencyContainer &_con) : con{_con} {}

void MaterialRenderer::render(vk::CommandBuffer cmd_buf) const {
    const auto &instance_container = con.get<PolygonInstanceContainer>();
    const auto &vert_buf_container = con.get<VertBufContainer>();
    const auto &material_container = con.get<MaterialContainer>();

    vert_buf_container.bindVertexBuffer(cmd_buf);

    const auto pipeline_layout = material_container.getPipelineLayout();
    PushConstantStruct push_constant;
    // for test
    push_constant.mvp =
        glm::perspective(glm::radians(45.0f), 16 / (float)9, 0.1f, 100.0f) *
        glm::lookAt(glm::vec3(5.0f, 5.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
        glm::rotate(glm::mat4(1.0f), 0.0f, glm::vec3(0.0f, 0.0f, 1.0f));

    GlobalMaterialId current_material{-1};
    for (const auto &polygon : instance_container.getPolygons()) {
        cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push_constant),
                              &push_constant);

        if (polygon.material.value != current_material.value) {
            current_material = polygon.material;
            material_container.bindResource(cmd_buf, current_material);
        }
        cmd_buf.drawIndexed(polygon.command.indexCount, polygon.command.instanceCount, polygon.command.firstIndex,
                            polygon.command.vertexOffset, polygon.command.firstInstance);
    }
}

} // namespace Pelican
