#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "camera.hpp"
#include "../light/lightcontainer.hpp"

namespace Pelican {

FullscreenPassRenderer::FullscreenPassRenderer() {}
FullscreenPassRenderer::~FullscreenPassRenderer() {}

void FullscreenPassRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id) const {
    auto& container = GET_MODULE(FullscreenPassContainer);
    auto& light_container = GET_MODULE(LightContainer);

    container.bindResource(cmd_buf, pass_id);

    cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, container.getPipelineLayout(), 1, {light_container.GetDescriptorSet()}, {});

    // PassId 0 is assumed to be the lighting pass, which needs camera position.
    // Other passes (bloom) have hardcoded parameters in their shaders.
    if (pass_id.value == 0) {
        glm::vec4 cameraPos = glm::vec4(GET_MODULE(Camera).getPos(), 1.0f);
        cmd_buf.pushConstants(container.getPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(cameraPos), &cameraPos);
    }

    cmd_buf.draw(6, 1, 0, 0);
}

} // namespace Pelican