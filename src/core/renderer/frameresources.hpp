#pragma once

#include "../container.hpp"
#include "../vkcore/buf.hpp"
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct alignas(16) FrameUniformData {
    alignas(16) glm::vec4 time_delta{0.0f};
    alignas(16) glm::uvec4 frame_index{0u};
    alignas(16) glm::vec4 resolution{0.0f};
    alignas(16) glm::vec4 camera_position{0.0f};
    alignas(16) glm::mat4 view{1.0f};
    alignas(16) glm::mat4 projection{1.0f};
    alignas(16) glm::mat4 previous_view{1.0f};
    alignas(16) glm::mat4 previous_projection{1.0f};
    alignas(8) glm::vec2 jitter_ndc{0.0f};
    alignas(8) glm::vec2 previous_jitter_ndc{0.0f};
    std::uint32_t temporal_reset_epoch = 0;
    std::uint32_t previous_temporal_reset_epoch = 0;
    glm::uvec2 temporal_padding{0u};
};

static_assert(offsetof(FrameUniformData, time_delta) == 0);
static_assert(offsetof(FrameUniformData, frame_index) == 16);
static_assert(offsetof(FrameUniformData, resolution) == 32);
static_assert(offsetof(FrameUniformData, camera_position) == 48);
static_assert(offsetof(FrameUniformData, view) == 64);
static_assert(offsetof(FrameUniformData, projection) == 128);
static_assert(offsetof(FrameUniformData, previous_view) == 192);
static_assert(offsetof(FrameUniformData, previous_projection) == 256);
static_assert(offsetof(FrameUniformData, jitter_ndc) == 320);
static_assert(offsetof(FrameUniformData, previous_jitter_ndc) == 328);
static_assert(offsetof(FrameUniformData, temporal_reset_epoch) == 336);
static_assert(offsetof(FrameUniformData, previous_temporal_reset_epoch) == 340);
static_assert(sizeof(FrameUniformData) == 352);

DECLARE_MODULE(FrameResources) {
    vk::Device device;
    BufferWrapper frame_buffer;
    vk::UniqueDescriptorPool descriptor_pool;
    vk::UniqueDescriptorSet descriptor_set;
    vk::Buffer object_buffer;
    vk::Buffer previous_object_buffer;
    vk::Buffer light_buffer;

  public:
    FrameResources();
    ~FrameResources();

    void setSceneBuffers(const BufferWrapper &objects, const BufferWrapper &previous_objects,
                         const BufferWrapper &lights);
    void update(const FrameUniformData &data) const;
    void bindGraphics(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const;
};

} // namespace Pelican
