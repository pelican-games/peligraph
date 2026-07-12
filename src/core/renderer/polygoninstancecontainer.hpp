#pragma once

#include "../container.hpp"
#include "../material/material.hpp"
#include "../model/modeltemplate.hpp"
#include "../userpublic/animation/abi_v1.hpp"
#include "../vkcore/buf.hpp"
#include "modelinstance.hpp"
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>
#include <span>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RenderCommand {
    vk::DrawIndexedIndirectCommand command;
    GlobalMaterialId material;
    bool skinned = false;
};

struct DrawIndirectInfo {
    GlobalMaterialId material;
    vk::DeviceSize offset;
    uint32_t draw_count, stride;
    bool skinned = false;
};

DECLARE_MODULE(PolygonInstanceContainer) {
    std::vector<RenderCommand> render_commands;
    BufferWrapper indirect_buf;
    std::vector<DrawIndirectInfo> draw_calls;

    std::vector<glm::mat4> model_instances_data;
    std::vector<glm::mat4> previous_model_instances_data;
    std::vector<bool> model_history_valid;
    BufferWrapper model_data_buffer;
    BufferWrapper previous_model_data_buffer;
    vk::Device device;
    std::vector<std::vector<glm::mat4>> skin_palettes;
    std::vector<std::vector<glm::mat4>> previous_skin_palettes;
    std::vector<std::uint64_t> animation_revisions;
    std::vector<std::uint64_t> previous_animation_revisions;
    std::vector<std::uint32_t> animation_generations;
    BufferWrapper skin_palette_buffer;
    BufferWrapper previous_skin_palette_buffer;
    vk::UniqueDescriptorSetLayout skin_descriptor_layout;
    vk::UniqueDescriptorPool skin_descriptor_pool;
    vk::UniqueDescriptorSet skin_descriptor_set;

  public:
    PolygonInstanceContainer();
    ModelInstanceId placeModelInstance(ModelTemplate & model);
    void removeModelInstance(ModelInstanceId id);
    void clear();
    void triggerUpdate();
    void commitFrameHistory();
    void advanceTemporalHistoryAfterRender();
    void resetTemporalHistory();

    void setTrs(ModelInstanceId id, glm::vec3 pos, glm::quat rotation, glm::vec3 scale);
    void setSkinningPalette(ModelInstanceId id, std::span<const glm::mat4> palette);
    Animation::InstanceHandle animationInstance(ModelInstanceId id) const;
    Animation::Status publishAnimationFrame(ModelInstanceId id,
                                             const Animation::PublishAnimationFrameDescV1 &frame);
    void bindSkinning(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const;

    const BufferWrapper &getIndirectBuf() const;
    const BufferWrapper &getObjectBuf() const;
    const BufferWrapper &getPreviousObjectBuf() const;
    const std::vector<DrawIndirectInfo> &getDrawCalls() const;
    size_t instanceCountForTesting() const { return model_instances_data.size(); }
    glm::mat4 currentModelMatrixForTesting(ModelInstanceId id) const { return model_instances_data.at(id.value); }
    glm::mat4 previousModelMatrixForTesting(ModelInstanceId id) const { return previous_model_instances_data.at(id.value); }
    std::uint64_t currentAnimationRevisionForTesting(ModelInstanceId id) const {
        return animation_revisions.at(id.value);
    }
    std::uint64_t previousAnimationRevisionForTesting(ModelInstanceId id) const {
        return previous_animation_revisions.at(id.value);
    }
};

} // namespace Pelican
