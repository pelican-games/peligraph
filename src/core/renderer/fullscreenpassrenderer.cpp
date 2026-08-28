#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "frameresources.hpp"

#include <algorithm>

namespace Pelican {

FullscreenDrawCommand fullscreenDrawCommand(
    const FullscreenPassInfo &fullscreen_info) noexcept {
    const auto &raster_state = fullscreen_info.raster_state;
    const bool has_fixed_function_blend =
        raster_state &&
        std::ranges::any_of(
            raster_state->color_attachments,
            [](const auto &attachment) {
                return attachment.blend.enabled;
            });
    const bool uses_engine_oversized_triangle =
        fullscreen_info.vert_shader.ref == "engine://fullscreen";
    const bool uses_triangle_list =
        !raster_state ||
        raster_state->topology ==
            RasterPrimitiveTopology::triangle_list;

    // 9568042^ recorded {6,1,0,0} for every fullscreen pass. Only an
    // explicitly blended engine oversized triangle may narrow that command
    // to three vertices. The parser rejects an authored non-triangle-list
    // engine pass, while this condition keeps programmatic definitions on the
    // parent command rather than silently changing their primitive meaning.
    return {
        .vertex_count =
            has_fixed_function_blend &&
                    uses_engine_oversized_triangle &&
                    uses_triangle_list
                ? 3u
                : 6u,
    };
}

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
        const auto draw =
            fullscreenDrawCommand(pass_def.fullscreenInfo());
        cmd_buf.draw(
            draw.vertex_count, draw.instance_count,
            draw.first_vertex, draw.first_instance);
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
