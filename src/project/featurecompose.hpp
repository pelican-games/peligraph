#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct RenderFeatureComposeDependencies {
    std::function<std::string(std::string_view)> load_feature_json;
    bool runtime_shader_compiler_enabled = false;
    // Called after the feature envelope has been validated and before it is
    // composed.  This is a startup-only policy seam used by immutable graph
    // variants; returning false removes the whole feature from that variant.
    std::function<bool(std::string_view, const nlohmann::json &)> include_feature;
};

struct RenderFeatureComposeResult {
    nlohmann::json config;
    std::vector<std::string> shader_defines;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::optional<nlohmann::json> projection_jitter;
    nlohmann::json feature_instances = nlohmann::json::array();
    bool used_features = false;
};

RenderFeatureComposeResult composeRenderFeatureConfig(
    const nlohmann::json &config,
    const RenderFeatureComposeDependencies &dependencies = {});

} // namespace Pelican
