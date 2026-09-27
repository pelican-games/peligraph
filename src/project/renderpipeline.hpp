#pragma once

#include "graphcomposition.hpp"
#include "graphvariantpolicy.hpp"
#include "lightingdata.hpp"
#include "logicalrendergraph.hpp"
#include "materialscreeninput.hpp"
#include "materialoutput.hpp"
#include "passfieldownership.hpp"
#include "renderloopreport.hpp"
#include "samplecountplanning.hpp"
#include "sceneaccelerationstructure.hpp"
#include "targetplanning.hpp"
#include "targetrenderplanning.hpp"
#include "xrtargetpolicy.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
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
    // LP-0d: subject is the graph node, detail
    // "<variant>:<reason>:<excluded feature>" (RenderGraphDeactivatedNode).
    graph_node_deactivated,
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

// GL-2a: a feature's pass or compute task a graph node took the place of
// (a feature instance's `pass_replacements`).
struct RenderPassReplacedDeclaration {
    std::string feature;
    std::string pass;

    bool operator==(const RenderPassReplacedDeclaration &) const = default;
};

struct RenderPassProvenance {
    std::string name;
    RenderPipelineProvenanceSource source =
        RenderPipelineProvenanceSource::engine;
    std::string provider_feature;
    std::string provider_reference;
    // Source declaration identity survives runtime variant namespacing.
    std::string authored_name;
    // LP-1a: set on a pass or compute task a graph.loop expanded.
    std::optional<RenderLoopOrigin> loop_origin;
    // GL-2a: set on a pass or compute task a node of the rendering graph
    // made -- a root node, a node of an instance's definition, or (with
    // loop_origin) a loop body's node. Absent on the preset's, a feature's
    // and the rendering section's own passes, whatever `source` says. For a
    // loop's copy, `instance_path` is empty (a body is no instance) and
    // `loop` / `loop_branch` say which loop and branch; the element is
    // loop_origin's.
    std::optional<GraphNodeOrigin> graph_origin;
    // GL-2a: the feature passes this one took the place of, in the order
    // the instance named them (one node may replace several).
    std::vector<RenderPassReplacedDeclaration> replaces;

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
    // LP-1a: set on a render target or buffer a graph.loop expanded.
    std::optional<RenderLoopOrigin> loop_origin;

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

struct CompiledRenderFeatureInstance;
struct CompiledSurfaceResourceContract;

// LP-Vb: the length of one axis of a view family (docs/design_graph_loops.md
// 7.1), as a graph.loop over view_axis needs it when the graph is resolved.
// The view family providers live in the engine core, which this layer does
// not reach; the engine hands the lookup in (renderGraphViewAxisLengthResolver
// in the core's viewfamilyproviderregistry.hpp).
enum class RenderGraphViewAxisKind : std::uint8_t {
    // No provider of the family declares the axis (an authored family has
    // no axes at all).
    unknown,
    // The axis is known only per frame (a light count).
    runtime,
    // Fixed when the render graph is (re)compiled.
    compile_time,
};

struct RenderGraphViewAxisLength {
    RenderGraphViewAxisKind kind = RenderGraphViewAxisKind::unknown;
    std::uint32_t length = 0;

    bool operator==(const RenderGraphViewAxisLength &) const = default;
};

// family, axis, and what the composed pipeline says so far (the feature
// instances with their bound parameters, the surface resource contracts):
// what a provider's compile-time length reads.
using RenderGraphViewAxisLengthResolver = std::function<RenderGraphViewAxisLength(
    std::string_view family, std::string_view axis,
    const std::vector<CompiledRenderFeatureInstance> &feature_instances,
    const std::vector<CompiledSurfaceResourceContract> &surface_resource_contracts)>;

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
    // LP-Vb: a graph.loop over view_axis is sized with this; left empty, such
    // a loop is refused when the graph is resolved.
    RenderGraphViewAxisLengthResolver view_axis_length;
};

struct MaterialDrawLoopRules;

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
    // LP-1a: every graph.loop of the composed graph (empty when it has none).
    std::vector<RenderGraphLoopReport> graph_loops;
    // LP-3a: the graph's draw_loop rules, handed on to the compiled pipeline.
    std::shared_ptr<const MaterialDrawLoopRules> material_draw_loops;
    // LP-0d: the graph nodes this variant made inactive because they depend
    // on a feature it excluded (also in `diagnostics`).
    std::vector<RenderGraphDeactivatedNode> deactivated_graph_nodes;
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

// LP-Vb: what a view family's compile-time axis length reads, taken from the
// composition's JSON while the graph is resolved (before the pipeline is
// compiled): the feature instances as the pipeline compiles them, and of each
// surface resource contract its name, resource, producer, provider and
// consumers -- nothing more is checked; the pipeline's own compilation checks
// the rest later.
struct RenderViewAxisLengthInputs {
    std::vector<CompiledRenderFeatureInstance> feature_instances;
    std::vector<CompiledSurfaceResourceContract> surface_resource_contracts;
};

RenderViewAxisLengthInputs compileRenderViewAxisLengthInputs(
    const nlohmann::json &feature_instances,
    const nlohmann::json &surface_resource_contracts);

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

struct MaterialDrawLoopRules;

// VF-2: the extent the raster passes of one secondary view family (not
// `$main`) are compared against at recompile (VF-1): the first raster
// attachment, in declaration order, whose target has a definition. The
// target's extent and layer count are as authored. Filled by the core's
// renderViewFamilyExtentReferences; get_frame_plan reports it on the
// family's nodes.
struct RenderViewFamilyExtentReference {
    std::string family;
    std::string pass;
    std::string target;
    // [width, height] when the target is fixed-size; otherwise it is sized
    // from the output by `extent_scale`.
    std::optional<std::array<std::uint32_t, 2>> fixed_extent;
    float extent_scale = 1.0f;
    std::uint32_t layers = 1;

    bool operator==(const RenderViewFamilyExtentReference &) const =
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
    // LP-2: the draw-loop rules of this pipeline's material passes (LP-3a
    // fills them from graph.loop). A generation built from it evaluates
    // them for every registered material (materialdrawloop.hpp).
    std::shared_ptr<const MaterialDrawLoopRules> material_draw_loops;
    // LP-1a: get_frame_plan's `loops[]` (absent when the graph has no loop).
    std::vector<RenderGraphLoopReport> graph_loops;
    // LP-0d: the graph nodes this variant made inactive.
    std::vector<RenderGraphDeactivatedNode> deactivated_graph_nodes;
    // VF-2: one entry per secondary view family with a raster pass writing a
    // declared target, in the order the families are first written. Set by
    // the render compiler next to VF-1's check.
    std::vector<RenderViewFamilyExtentReference> view_family_extents;
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
