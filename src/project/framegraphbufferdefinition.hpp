#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class FrameGraphHostBufferSource : std::uint8_t {
    scene_lights_v2,
    view_family_matrices_v1,
    scene_draw_commands_v1,
    scene_draw_bounds_v1,
    scene_draw_segments_v1,
    runtime_f32,
};

std::string_view frameGraphHostBufferSourceName(
    FrameGraphHostBufferSource source);

// Command layouts are logical buffer element contracts. They imply the
// required physical Vulkan usage without exposing VkBufferUsageFlags in
// project authoring.
enum class FrameGraphBufferCommandLayout : std::uint8_t {
    compute_dispatch,
    ray_dispatch,
    indexed_draw,
    draw_count,
};

std::string_view frameGraphBufferCommandLayoutName(
    FrameGraphBufferCommandLayout layout);

// RC-3a2: the byte sizes are the engine's (uint64, as Vulkan's VkDeviceSize
// is). An indexed draw command is five 32-bit words; core checks that this
// is Vulkan's VkDrawIndexedIndirectCommand (vulkanimagevocabulary.cpp).
inline constexpr std::uint64_t
    frameGraphIndexedDrawCommandBytes =
        sizeof(std::uint32_t) * 5;
inline constexpr std::uint64_t
    frameGraphDrawCountBytes =
        sizeof(std::uint32_t);
inline constexpr std::uint64_t
    frameGraphSceneDrawBoundsV1Bytes =
        sizeof(float) * 8;
inline constexpr std::uint64_t
    frameGraphSceneDrawSegmentV1Bytes =
        sizeof(std::uint32_t) * 8;
inline constexpr std::uint64_t
    frameGraphIndirectCommandAlignment = 4;

struct FrameGraphBufferExtentSizeDefinition {
    std::string resource;
    std::uint32_t tile_width = 1;
    std::uint32_t tile_height = 1;
    std::uint64_t header_bytes = 0;
    std::uint64_t bytes_per_tile = 0;
    // Repeats the complete header + tiled payload region. View-affine
    // algorithms use one region per bounded logical view and select the
    // region in shader-visible ABI data.
    std::uint32_t copies = 1;

    bool operator==(
        const FrameGraphBufferExtentSizeDefinition &) const =
        default;
};

struct FrameGraphBufferDefinition {
    std::string name;
    std::uint64_t size = 0;
    bool persistent = true;
    std::optional<FrameGraphHostBufferSource>
        host_source;
    std::optional<FrameGraphBufferExtentSizeDefinition>
        extent_size;
    std::optional<FrameGraphBufferCommandLayout>
        command_layout;
    // Required by view_family_matrices_v1; selects the resolved provider family.
    std::string host_view_family;
    bool index_buffer = false;
    // Starting contents of a runtime_f32 buffer, in floats. Empty means zero,
    // the behavior every buffer had before feature parameters could seed one.
    // A `render.scalar` write replaces it from the next frame onwards.
    std::vector<float> initial_f32;
};

// A compute task's indirect dispatch: the buffer holding the command and the
// command's byte offset in it.
struct ComputeIndirectDispatchDefinition {
    std::string buffer;
    std::uint64_t offset = 0;

    bool operator==(
        const ComputeIndirectDispatchDefinition &) const =
        default;
};

} // namespace Pelican
