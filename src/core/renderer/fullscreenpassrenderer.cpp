#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "frameresources.hpp"

namespace Pelican {

FullscreenPassRenderer::FullscreenPassRenderer() {}
FullscreenPassRenderer::~FullscreenPassRenderer() {}

void FullscreenPassRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                                    const FullscreenPassRendererDependencies &dependencies,
                                    RenderPassViewInvocation invocation) const {
    auto &container = dependencies.fullscreen_pass_container;
    const auto pipeline_layout = container.getPipelineLayout(pass_id);

    container.bindResource(cmd_buf, pass_id, invocation);
    dependencies.frame_resources.bindGraphics(cmd_buf, pipeline_layout);

    if (!pass_def.isGenericRaster()) {
        cmd_buf.draw(6, 1, 0, 0);
        return;
    }
    std::visit(
        [&](const auto &draw) {
            cmd_buf.draw(
                draw.vertex_count,
                draw.instance_count,
                draw.first_vertex,
                draw.first_instance);
        },
        pass_def.genericRasterInfo()
            .contract.geometry.operation);
}

} // namespace Pelican
