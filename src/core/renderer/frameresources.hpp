#pragma once

#include "../container.hpp"
#include "../vkcore/buf.hpp"
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
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
    struct FrameSlot {
        BufferWrapper frame_buffer;
        vk::UniqueDescriptorSet descriptor_set;
        FrameUniformData last_data;
    };

    vk::Device device;
    vk::UniqueDescriptorPool descriptor_pool;
    std::vector<FrameSlot> frame_slots;
    vk::Buffer object_buffer;
    vk::Buffer previous_object_buffer;
    vk::Buffer light_buffer;
    std::uint32_t view_count = 0;
    std::size_t active_slot = 0;

    void configureViewCount(std::uint32_t count);
    void updateSceneDescriptors();

  public:
    FrameResources();
    ~FrameResources();

    void setSceneBuffers(const BufferWrapper &objects, const BufferWrapper &previous_objects,
                         const BufferWrapper &lights);
    void beginLogicalFrame(std::uint32_t count);
    void selectView(std::uint32_t in_flight_frame_index, std::uint32_t view_index);
    void update(const FrameUniformData &data);
    void bindGraphics(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const;

    std::uint32_t viewCountForTesting() const { return view_count; }
    std::size_t slotCountForTesting() const { return frame_slots.size(); }
    vk::Buffer slotBufferForTesting(std::uint32_t in_flight_frame_index,
                                    std::uint32_t view_index) const;
    const FrameUniformData &slotDataForTesting(std::uint32_t in_flight_frame_index,
                                               std::uint32_t view_index) const;
};

} // namespace Pelican
