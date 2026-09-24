#pragma once

#include "renderpipeline.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
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
    // Engine/runtime policy is supplied explicitly so the same feature
    // availability decision can be used by authoring catalogs and the real
    // compile/apply path.  Pure project-layer callers may leave it empty.
    std::function<void(std::string_view, const nlohmann::json &)>
        validate_feature;
    // Authoring-only observer of concrete declarations at the actual binding
    // position. Runtime compilation leaves this empty.
    std::function<void(std::string_view, const nlohmann::json &)>
        materialized_feature;
    // Reports only integration fragments whose feature/pass requirements match.
    std::function<void(std::string_view, const nlohmann::json &)>
        applied_integration;
    std::set<std::string> inactive_graph_scopes;
};

struct RenderFeatureComposeResult {
    nlohmann::json config;
    std::vector<std::string> shader_defines;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::optional<nlohmann::json> projection_jitter;
    nlohmann::json feature_instances = nlohmann::json::array();
    nlohmann::json surface_resource_contracts =
        nlohmann::json::array();
    nlohmann::json material_routing;
    nlohmann::json draw_sort;
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    std::vector<RenderPassProvenance> pass_provenance;
    std::vector<RenderResourceProvenance> resource_provenance;
    bool used_features = false;
};

RenderFeatureComposeResult composeRenderFeatureConfig(
    const nlohmann::json &config,
    const RenderFeatureComposeDependencies &dependencies = {});

// Resolve a feature's parameters into editable declarations. The config must
// contain the resources visible at this feature's composition position.
// The returned feature can be serialized and loaded by the normal composer;
// it retains provider parameters but no longer accepts parameter overrides.
nlohmann::json materializeRenderFeatureDocument(
    const nlohmann::json &feature,
    const nlohmann::json &instance_parameters,
    const nlohmann::json &config);

inline constexpr std::string_view
    renderFeatureRuntimeCompilerRequiredMessage =
        "render feature には実行時コンパイラが必要です (runtime shader compiler is required)";

bool renderFeatureRequiresRuntimeShaderCompiler(
    const nlohmann::json &feature,
    std::string_view feature_name);
std::vector<std::string> renderFeatureRequiredCapabilities(
    const nlohmann::json &feature,
    std::string_view feature_name);

// G-6: whether `before:<name>` / `after:<name>` names one of the canonical
// color pipeline's anchors. Composition resolves such an insert to the anchor
// before any pass that happens to share the name.
bool isCanonicalRenderPassAnchor(std::string_view name);

} // namespace Pelican
