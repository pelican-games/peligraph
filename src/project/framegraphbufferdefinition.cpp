#include "framegraphbufferdefinition.hpp"

#include "graphcomposition.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace Pelican {

std::string_view frameGraphHostBufferSourceName(
    FrameGraphHostBufferSource source) {
    switch (source) {
    case FrameGraphHostBufferSource::runtime_f32:
        return "runtime_f32";
    case FrameGraphHostBufferSource::scene_lights_v2:
        return "scene_lights_v2";
    case FrameGraphHostBufferSource::view_family_matrices_v1:
        return "view_family_matrices_v1";
    case FrameGraphHostBufferSource::
        scene_draw_commands_v1:
        return "scene_draw_commands_v1";
    case FrameGraphHostBufferSource::
        scene_draw_bounds_v1:
        return "scene_draw_bounds_v1";
    case FrameGraphHostBufferSource::
        scene_draw_segments_v1:
        return "scene_draw_segments_v1";
    }
    throw std::runtime_error(
        "unknown frame-graph host buffer source");
}

std::string_view frameGraphBufferCommandLayoutName(
    FrameGraphBufferCommandLayout layout) {
    switch (layout) {
    case FrameGraphBufferCommandLayout::
        compute_dispatch:
        return "compute_dispatch";
    case FrameGraphBufferCommandLayout::ray_dispatch:
        return "ray_dispatch";
    case FrameGraphBufferCommandLayout::
        indexed_draw:
        return "indexed_draw";
    case FrameGraphBufferCommandLayout::
        draw_count:
        return "draw_count";
    }
    throw std::runtime_error(
        "unknown frame-graph buffer command layout");
}

namespace {

[[noreturn]] void refuseSize(const FrameGraphBufferDefinition &buffer, std::string_view field,
                             std::string_view reason) {
    throw GraphRegistryError("render_graph.buffer_invalid@1",
                             {{"resource", buffer.name}, {"field", std::string{field}},
                              {"reason", std::string{reason}}});
}

std::uint64_t checkedMultiply(const FrameGraphBufferDefinition &buffer, std::uint64_t lhs,
                              std::uint64_t rhs) {
    if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs)
        refuseSize(buffer, "size_from_extent", "overflow");
    return lhs * rhs;
}

} // namespace

std::uint64_t extentDerivedBufferBytes(const FrameGraphBufferDefinition &buffer,
                                       ResolvedResourceExtent image_extent) {
    const auto &size = *buffer.extent_size;
    const auto tiles_x = (static_cast<std::uint64_t>(image_extent.width) + size.tile_width - 1) / size.tile_width;
    const auto tiles_y = (static_cast<std::uint64_t>(image_extent.height) + size.tile_height - 1) / size.tile_height;
    const auto payload = checkedMultiply(buffer, checkedMultiply(buffer, tiles_x, tiles_y), size.bytes_per_tile);
    if (payload > std::numeric_limits<std::uint64_t>::max() - size.header_bytes)
        refuseSize(buffer, "size_from_extent", "overflow");
    return checkedMultiply(buffer, size.header_bytes + payload, size.copies);
}

void requireBufferSizeContract(const FrameGraphBufferDefinition &buffer) {
    const auto size = buffer.size;
    if (buffer.index_buffer && size % sizeof(std::uint32_t) != 0)
        refuseSize(buffer, "size", "index_buffer_not_uint32_aligned");
    if (buffer.host_source && size == 0) refuseSize(buffer, "size", "host_source_requires_size");
    if (buffer.host_source) switch (*buffer.host_source) {
    case FrameGraphHostBufferSource::runtime_f32:
        if (size % sizeof(float) != 0) refuseSize(buffer, "size", "runtime_f32_not_float_aligned");
        break;
    case FrameGraphHostBufferSource::view_family_matrices_v1:
        if (size < 16 || size % 16 != 0) refuseSize(buffer, "size", "view_family_matrices_not_uvec4_aligned");
        break;
    case FrameGraphHostBufferSource::scene_draw_commands_v1:
        if (size % frameGraphIndexedDrawCommandBytes != 0)
            refuseSize(buffer, "size", "scene_draw_commands_not_whole_commands");
        break;
    case FrameGraphHostBufferSource::scene_draw_bounds_v1:
        if (size % frameGraphSceneDrawBoundsV1Bytes != 0)
            refuseSize(buffer, "size", "scene_draw_bounds_not_whole_records");
        break;
    case FrameGraphHostBufferSource::scene_draw_segments_v1:
        if (size % frameGraphSceneDrawSegmentV1Bytes != 0)
            refuseSize(buffer, "size", "scene_draw_segments_not_whole_records");
        break;
    case FrameGraphHostBufferSource::scene_lights_v2:
        break;
    }
    if (!buffer.initial_f32.empty() && buffer.initial_f32.size() * sizeof(float) != size)
        refuseSize(buffer, "initial_f32", "count_mismatch");
    if (buffer.command_layout) {
        const auto minimum = [&] {
            switch (*buffer.command_layout) {
            case FrameGraphBufferCommandLayout::compute_dispatch:
            case FrameGraphBufferCommandLayout::ray_dispatch:
                return frameGraphDispatchCommandBytes;
            case FrameGraphBufferCommandLayout::indexed_draw:
                return frameGraphIndexedDrawCommandBytes;
            case FrameGraphBufferCommandLayout::draw_count:
                return frameGraphDrawCountBytes;
            }
            return std::uint64_t{0};
        }();
        if (size < minimum) refuseSize(buffer, "size", "smaller_than_command");
    }
}

} // namespace Pelican
