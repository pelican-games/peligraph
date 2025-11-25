#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"

namespace Pelican {

FullscreenPassRenderer::FullscreenPassRenderer() {}
FullscreenPassRenderer::~FullscreenPassRenderer() {}

void FullscreenPassRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id) const {
    GET_MODULE(FullscreenPassContainer).bindResource(cmd_buf, pass_id);
    cmd_buf.draw(6, 1, 0, 0);
}

} // namespace Pelican