#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "frameresources.hpp"

namespace Pelican {

FullscreenPassRenderer::FullscreenPassRenderer() {}
FullscreenPassRenderer::~FullscreenPassRenderer() {}

void FullscreenPassRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &,
                                    const FullscreenPassRendererDependencies &dependencies,
                                    RenderPassViewInvocation invocation) const {
    auto &container = dependencies.fullscreen_pass_container;
    const auto pipeline_layout = container.getPipelineLayout(pass_id);

    container.bindResource(cmd_buf, pass_id, invocation);
    dependencies.frame_resources.bindGraphics(cmd_buf, pipeline_layout);

    cmd_buf.draw(6, 1, 0, 0);
}

} // namespace Pelican
