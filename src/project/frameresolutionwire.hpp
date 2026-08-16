#pragma once

#include "targetrenderplanning.hpp"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::string_view frameRuntimeResolutionWireSchema =
    "pelican.frame_runtime_resolution";
inline constexpr std::uint32_t frameRuntimeResolutionWireVersion = 1;

struct FrameRuntimeResourceExtent {
    std::string resource;
    ResolvedResourceExtent extent;

    bool operator==(const FrameRuntimeResourceExtent &) const = default;
};

struct FrameRuntimeResolutionWire {
    std::string render_source_resource;
    ResolvedResourceExtent render_extent;
    std::string output_source_resource;
    ResolvedResourceExtent output_extent;
    std::vector<FrameRuntimeResourceExtent> resources;

    bool operator==(const FrameRuntimeResolutionWire &) const = default;
};

using FrameRuntimeExtentResolver =
    std::function<ResolvedResourceExtent(std::string_view)>;

// Builds the runtime-owned wire projection from concrete extents. The
// callback is supplied by the backend and must return the already-resolved
// extent for the named resource; this function never evaluates scale values.
FrameRuntimeResolutionWire makeFrameRuntimeResolutionWire(
    const VulkanTargetPlan &plan,
    const FrameRuntimeExtentResolver &resolve_extent);

nlohmann::ordered_json frameRuntimeResolutionWireToJson(
    const FrameRuntimeResolutionWire &resolution);
FrameRuntimeResolutionWire frameRuntimeResolutionWireFromJson(
    const nlohmann::json &document);

} // namespace Pelican
