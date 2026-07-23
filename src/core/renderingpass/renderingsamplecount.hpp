#pragma once

#include "rendertargetdefinition.hpp"
#include "../../project/samplecountplanning.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Pelican {

using AttachmentSampleCapabilityQuery =
    std::function<std::vector<std::uint32_t>(
        const RenderTargetDefinition &)>;

struct RenderingSampleCountAssignment {
    std::string resource;
    std::uint32_t samples = 1;

    bool operator==(const RenderingSampleCountAssignment &) const = default;
};

struct RenderingSampleCountResolution {
    ResolvedSampleCountPlan plan;
    std::vector<RenderingSampleCountAssignment> assignments;
};

// Builds attachment-connected components from the authored pass outputs.
// Geometry components are selected by default; "scope": "all" selects every
// renderable component and explicit "targets" can opt individual components
// in without hard-coding G-buffer names or attachment counts.
RenderingSampleCountResolution resolveRenderingSampleCounts(
    const nlohmann::json &rendering_config,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    const AttachmentSampleCapabilityQuery &query_capabilities);

void applyRenderingSampleCounts(
    std::span<RenderTargetDefinition> render_targets,
    const RenderingSampleCountResolution &resolution);

vk::ResolveModeFlagBits colorAttachmentResolveMode(vk::Format format);

} // namespace Pelican
