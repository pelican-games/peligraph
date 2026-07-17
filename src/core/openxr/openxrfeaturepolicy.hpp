#pragma once

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace Pelican::OpenXr {

inline constexpr std::string_view xr_mirror_intermediate_name =
    "__xr_mirror_left";

// Startup-only feature policy for the XR render-graph variant.  Known
// temporal/UI features are removed as complete feature units.  A history
// feature outside that known set is rejected by name because removing only
// part of an unknown feature would leave an unverifiable graph.
bool includeFeatureInXrGraph(std::string_view feature_name,
                             const nlohmann::json &feature);

void validateXrGraphConfig(const nlohmann::json &composed_config);

} // namespace Pelican::OpenXr
