#pragma once

#include "renderpipeline.hpp"

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
    // Kept separate from feature loading so embedders may apply distinct
    // allowlists.  Registration normally supplies the same PathResolver-backed
    // loader to both seams.
    RenderPipelinePresetLoader load_pipeline_json;
    // Runs after preset expansion and before feature parsing. The production
    // renderer uses this pure seam for an optional renderer-wide strategy;
    // the returned config is still compiled by every normal validation stage.
    std::function<nlohmann::json(const nlohmann::json &)>
        transform_resolved_config;
};

struct RenderFeatureComposeResult {
    nlohmann::json config;
    std::vector<std::string> shader_defines;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::optional<nlohmann::json> projection_jitter;
    nlohmann::json feature_instances = nlohmann::json::array();
    nlohmann::json material_routing;
    nlohmann::json draw_sort;
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    bool used_features = false;
};

RenderFeatureComposeResult composeRenderFeatureConfig(
    const nlohmann::json &config,
    const RenderFeatureComposeDependencies &dependencies = {});

} // namespace Pelican
