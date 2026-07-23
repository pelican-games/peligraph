#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FrameResources;

struct DebugDrawVertex {
    glm::vec4 position;
    glm::vec4 color;
};

DECLARE_MODULE(DebugDraw) {
    struct PipelineRecord {
        PipelineHandle pipeline;
        vk::UniqueDescriptorSet descriptor_set;
    };

    vk::Device device;
    bool enabled = false;
    vk::UniqueDescriptorPool descriptor_pool;
    BufferWrapper vertex_buffer;
    vk::DeviceSize vertex_buffer_bytes = 0;
    std::vector<DebugDrawVertex> vertices;
    std::unordered_map<PassId, PipelineRecord, PassId::Hash> pipelines;
    std::vector<PassId> registration_order;
    int next_pass_id = 0;

    void ensureDevice();
    void ensureDescriptorPool();
    void ensureVertexCapacity(size_t vertex_count);
    void ensureDescriptorSet(PassId pass_id, PipelineRecord &record);
    void updateDescriptorSet(const PipelineRecord &record, vk::DeviceSize bytes);

  public:
    struct RegistrationCheckpoint {
        std::size_t registration_count = 0;
        bool enabled = false;
        int next_pass_id = 0;
    };

    DebugDraw();
    ~DebugDraw();

    PassId registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                        ShaderBundleId frag_shader,
                        std::vector<std::string> shader_defines = {},
                        vk::SampleCountFlagBits samples =
                            vk::SampleCountFlagBits::e1);
    void line(glm::vec3 from_ndc, glm::vec3 to_ndc, glm::vec4 color);
    void line(glm::vec3 from_ndc, glm::vec3 to_ndc, glm::vec4 from_color, glm::vec4 to_color);
    void clear();
    void render(vk::CommandBuffer cmd_buf, PassId pass_id, const FrameResources &frame_resources);

    bool isEnabledForTesting() const { return enabled; }
    size_t queuedVertexCountForTesting() const { return vertices.size(); }

    RegistrationCheckpoint checkpointRegistrations() const noexcept;
    void rollbackRegistrations(RegistrationCheckpoint checkpoint);
    std::vector<PassId>
    registrationsSince(RegistrationCheckpoint checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return registration_order.size();
    }
    void retireRegistrations(
        const std::vector<PassId> &ids) noexcept;
};

} // namespace Pelican
