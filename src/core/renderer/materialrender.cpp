#include "materialrender.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "polygoninstancecontainer.hpp"

namespace Pelican {

MaterialRenderer::MaterialRenderer(DependencyContainer &_con) : con{_con} {}

void MaterialRenderer::render(vk::CommandBuffer cmd_buf) const {
    const auto &instance_container = con.get<PolygonInstanceContainer>();
    const auto &vert_buf_container = con.get<VertBufContainer>();
    const auto &material_container = con.get<MaterialContainer>();

    vert_buf_container.bindVertexBuffer(cmd_buf);

    GlobalMaterialId current_material{-1};
    for (const auto &polygon : instance_container.getPolygons()) {
        if (polygon.material.value != current_material.value) {
            current_material = polygon.material;
            material_container.bindResource(cmd_buf, current_material);
        }
        cmd_buf.drawIndexed(polygon.command.indexCount, polygon.command.instanceCount, polygon.command.firstIndex,
                            polygon.command.vertexOffset, polygon.command.firstInstance);
    }
}

} // namespace Pelican
