#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "camera.hpp"
#include "../light/lightcontainer.hpp"

namespace Pelican {

namespace {
constexpr uint32_t lightDescriptorSetNumber = 1;

struct CameraPositionPC {
    glm::vec4 cameraPos;
};

struct ProjectionViewPC {
    glm::mat4 proj;
    glm::mat4 view;
};

void pushCameraPosition(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) {
    auto &camera = GET_MODULE(Camera);

    CameraPositionPC pc;
    pc.cameraPos = glm::vec4(camera.getPos(), 1.0f);
    cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(pc), &pc);
}

void pushProjectionView(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) {
    auto &camera = GET_MODULE(Camera);

    ProjectionViewPC pc;
    pc.proj = camera.getProjectionMatrix();
    pc.view = camera.getViewMatrix();
    cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(pc), &pc);
}

void pushFullscreenConstants(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout,
                             FullscreenPushConstantData push_constants) {
    switch (push_constants) {
    case FullscreenPushConstantData::eCameraPosition:
        pushCameraPosition(cmd_buf, pipeline_layout);
        break;
    case FullscreenPushConstantData::eProjectionView:
        pushProjectionView(cmd_buf, pipeline_layout);
        break;
    case FullscreenPushConstantData::eNone:
        break;
    }
}

} // namespace

FullscreenPassRenderer::FullscreenPassRenderer() {}
FullscreenPassRenderer::~FullscreenPassRenderer() {}

void FullscreenPassRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def) const {
    auto &container = GET_MODULE(FullscreenPassContainer);
    auto &light_container = GET_MODULE(LightContainer);
    const auto pipeline_layout = container.getPipelineLayout();

    container.bindResource(cmd_buf, pass_id);

    light_container.bindResource(cmd_buf, pipeline_layout, lightDescriptorSetNumber);

    pushFullscreenConstants(cmd_buf, pipeline_layout, pass_def.fullscreenInfo().push_constants);

    cmd_buf.draw(6, 1, 0, 0);
}

} // namespace Pelican
