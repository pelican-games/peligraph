#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace Pelican {

enum class FrameGraphHostBufferSource : std::uint8_t {
    scene_lights_v2,
    scene_draw_commands_v1,
    scene_draw_bounds_v1,
    scene_draw_segments_v1,
};

std::string_view frameGraphHostBufferSourceName(
    FrameGraphHostBufferSource source);

// Command layouts are logical buffer element contracts. They imply the
// required physical Vulkan usage without exposing VkBufferUsageFlags in
// project authoring.
enum class FrameGraphBufferCommandLayout : std::uint8_t {
    compute_dispatch,
    indexed_draw,
    draw_count,
};

std::string_view frameGraphBufferCommandLayoutName(
    FrameGraphBufferCommandLayout layout);

inline constexpr vk::DeviceSize
    frameGraphIndexedDrawCommandBytes =
        sizeof(vk::DrawIndexedIndirectCommand);
inline constexpr vk::DeviceSize
    frameGraphDrawCountBytes =
        sizeof(std::uint32_t);
inline constexpr vk::DeviceSize
    frameGraphSceneDrawBoundsV1Bytes =
        sizeof(float) * 8;
inline constexpr vk::DeviceSize
    frameGraphSceneDrawSegmentV1Bytes =
        sizeof(std::uint32_t) * 8;
inline constexpr vk::DeviceSize
    frameGraphIndirectCommandAlignment = 4;

struct FrameGraphBufferExtentSizeDefinition {
    std::string resource;
    std::uint32_t tile_width = 1;
    std::uint32_t tile_height = 1;
    vk::DeviceSize header_bytes = 0;
    vk::DeviceSize bytes_per_tile = 0;

    bool operator==(
        const FrameGraphBufferExtentSizeDefinition &) const =
        default;
};

struct FrameGraphBufferDefinition {
    std::string name;
    vk::DeviceSize size = 0;
    bool persistent = true;
    std::optional<FrameGraphHostBufferSource>
        host_source;
    std::optional<FrameGraphBufferExtentSizeDefinition>
        extent_size;
    std::optional<FrameGraphBufferCommandLayout>
        command_layout;
};

} // namespace Pelican
