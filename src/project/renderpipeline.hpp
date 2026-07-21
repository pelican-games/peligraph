#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican {

// A route is a material classification.  It is deliberately not a pass id:
// several render graphs (flat/XR/preview) may provide different concrete
// passes for the same semantic route.
enum class MaterialRouteClass {
    deferred_geometry,
    forward_opaque,
    forward_transparent,
};

// Fragment-output ABI expected by a material pipeline.
enum class MaterialShaderContract {
    legacy_gbuffer_v1,
    gbuffer_v1,
    forward_scene_color_v1,
};

enum class MaterialPhase {
    opaque,
    transparent,
};

// The pass contract bundles the route accepted by a pass with the attachment
// ABI its material pipelines must use.  The implicit legacy value preserves
// existing five-MRT configs and accepts both generations of the G-buffer ABI.
enum class MaterialPassContract {
    legacy_gbuffer_v1,
    deferred_geometry_v1,
    forward_opaque_v1,
    forward_transparent_v1,
};

enum class MaterialRouteReason {
    automatic_deferred_compatible,
    automatic_openpbr_base,
    automatic_blended,
    automatic_screen_input,
    automatic_custom_brdf,
    automatic_custom_ambient,
    automatic_custom_lighting,
    explicit_deferred,
    explicit_forward,
};

enum class DeferredMaterialModel {
    standard_pbr_v1,
    openpbr_base_v1,
};

std::string_view materialRouteClassName(MaterialRouteClass route);
std::optional<MaterialRouteClass> materialRouteClassFromName(std::string_view name);
std::string_view materialShaderContractName(MaterialShaderContract contract);
std::string_view materialPhaseName(MaterialPhase phase);
std::string_view materialPassContractName(MaterialPassContract contract);
std::optional<MaterialPassContract> materialPassContractFromName(std::string_view name);
std::optional<MaterialRouteClass> materialPassRoute(MaterialPassContract contract);
MaterialShaderContract materialPassShaderContract(MaterialPassContract contract);
MaterialPhase materialPassPhase(MaterialPassContract contract);
std::string_view materialRouteReasonName(MaterialRouteReason reason);
std::string_view deferredMaterialModelName(DeferredMaterialModel model);
bool materialPassAcceptsMaterial(MaterialPassContract pass_contract,
                                 std::string_view pass_name,
                                 MaterialRouteClass material_route,
                                 MaterialShaderContract material_shader_contract,
                                 const std::optional<std::string> &exact_pass = std::nullopt);

struct RenderPipelinePresetInfo {
    std::string reference;
    std::string name;
    int version = 1;
};

struct RenderPipelinePresetResolution {
    nlohmann::json config;
    std::optional<RenderPipelinePresetInfo> preset;
};

using RenderPipelinePresetLoader =
    std::function<std::string(std::string_view reference)>;

// Expands the optional top-level pipeline.preset envelope into an ordinary
// verbose rendering config.  Source files are never rewritten.  v1 permits a
// deliberately small overlay surface; larger customization should copy/eject
// the resolved config instead of depending on an ambiguous deep merge.
RenderPipelinePresetResolution resolveRenderPipelinePreset(
    const nlohmann::json &authored_config,
    const RenderPipelinePresetLoader &load_preset_json = {});

// Validates the optional detailed material_routing table after features have
// been composed.  The returned object is stable diagnostic metadata suitable
// for frame-plan dumps.  A null JSON value means legacy routing.
nlohmann::json resolveMaterialRoutingTable(const nlohmann::json &composed_config);

// Authoring resolution is deliberately data-only.  These variants describe
// which immutable graph program is being resolved; backend lifecycle remains
// outside this layer.
enum class RenderPipelineGraphVariant {
    flat,
    preview,
    xr,
};

std::string_view renderPipelineGraphVariantName(
    RenderPipelineGraphVariant variant);

struct RenderPipelineRequest {
    nlohmann::json authored_config = nlohmann::json::object();
    std::string source_name = "render pipeline";
};

struct RenderEnvironmentCapabilities {
    bool runtime_shader_compiler_enabled = false;
    RenderPipelineGraphVariant graph_variant =
        RenderPipelineGraphVariant::flat;
};

enum class RenderPipelineDiagnosticKind {
    graph_variant_selected,
    feature_excluded,
    pipeline_preset_resolved,
};

struct RenderPipelineDiagnostic {
    RenderPipelineDiagnosticKind kind =
        RenderPipelineDiagnosticKind::graph_variant_selected;
    std::string subject;
    std::string detail;

    bool operator==(const RenderPipelineDiagnostic &) const = default;
};

// Every callback is a data transform or lookup. Callers snapshot mutable
// environment values and expose lookup services explicitly instead of making
// the resolver discover modules or containers. This keeps the resolver usable
// in CPU-only tests; the resolver itself has no path for publishing
// GPU/runtime state.
struct RenderPipelineResolveDependencies {
    std::function<std::string(std::string_view)> load_feature_json;
    RenderPipelinePresetLoader load_pipeline_json;
    std::function<bool(std::string_view, const nlohmann::json &)>
        include_feature;
    std::function<nlohmann::json(
        const nlohmann::json &, const std::vector<std::string> &)>
        normalize_config;
    std::function<void(nlohmann::json &)> transform_config;
    std::function<void(const nlohmann::json &)> validate_config;
    std::string rendering_pass_name_suffix;
};

struct ResolvedRenderPipeline {
    nlohmann::json normalized_config = nlohmann::json::object();
    std::vector<std::string> shader_defines;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::optional<nlohmann::json> projection_jitter;
    nlohmann::json feature_instances = nlohmann::json::array();
    nlohmann::json material_routing;
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    RenderPipelineGraphVariant graph_variant =
        RenderPipelineGraphVariant::flat;
    std::string rendering_pass_name_suffix;
    std::vector<RenderPipelineDiagnostic> diagnostics;
    bool used_features = false;
};

ResolvedRenderPipeline resolveRenderPipeline(
    const RenderPipelineRequest &request,
    const RenderEnvironmentCapabilities &capabilities,
    const RenderPipelineResolveDependencies &dependencies = {});

enum class ProjectionJitterPattern {
    halton23,
    table,
};

std::string_view projectionJitterPatternName(ProjectionJitterPattern pattern);

using CompiledRenderNumericValue =
    std::variant<std::int64_t, std::uint64_t, double>;

double compiledRenderNumericValueAsDouble(
    const CompiledRenderNumericValue &value);

struct CompiledProjectionJitter {
    std::string provider;
    ProjectionJitterPattern pattern = ProjectionJitterPattern::halton23;
    std::uint32_t phases = 8;
    std::vector<std::array<CompiledRenderNumericValue, 2>> offsets_px;
};

using CompiledRenderFeatureParameterValue =
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

struct CompiledRenderFeatureParameter {
    std::string name;
    CompiledRenderFeatureParameterValue value;
};

struct CompiledRenderFeatureInstance {
    std::string feature;
    std::string reference;
    std::vector<CompiledRenderFeatureParameter> parameters;
};

enum class MaterialRoutingPolicy {
    hybrid_auto_v1,
};

std::string_view materialRoutingPolicyName(MaterialRoutingPolicy policy);

struct CompiledMaterialRoute {
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    std::string pass_name;
    MaterialPassContract pass_contract =
        MaterialPassContract::deferred_geometry_v1;
};

struct CompiledMaterialRouting {
    MaterialRoutingPolicy policy = MaterialRoutingPolicy::hybrid_auto_v1;
    std::vector<CompiledMaterialRoute> routes;
};

// Immutable after publication.  It deliberately contains neither the
// normalized authoring JSON nor GPU/container state.  Runtime owners publish
// it through shared_ptr<const CompiledRenderPipeline>; JSON is reconstructed
// only by the dump serializer below.
struct CompiledRenderPipeline {
    std::vector<std::string> shader_defines;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::optional<CompiledProjectionJitter> projection_jitter;
    std::vector<CompiledRenderFeatureInstance> feature_instances;
    std::optional<CompiledMaterialRouting> material_routing;
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    RenderPipelineGraphVariant graph_variant =
        RenderPipelineGraphVariant::flat;
    std::string rendering_pass_name_suffix;
    std::vector<RenderPipelineDiagnostic> diagnostics;
    bool used_features = false;
};

// Pure CPU-only transition from authoring resolution into the runtime
// contract.  Any malformed transitional metadata is rejected before runtime
// containers can be mutated.
CompiledRenderPipeline compileRenderPipeline(
    const ResolvedRenderPipeline &pipeline);

// Dump-only compatibility serializer.  Runtime code must consume the typed
// fields above rather than reading keys from this representation.
nlohmann::json serializeCompiledRenderPipelineMetadata(
    const CompiledRenderPipeline &pipeline);

} // namespace Pelican
