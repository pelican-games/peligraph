#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "frameresources.hpp"

#include <algorithm>

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
        const auto &raster_state = pass_def.fullscreenInfo().raster_state;
        const bool has_fixed_function_blend =
            raster_state &&
            std::ranges::any_of(
                raster_state->color_attachments,
                [](const auto &attachment) {
                    return attachment.blend.enabled;
                });
        const bool uses_engine_oversized_triangle =
            pass_def.fullscreenInfo().vert_shader.ref ==
            "engine://fullscreen";
        // The engine shader's first three procedural vertices form one
        // oversized triangle; vertices 3..5 overlap part of it. Preserve the
        // legacy six-vertex path for opaque compatibility and for project
        // vertex shaders that define an ordinary two-triangle quad. An
        // explicitly blended engine fullscreen pass must apply its source
        // contribution exactly once.
        cmd_buf.draw(has_fixed_function_blend && uses_engine_oversized_triangle
                         ? 3
                         : 6,
                     1, 0, 0);
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
