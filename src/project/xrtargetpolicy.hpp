#pragma once

#include "graphvariantpolicy.hpp"
#include "xrmultiviewprofile.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace Pelican {

// Authoring preference for the physical execution selected after the logical
// XR graph has been compiled.  This deliberately does not extend
// RenderStrategyV1: renderer strategies author logical stereo content, while
// the target compiler resolves device-dependent execution.
enum class XrViewExecutionPreference {
    automatic,
    sequential,
    require_multiview,
};

std::string_view xrViewExecutionPreferenceName(
    XrViewExecutionPreference preference);

struct CompiledXrTargetPolicy {
    XrViewExecutionPreference view_execution =
        XrViewExecutionPreference::automatic;
    XrMultiviewAutoPolicy multiview_auto;
    bool multiview_auto_authored = false;
    bool authored = false;
    bool active = false;

    bool operator==(const CompiledXrTargetPolicy &) const = default;
};

// The optional top-level `xr` object is validated for every variant because a
// single project config is compiled into flat/preview/XR programs.  It becomes
// active only for the XR program.
CompiledXrTargetPolicy compileXrTargetPolicy(
    const nlohmann::json &config,
    RenderPipelineGraphVariant variant);

nlohmann::ordered_json xrTargetPolicyToJson(
    const CompiledXrTargetPolicy &policy);

} // namespace Pelican
