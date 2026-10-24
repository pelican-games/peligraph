#include "framegraphbufferdefinition.hpp"

#include <stdexcept>

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

} // namespace Pelican
