#include "fullscreenpassrenderer.hpp"

#include "../fullscreenpass/fullscreenpass_push_constants.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../light/lightcontainer.hpp"

namespace Pelican {

namespace {
constexpr uint32_t lightDescriptorSetNumber = 1;

void pushCameraPosition(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout,
                        const FullscreenPassCameraData &camera_data) {
    FullscreenCameraPositionPushConstant pc;
    pc.cameraPos = glm::vec4(camera_data.position, 1.0f);
    cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(pc), &pc);
}

void pushProjectionView(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout,
                        const FullscreenPassCameraData &camera_data) {
    FullscreenProjectionViewPushConstant pc;
    pc.proj = camera_data.projection;
    pc.view = camera_data.view;
    cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(pc), &pc);
}

void pushFullscreenConstants(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout,
                             FullscreenPushConstantData push_constants,
                             const FullscreenPassCameraData &camera_data) {
    switch (push_constants) {
    case FullscreenPushConstantData::eCameraPosition:
        pushCameraPosition(cmd_buf, pipeline_layout, camera_data);
        break;
    case FullscreenPushConstantData::eProjectionView:
        pushProjectionView(cmd_buf, pipeline_layout, camera_data);
        break;
    case FullscreenPushConstantData::eNone:
        break;
    }
}

} // namespace

FullscreenPassRenderer::FullscreenPassRenderer() {}
FullscreenPassRenderer::~FullscreenPassRenderer() {}

void FullscreenPassRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                                    const FullscreenPassCameraData &camera_data,
                                    const FullscreenPassRendererDependencies &dependencies) const {
    auto &container = dependencies.fullscreen_pass_container;
    const auto &fullscreenInfo = pass_def.fullscreenInfo();
    const auto pipeline_layout = container.getPipelineLayout(pass_id);

    container.bindResource(cmd_buf, pass_id);

    if (fullscreenInfo.uses_light_data) {
        dependencies.light_container.bindResource(cmd_buf, pipeline_layout, lightDescriptorSetNumber);
    }

    pushFullscreenConstants(cmd_buf, pipeline_layout, fullscreenInfo.push_constants, camera_data);

    cmd_buf.draw(6, 1, 0, 0);
}

} // namespace Pelican
