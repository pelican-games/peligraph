#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "camera.hpp"
#include "../light/lightcontainer.hpp"

namespace Pelican {

FullscreenPassRenderer::FullscreenPassRenderer() {}
FullscreenPassRenderer::~FullscreenPassRenderer() {}

void FullscreenPassRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition& pass_def) const {
    auto& container = GET_MODULE(FullscreenPassContainer);
    auto& light_container = GET_MODULE(LightContainer);
    auto& camera = GET_MODULE(Camera);

    container.bindResource(cmd_buf, pass_id);

    cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, container.getPipelineLayout(), 1, {light_container.GetDescriptorSet()}, {});

    // Define push constant structures for different passes
    struct CameraPosPC {
        glm::vec4 cameraPos;
    };

    struct PVMatPC {
        glm::mat4 proj;
        glm::mat4 view;
    };

    // Push constants based on pass requirements
    if (pass_def.name == "lighting_pass") {
        CameraPosPC pc;
        pc.cameraPos = glm::vec4(camera.getPos(), 1.0f);
        cmd_buf.pushConstants(container.getPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(pc), &pc);
    }
    else if (pass_def.needs_projection_matrix) { // This flag is now a bit of a misnomer, but it works.
        PVMatPC pc;
        pc.proj = camera.getProjectionMatrix();
        pc.view = camera.getViewMatrix();
        cmd_buf.pushConstants(container.getPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(pc), &pc);
    }
    // Other fullscreen passes (bloom, blur) currently have no push constants

    cmd_buf.draw(6, 1, 0, 0);
}

} // namespace Pelican