#pragma once

#include "../container.hpp"
#include "../vkcore/buf.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
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
    std::uint32_t view_index = 0;
    std::uint32_t view_count = 1;
    alignas(16) glm::vec4 clip_plane{0.0f};
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
static_assert(offsetof(FrameUniformData, view_index) == 344);
static_assert(offsetof(FrameUniformData, view_count) == 348);
static_assert(offsetof(FrameUniformData, clip_plane) == 352);
static_assert(sizeof(FrameUniformData) == 368);
static_assert(std::is_trivially_copyable_v<FrameUniformData>);

struct alignas(16) FrameResolutionUniformData {
    // xy = pixels, zw = reciprocal pixels.
    alignas(16) glm::vec4 render_resolution{0.0f};
    alignas(16) glm::vec4 output_resolution{0.0f};
};

static_assert(
    offsetof(FrameResolutionUniformData, render_resolution) == 0);
static_assert(
    offsetof(FrameResolutionUniformData, output_resolution) == 16);
static_assert(sizeof(FrameResolutionUniformData) == 32);
static_assert(
    std::is_trivially_copyable_v<FrameResolutionUniformData>);

// Produces the std140-compatible contiguous array consumed by
// PELICAN_MULTIVIEW shaders. With one view this is byte-identical to the
// existing scalar FrameUniformData ABI.
std::vector<std::byte> packFrameUniformViews(
    std::span<const FrameUniformData> views);
std::vector<std::byte> packFrameResolutionViews(
    std::span<const FrameResolutionUniformData> views);

struct FrameDescriptorPoolPlan {
    std::uint32_t max_sets = 0;
    std::vector<vk::DescriptorPoolSize> pool_sizes;

    bool operator==(const FrameDescriptorPoolPlan &) const = default;
};

FrameDescriptorPoolPlan makeFrameDescriptorPoolPlan(
    std::uint32_t slot_count, bool ray_query);

DECLARE_MODULE(FrameResources) {
    struct FrameSlot {
        BufferWrapper frame_buffer;
        BufferWrapper resolution_buffer;
        vk::UniqueDescriptorSet descriptor_set;
        vk::UniqueDescriptorSet ray_query_descriptor_set;
        FrameUniformData last_data;
        FrameResolutionUniformData last_resolution;
    };
    struct MultiviewFrameSlot {
        BufferWrapper frame_buffer;
        BufferWrapper resolution_buffer;
        vk::UniqueDescriptorSet descriptor_set;
        vk::UniqueDescriptorSet ray_query_descriptor_set;
        std::vector<FrameUniformData> last_data;
        std::vector<FrameResolutionUniformData>
            last_resolution;
    };

    vk::Device device;
    vk::UniqueDescriptorPool descriptor_pool;
    std::vector<FrameSlot> frame_slots;
    std::vector<MultiviewFrameSlot>
        multiview_frame_slots;
    vk::Buffer object_buffer;
    vk::Buffer previous_object_buffer;
    vk::Buffer light_buffer;
    vk::Buffer directional_shadow_buffer;
    std::uint32_t view_count = 0;
    std::uint32_t sequential_view_count = 0;
    std::size_t active_slot = 0;
    bool active_multiview_slot = false;
    bool ray_query_enabled = false;

    void configureViewCount(
        std::uint32_t count,
        std::uint32_t sequential_count,
        bool ray_query);
    void updateSceneDescriptors();

  public:
    FrameResources();
    ~FrameResources();

    void setSceneBuffers(const BufferWrapper &objects, const BufferWrapper &previous_objects,
                         const BufferWrapper &lights,
                         const BufferWrapper &directional_shadows);
    void beginLogicalFrame(std::uint32_t count);
    void beginLogicalFrame(
        std::uint32_t main_view_count,
        std::uint32_t sequential_count);
    void beginLogicalFrame(
        std::uint32_t main_view_count,
        std::uint32_t sequential_count,
        bool ray_query);
    void setRayQueryAccelerationStructure(
        std::uint32_t in_flight_frame_index,
        vk::AccelerationStructureKHR top_level);
    void selectView(std::uint32_t in_flight_frame_index, std::uint32_t view_index);
    void selectSequentialView(
        std::uint32_t in_flight_frame_index,
        std::uint32_t sequential_view_index);
    void selectMultiview(
        std::uint32_t in_flight_frame_index);
    void update(const FrameUniformData &data);
    void updateResolution(
        const FrameResolutionUniformData &data);
    void updateMultiview(
        std::span<const FrameUniformData> data);
    void updateMultiviewResolutions(
        std::span<const FrameResolutionUniformData> data);
    void bindGraphics(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const;
    void bindCompute(vk::CommandBuffer cmd_buf,
                     vk::PipelineLayout pipeline_layout) const;

    std::uint32_t viewCountForTesting() const { return view_count; }
    std::uint32_t sequentialViewCountForTesting() const {
        return sequential_view_count;
    }
    std::size_t slotCountForTesting() const { return frame_slots.size(); }
    vk::Buffer slotBufferForTesting(std::uint32_t in_flight_frame_index,
                                    std::uint32_t view_index) const;
    const FrameUniformData &slotDataForTesting(std::uint32_t in_flight_frame_index,
                                               std::uint32_t view_index) const;
    const FrameResolutionUniformData &
    slotResolutionForTesting(
        std::uint32_t in_flight_frame_index,
        std::uint32_t view_index) const;
    const BufferWrapper &
    multiviewSlotBufferForTesting(
        std::uint32_t in_flight_frame_index) const;
    const std::vector<FrameUniformData> &
    multiviewSlotDataForTesting(
        std::uint32_t in_flight_frame_index) const;
    const std::vector<FrameResolutionUniformData> &
    multiviewSlotResolutionForTesting(
        std::uint32_t in_flight_frame_index) const;
};

} // namespace Pelican
