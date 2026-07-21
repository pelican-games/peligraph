#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

namespace Pelican::FeatureComposeInternal {

// Validates and canonicalizes the projection-jitter provider declaration.
// Keeping this policy separate prevents the general graph composer from
// accumulating temporal-algorithm-specific schema rules.
std::optional<nlohmann::json> parseProjectionJitterDeclaration(
    const nlohmann::json &feature, const std::string &feature_name);

} // namespace Pelican::FeatureComposeInternal
