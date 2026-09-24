#pragma once

#include "graphvariantpolicy.hpp"
#include "lightingdata.hpp"
#include "logicalrendergraph.hpp"
#include "materialscreeninput.hpp"
#include "materialoutput.hpp"
#include "passfieldownership.hpp"
#include "samplecountplanning.hpp"
#include "sceneaccelerationstructure.hpp"
#include "targetplanning.hpp"
#include "targetrenderplanning.hpp"
#include "xrtargetpolicy.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
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
    // G-4: an automatic material routed by the rendering document's
    // `material_route_default: "forward"` rather than by its own render_path.
    default_forward,
};

// G-4: the render_path an automatic material receives.  `automatic` is the
// behavior from before the key existed; `forward` sends every automatic
// material through the explicit-forward route.  An explicit render_path on a
// material always wins.
enum class MaterialRouteDefault {
    automatic,
    forward,
};

inline constexpr std::string_view materialRouteDefaultKey =
    "material_route_default";

std::string_view materialRouteDefaultName(MaterialRouteDefault value);

// Reads the key from a rendering section root (a direct root, or the root that
// selects a preset). Absent means `automatic`; any value other than "auto" or
// "forward" is refused by name.
MaterialRouteDefault readMaterialRouteDefault(
    const nlohmann::json &rendering_section);

// G-4: named refusals. An edit ticket carries them through the renderer's
// string channel as "<code>: <JSON fields>"; the editor's completion handler
// splits them back into a rejection code and fields
// (splitCodedRefusalMessage). The load-time fatal uses the same code with
// readable text (materialRouteUnservedText).
inline constexpr std::string_view materialRouteUnservedCode =
    "material_route_unserved@1";
inline constexpr std::string_view materialRouteDefaultUnavailableCode =
    "material_route_default_unavailable@1";

// Built-in engine materials carry names under this prefix so a refusal can
// name them instead of printing "<unnamed>".
inline constexpr std::string_view builtinMaterialNamePrefix =
    "pelican.builtin.";

struct UnservedMaterial {
    std::string name;
    // "builtin", "document", "asset" or "unknown".
    std::string origin;
    // The document or asset reference; empty for a built-in.
    std::string source;
};

struct UnservedMaterialRoute {
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    MaterialShaderContract shader_contract = MaterialShaderContract::gbuffer_v1;
    std::vector<UnservedMaterial> materials;
};

UnservedMaterial describeUnservedMaterial(std::string_view name,
                                          std::string_view source_document,
                                          std::string_view source_asset);
// What to add or set so the route has a receiving pass.
std::string materialRouteUnservedHint(MaterialRouteClass route);
// {"rendering_config", "unserved": [{route, shader_contract, materials: [{name,
// origin, source?}], hint}], "hint"}.
nlohmann::json materialRouteUnservedFields(
    const std::vector<UnservedMaterialRoute> &unserved,
    std::string_view rendering_config);
// "material_route_unserved@1: <fields JSON>".
std::string materialRouteUnservedMessage(
    const std::vector<UnservedMaterialRoute> &unserved,
    std::string_view rendering_config);
// "material_route_unserved@1: material 'x' (...) requires route ... Hint: ...".
std::string materialRouteUnservedText(
    const std::vector<UnservedMaterialRoute> &unserved,
    std::string_view rendering_config);
// The same text from the fields a ticket rejection carries.
std::string materialRouteUnservedText(const nlohmann::json &fields);
std::string materialRouteDefaultUnavailableMessage(std::string_view reason);

struct CodedRefusal {
    std::string code;
    nlohmann::json fields;
};
// Recognizes "<name>@<version>: <JSON object>" and nothing else.
std::optional<CodedRefusal> splitCodedRefusalMessage(std::string_view message);

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

struct RenderPipelineRequest {
    nlohmann::json authored_config = nlohmann::json::object();
    std::string source_name = "render pipeline";
};

struct RenderEnvironmentCapabilities {
    bool runtime_shader_compiler_enabled = false;
    RenderPipelineGraphVariant graph_variant =
        RenderPipelineGraphVariant::flat;
    GraphVariantPolicyCapabilities graph_variant_capabilities;
    PassFieldOwnershipCapabilities pass_field_ownership;
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

enum class RenderPipelineProvenanceSource {
    project,
    feature,
    engine,
};

struct RenderPassProvenance {
    std::string name;
    RenderPipelineProvenanceSource source =
        RenderPipelineProvenanceSource::engine;
    std::string provider_feature;
    std::string provider_reference;
    // Source declaration identity survives runtime variant namespacing.
    std::string authored_name;

    bool operator==(const RenderPassProvenance &) const = default;
};

struct RenderResourceProvenance {
    std::string name;
    std::string kind;
    RenderPipelineProvenanceSource source =
        RenderPipelineProvenanceSource::engine;
    std::string provider_feature;
    std::string provider_reference;
    // Present only when the resolved declaration publishes image usage.
    // Synthetic resources such as the swapchain deliberately remain
    // unknown instead of being represented as an empty usage set.
    std::optional<std::vector<std::string>> usage;

    bool operator==(const RenderResourceProvenance &) const = default;
};

std::string renderPipelineProvenanceSourceName(
    RenderPipelineProvenanceSource source,
    std::string_view provider_feature = {});

// Keeps compose-time ownership attached to the declarations which survived
// later graph transforms, drops removed declarations, and marks declarations
// introduced by the engine after feature composition.
void synchronizeRenderPipelineProvenance(
    const nlohmann::json &config,
    std::vector<RenderPassProvenance> &passes,
    std::vector<RenderResourceProvenance> &resources,
    RenderPipelineProvenanceSource default_source =
        RenderPipelineProvenanceSource::engine);

// Every callback is a data transform or lookup. Callers snapshot mutable
// environment values and expose lookup services explicitly instead of making
// the resolver discover modules or containers. This keeps the resolver usable
// in CPU-only tests; the resolver itself has no path for publishing
// GPU/runtime state.
struct RenderPipelineResolveDependencies {
    std::function<std::string(std::string_view)> load_feature_json;
    RenderPipelinePresetLoader load_pipeline_json;
    std::function<nlohmann::json(
        const nlohmann::json &, const std::vector<std::string> &)>
        normalize_config;
    std::function<void(nlohmann::json &)> transform_config;
    std::function<void(const nlohmann::json &)> validate_config;
    std::function<nlohmann::json(
        const nlohmann::json &,
        const CompiledGraphVariantPolicy &)>
        resolve_render_strategy;
    std::function<void(std::string_view, const nlohmann::json &)>
        validate_feature;
    std::set<std::string> inactive_graph_scopes;
};

struct ResolvedRenderPipeline {
    nlohmann::json normalized_config = nlohmann::json::object();
    std::vector<std::string> shader_defines;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::optional<nlohmann::json> projection_jitter;
    nlohmann::json feature_instances = nlohmann::json::array();
    nlohmann::json surface_resource_contracts =
        nlohmann::json::array();
    nlohmann::json material_routing;
    nlohmann::json draw_sort;
    SampleCountPolicy sample_count_policy;
    TargetPlanningPolicy target_planning;
    std::vector<VulkanTargetPlanPinPackage>
        vulkan_plan_pins;
    std::vector<VulkanPhysicalFragmentPackage>
        vulkan_physical_fragments;
    CompiledXrTargetPolicy xr_target_policy;
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    CompiledGraphVariantPolicy graph_variant_policy;
    std::vector<GraphVariantFeatureDecision>
        graph_variant_feature_decisions;
    std::vector<RenderPipelineDiagnostic> diagnostics;
    std::vector<RenderPassProvenance> pass_provenance;
    std::vector<RenderResourceProvenance> resource_provenance;
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

struct CompiledSurfaceResourceContract {
    MaterialPassInputContract contract;
    std::string resource;
    std::string producer;
    std::string provider_feature;
    std::string provider_reference;
    std::vector<std::string> material_consumers;
    std::vector<std::string> fullscreen_consumers;

    bool operator==(
        const CompiledSurfaceResourceContract &) const = default;
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
    std::optional<MaterialOutputSchema> output_schema;
    std::vector<MaterialOutputAttachmentState> output_states;
};

struct CompiledMaterialRouting {
    MaterialRoutingPolicy policy = MaterialRoutingPolicy::hybrid_auto_v1;
    std::vector<CompiledMaterialRoute> routes;
};

enum class DrawSortXrViewPolicy {
    logical_view_center,
    per_view,
};

std::string_view drawSortXrViewPolicyName(DrawSortXrViewPolicy policy);

struct CompiledDrawSortPolicy {
    std::string provider;
};

struct CompiledDrawSorting {
    CompiledDrawSortPolicy opaque{"state_batched_v1"};
    CompiledDrawSortPolicy transparent{"back_to_front_v1"};
    DrawSortXrViewPolicy xr_view_policy =
        DrawSortXrViewPolicy::logical_view_center;
    bool authored = false;
};

enum class RenderCompilerProgramMode : std::uint8_t {
    portable,
    mixed,
    backend_native,
};

std::string_view renderCompilerProgramModeName(
    RenderCompilerProgramMode mode);

// Identifies the one top-level compiler program which produced a runtime
// candidate. The backend is an open, namespaced identity rather than a
// closed GPU API enum; the current executable backend is "vulkan".
struct RenderCompilerProgramSelection {
    std::uint32_t schema_version = 1;
    std::string name;
    std::string implementation;
    std::string backend;
    RenderCompilerProgramMode mode =
        RenderCompilerProgramMode::portable;

    bool operator==(
        const RenderCompilerProgramSelection &) const =
        default;
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
    std::vector<CompiledSurfaceResourceContract>
        surface_resource_contracts;
    std::optional<CompiledMaterialRouting> material_routing;
    std::optional<CompiledLightingDataPlan>
        lighting_data;
    CompiledDrawSorting draw_sorting;
    SampleCountPolicy sample_count_policy;
    TargetPlanningPolicy target_planning;
    std::vector<SceneAccelerationStructureDefinition> scene_acceleration_structures;
    std::vector<VulkanTargetPlanPinPackage>
        vulkan_plan_pins;
    std::vector<VulkanPhysicalFragmentPackage>
        vulkan_physical_fragments;
    CompiledXrTargetPolicy xr_target_policy;
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    CompiledGraphVariantPolicy graph_variant_policy;
    std::vector<GraphVariantFeatureDecision>
        graph_variant_feature_decisions;
    std::vector<LogicalGraphTransformSelection>
        graph_transforms;
    std::optional<RenderStrategySelection>
        render_strategy;
    std::optional<RenderCompilerProgramSelection>
        render_compiler_program;
    // Root-authored, non-composed references used by renderer reload
    // dependency tracking. The parsed layout itself remains a core sidecar.
    std::optional<std::string> ray_group_layout_reference;
    std::optional<std::string> ray_bindings_reference;
    std::vector<RenderPipelineDiagnostic> diagnostics;
    std::vector<RenderPassProvenance> pass_provenance;
    std::vector<RenderResourceProvenance> resource_provenance;
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
