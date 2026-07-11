#pragma once

#include "../container.hpp"
#include "../material/material.hpp"
#include "../model/modeltemplate.hpp"
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
    BufferWrapper model_data_buffer;
    vk::Device device;
    BufferWrapper skin_palette_buffer;
    vk::UniqueDescriptorSetLayout skin_descriptor_layout;
    vk::UniqueDescriptorPool skin_descriptor_pool;
    vk::UniqueDescriptorSet skin_descriptor_set;

  public:
    PolygonInstanceContainer();
    ModelInstanceId placeModelInstance(ModelTemplate & model);
    void removeModelInstance(ModelInstanceId id);
    void clear();
    void triggerUpdate();

    void setTrs(ModelInstanceId id, glm::vec3 pos, glm::quat rotation, glm::vec3 scale);
    void setSkinningPalette(ModelInstanceId id, std::span<const glm::mat4> palette);
    void bindSkinning(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const;

    const BufferWrapper &getIndirectBuf() const;
    const BufferWrapper &getObjectBuf() const;
    const std::vector<DrawIndirectInfo> &getDrawCalls() const;
    size_t instanceCountForTesting() const { return model_instances_data.size(); }
};

} // namespace Pelican
