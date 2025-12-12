#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "camera.hpp"

namespace Pelican {

FullscreenPassRenderer::FullscreenPassRenderer() {}
FullscreenPassRenderer::~FullscreenPassRenderer() {}

void FullscreenPassRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id) const {
    auto& container = GET_MODULE(FullscreenPassContainer);
    container.bindResource(cmd_buf, pass_id);

    glm::vec3 cameraPos = GET_MODULE(Camera).getPos();
    glm::vec4 pushConsts = glm::vec4(cameraPos, 1.0f);
    cmd_buf.pushConstants(container.getPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(pushConsts), &pushConsts);

    cmd_buf.draw(6, 1, 0, 0);
}

} // namespace Pelican