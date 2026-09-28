#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// Frame-plan node names are unique only inside one graph.  This pair is the
// complete UI identity; it deliberately is not a persistent/stable id.
struct FramePlanNodeKey {
    std::string graph;
    std::string name;

    auto operator<=>(const FramePlanNodeKey &) const = default;
};

struct FramePlanAttachmentOps {
    std::string resource;
    std::string aspect;
    std::string load_op;
    std::string store_op;

    bool operator==(const FramePlanAttachmentOps &) const = default;
};

struct FramePlanResourceUse {
    std::string resource;
    std::string epoch;
    std::string access;
    std::string intent;
    std::string footprint;

    bool operator==(const FramePlanResourceUse &) const = default;
};

struct FramePlanMaterialFilter {
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::vector<std::string> unmatched_include;
    std::vector<std::string> unmatched_exclude;
    std::string filter_id;
    std::string resolution_state;
    std::string resolution_provenance;
    std::optional<std::size_t> resolved_draw_count;

    bool operator==(const FramePlanMaterialFilter &) const = default;
};

enum class FramePlanShaderResolutionState {
    resolved,
    material_owned,
    not_applicable,
};

struct FramePlanShaderStage {
    std::string stage;
    std::optional<std::size_t> index;
    std::optional<std::string> declared_ref;
    std::string effective_ref;
    std::string origin;
    std::optional<std::string> source_open_ref;
    std::optional<std::string> source_open_reason;

    bool operator==(const FramePlanShaderStage &) const = default;
};

struct FramePlanHitShaderSlot {
    std::size_t hit_slot = 0;
    FramePlanShaderStage projection;
    std::string group_name;
    std::string ray_type;

    bool operator==(const FramePlanHitShaderSlot &) const = default;
};

using FramePlanAnyHitSlot = FramePlanHitShaderSlot;

struct FramePlanShaderResolution {
    FramePlanShaderResolutionState state =
        FramePlanShaderResolutionState::not_applicable;
    std::vector<FramePlanShaderStage> stages;
    std::optional<std::size_t> hit_slot_count;
    std::vector<FramePlanHitShaderSlot> closesthit_slots;
    std::vector<FramePlanAnyHitSlot> anyhit_slots;
    std::vector<FramePlanHitShaderSlot> intersection_slots;

    [[nodiscard]] bool resolved() const noexcept {
        return state == FramePlanShaderResolutionState::resolved;
    }

    bool operator==(const FramePlanShaderResolution &) const = default;
};

struct FramePlanRecordField {
    std::string name;
    std::string type;
    std::size_t offset = 0;
    std::string value;
    bool operator==(const FramePlanRecordField &) const = default;
};

struct FramePlanCallableRecord {
    std::size_t index = 0;
    std::vector<FramePlanRecordField> fields;
    bool operator==(const FramePlanCallableRecord &) const = default;
};

// VI-3a. One set-1 declaration of the shader a fullscreen / raster pass draws
// with (get_frame_plan nodes[].pass_inputs.declarations).
struct FramePlanPassInputDeclaration {
    std::size_t binding = 0;
    std::string type;
    std::string name;
    bool variadic = false;

    [[nodiscard]] bool buffer() const noexcept {
        return type.find("buffer") != std::string::npos;
    }

    bool operator==(const FramePlanPassInputDeclaration &) const = default;
};

struct FramePlanVariadicRange {
    std::size_t first = 0;
    std::size_t max = 0;

    bool operator==(const FramePlanVariadicRange &) const = default;
};

// VI-1b: a reload whose shader no longer matched the pass's inputs. The pass
// keeps drawing with its previous shader; `declarations` are the refused
// shader's.
struct FramePlanReloadRefusal {
    std::string reason;
    // The engine's shader_input_mismatch object, as it arrived.
    std::string shader_input_mismatch;
    std::vector<FramePlanPassInputDeclaration> declarations;

    bool operator==(const FramePlanReloadRefusal &) const = default;
};

// VI-1: what the pass's running pipeline declares at set 1, whether or not
// the pass supplies each binding. Only a fullscreen / raster pass carries it.
struct FramePlanPassInputs {
    // "declared" | "unavailable" (no pipeline; `reason` says why).
    std::string state;
    std::string fragment;
    std::string reason;
    std::vector<FramePlanPassInputDeclaration> declarations;
    std::optional<FramePlanVariadicRange> variadic_range;
    std::size_t pass_texture_inputs = 0;
    std::size_t pass_buffer_inputs = 0;
    std::size_t buffer_binding_base = 0;
    std::optional<FramePlanReloadRefusal> reload_refused;

    [[nodiscard]] bool declared() const noexcept {
        return state == "declared";
    }

    bool operator==(const FramePlanPassInputs &) const = default;
};

// LP-4a: a pass's `view_select` (LP-Va): the view axes it runs for, as
// get_frame_plan lists them. Empty when the node runs every view of its
// family.
struct FramePlanViewSelectAxis {
    std::string axis;
    std::uint64_t value = 0;

    bool operator==(const FramePlanViewSelectAxis &) const = default;
};

// LP-4b: one view axis of the node's view family, as get_frame_plan lists
// it (LP-Va2's view_axes). `length` is the count of views along the axis in
// the current generation, and is empty for an axis decided each frame
// (`runtime`), which cannot be selected. `labels`, when the engine gives
// them, name each value ("+X" for face 0).
struct FramePlanViewAxis {
    std::string axis;
    std::optional<std::uint64_t> length;
    bool runtime = false;
    std::vector<std::string> labels;

    bool operator==(const FramePlanViewAxis &) const = default;
};

// LP-4c: where an expanded pass, task or target came from (LP-1a's
// loop_origin): the graph.loop, the element that expanded it, and the node's
// id in the branch body's definition. Studio reads these rather than taking
// the expanded name (`<loop>/<element>/<node>`) apart.
struct FramePlanLoopOrigin {
    std::string loop;
    std::int64_t element = 0;
    std::string node;

    bool operator==(const FramePlanLoopOrigin &) const = default;
};

struct FramePlanLoopElement {
    std::int64_t element = 0;
    std::size_t index = 0;
    std::size_t branch = 0;
    // LP-4e: a draw_loop's element is a material, named by its matching key
    // (`<document>#<name>`, `<asset>#material[<index>]`); `element` is then 0.
    std::string key;

    bool operator==(const FramePlanLoopElement &) const = default;
};

// get_frame_plan's top-level loops[]: one per graph.loop, a loop that
// expanded nothing included.
struct FramePlanLoop {
    std::string node;
    std::string tier;
    std::size_t count = 0;
    std::vector<FramePlanLoopElement> elements;
    std::vector<std::size_t> unmatched_branches;
    // LP-4e: a draw_loop names the render.material_pass it draws in, and its
    // elements come a page at a time (LP-3b): `total` materials, of which
    // `elements` are the ones from `offset`. An expand loop has no page and
    // no `total` (0): elementCount() counts its elements then.
    std::string pass;
    std::size_t total = 0;
    std::size_t offset = 0;

    [[nodiscard]] std::size_t elementCount() const noexcept {
        return total > elements.size() ? total : elements.size();
    }

    bool operator==(const FramePlanLoop &) const = default;
};

// VF-2: the size a secondary view family's passes draw at, as get_frame_plan
// writes it on each node of that family -- family_extent ([w, h] for a
// fixed-size target, {"output_scale": s} for one relative to the output),
// family_layers, and family_extent_pass, the pass whose target sets it (the
// one VF-1's render.view_family_extent_mismatch@1 names as expected_pass).
// Absent on $main, and for a family no raster pass of which writes a
// declared target.
struct FramePlanFamilyExtent {
    // Set for a fixed size; empty for one relative to the output.
    std::optional<std::array<std::uint64_t, 2>> size;
    double output_scale = 1.0;
    std::uint64_t layers = 1;
    std::string pass;

    bool operator==(const FramePlanFamilyExtent &) const = default;
};

// Reads those three keys off one get_frame_plan node (`context` names it in
// the error). Nothing when the node carries no family_extent; throws
// std::runtime_error when it carries one Studio cannot read.
[[nodiscard]] std::optional<FramePlanFamilyExtent> framePlanFamilyExtent(
    const nlohmann::json &node, std::string_view context);

// Q-19: reads one get_frame_plan node's pass_inputs, as the Frame Plan does
// (`context` names it in the error). Nothing when the node carries none;
// throws std::runtime_error when it carries one Studio cannot read.
[[nodiscard]] std::optional<FramePlanPassInputs> framePlanPassInputs(
    const nlohmann::json &node, std::string_view context);

struct FramePlanNode {
    std::string name;
    std::string authored_name;
    std::string kind;
    std::string source;
    std::string provider_feature;
    std::string provider_reference;
    std::size_t declaration_index = 0;
    std::size_t order = 0;
    std::size_t level = 0;
    std::vector<std::string> reads;
    std::vector<std::string> history_reads;
    std::vector<std::string> writes;
    std::string view_family;
    std::vector<FramePlanViewSelectAxis> view_select;
    // LP-4b: the axes of the node's view family; empty for a family without
    // axes ($main, planar, an author's family) and for a plan that predates
    // view_axes.
    std::vector<FramePlanViewAxis> view_axes;
    // VF-2: the size the node's secondary view family draws at.
    std::optional<FramePlanFamilyExtent> family_extent;
    // LP-4a: the frame ran no view the node's view_select matches (the
    // engine's execution trace says so); the node recorded nothing.
    bool no_selected_view = false;
    // LP-4c: set for a pass or task a graph.loop expanded.
    std::optional<FramePlanLoopOrigin> loop_origin;
    std::string snapshot_after;
    std::size_t byte_size = 0;
    std::optional<FramePlanMaterialFilter> material_filter;
    std::string material_variant;
    FramePlanShaderResolution shader_resolution;
    std::string callable_record_source;
    std::vector<FramePlanCallableRecord> callable_records;
    // VI-3a: absent for a compute task, and for a pass the engine published
    // no pipeline declaration for.
    std::optional<FramePlanPassInputs> pass_inputs;

    // The public response may publish these directly. Current runtime output
    // carries the same high-level facts in physical_target_plan.attachments;
    // the builder normalizes both forms into these fields.
    std::string color_load_op;
    std::string color_store_op;
    std::string depth_load_op;
    std::string depth_store_op;
    std::vector<FramePlanAttachmentOps> attachments;

    // Backend-independent execution-plan facts. Backend-native target-plan
    // details intentionally do not enter this model.
    std::string semantic_dialect;
    std::string selected_implementation;
    std::string selected_endpoint;
    std::vector<std::string> required_capabilities;
    std::vector<FramePlanResourceUse> resource_uses;

    std::vector<std::size_t> incoming_barriers;
    std::vector<std::size_t> outgoing_barriers;

    bool operator==(const FramePlanNode &) const = default;
};

struct FramePlanBarrier {
    std::string kind;
    std::string resource;
    std::string from;
    std::string to;

    bool operator==(const FramePlanBarrier &) const = default;
};

struct FramePlanDependency {
    std::string from;
    std::string to;
    std::string reason;
    std::string resource;

    bool operator==(const FramePlanDependency &) const = default;
};

struct FramePlanExtent {
    std::string kind;
    double scale_x = 1.0;
    double scale_y = 1.0;
    std::size_t width = 0;
    std::size_t height = 0;

    bool operator==(const FramePlanExtent &) const = default;
};

struct FramePlanLifetime {
    bool used = false;
    std::optional<std::size_t> first_use;
    std::optional<std::size_t> last_use;

    bool operator==(const FramePlanLifetime &) const = default;
};

struct FramePlanResource {
    std::string name;
    std::string kind;
    std::string source;
    // Missing in an older frame plan (or unavailable from the producer) is
    // distinct from a present, explicitly empty usage array.
    std::optional<std::vector<std::string>> usage;
    std::string format = "unknown";
    std::string dimension;
    std::optional<FramePlanExtent> extent;
    std::optional<std::size_t> width;
    std::optional<std::size_t> height;
    std::string alias_group;
    std::string pattern;
    std::string representation;
    std::string widest_read;
    FramePlanLifetime lifetime;
    bool stored = false;
    bool aliasable = false;
    std::vector<std::string> required_physical_features;
    std::string view_layout;
    std::size_t array_layers = 0;
    std::string mip_level_mode;
    std::size_t mip_level_count = 0;
    std::optional<std::size_t> rasterization_samples;
    std::optional<bool> resolve_required;
    std::string reason;
    std::vector<std::string> readers;
    std::vector<std::string> history_readers;
    std::vector<std::string> writers;
    std::string provider_feature;
    std::string provider_reference;
    std::string sampling;
    std::string view_policy;
    std::string fallback;
    std::vector<std::string> material_consumers;
    std::vector<std::string> fullscreen_consumers;
    // LP-4c: set for a target or buffer a graph.loop expanded.
    std::optional<FramePlanLoopOrigin> loop_origin;
    // GL-2a: the frame plan nodes physical_target_plan.resources[] names
    // for it (its readers, writers and attachments), in the plan's order.
    std::vector<std::string> from_nodes;

    bool operator==(const FramePlanResource &) const = default;
};

struct FramePlanMaterialRoute {
    std::string route;
    std::string pass;
    std::string contract;
    std::string shader_contract;
    std::string phase;

    bool operator==(const FramePlanMaterialRoute &) const = default;
};

struct FramePlanDecision {
    std::string id;
    std::string subject;
    std::string selected;
    std::string detail;
    // GL-2a: the frame plan nodes its subject resolves to; empty for a
    // decision about the whole graph.
    std::vector<std::string> from_nodes;

    bool operator==(const FramePlanDecision &) const = default;
};

struct FramePlanDecisionGroup {
    std::string subject;
    std::vector<FramePlanDecision> decisions;

    bool operator==(const FramePlanDecisionGroup &) const = default;
};

struct FramePlanBackendFailure {
    std::string id;
    std::string subject;
    std::string detail;

    bool operator==(const FramePlanBackendFailure &) const = default;
};

struct FramePlanPlanningDiagnostic {
    std::string id;
    std::string severity;
    std::string subject;
    std::string detail;

    bool operator==(const FramePlanPlanningDiagnostic &) const = default;
};

struct FramePlanBackendCandidate {
    std::string candidate;
    std::string endpoint;
    bool feasible = false;
    bool selected = false;
    std::vector<FramePlanBackendFailure> failures;
    std::vector<FramePlanPlanningDiagnostic> diagnostics;
    // GL-2a: the frame plan nodes it concerns (so far always empty).
    std::vector<std::string> from_nodes;

    bool operator==(const FramePlanBackendCandidate &) const = default;
};

struct FramePlanLoweringNode {
    std::string name;
    std::string kind;
    std::string dialect;
    std::vector<std::string> sources;
    std::vector<std::string> regions;
    std::vector<std::string> required_physical_features;

    bool operator==(const FramePlanLoweringNode &) const = default;
};

struct FramePlanPhysicalScope {
    std::string id;
    std::string kind;
    std::vector<std::string> nodes;
    bool single_rendering_instance = false;
    std::vector<std::string> local_reads;
    std::vector<std::string> regions;
    std::string view_execution;
    std::size_t view_count = 0;
    std::size_t execution_count = 0;
    std::size_t view_mask = 0;
    std::optional<std::size_t> rasterization_samples;
    // LP-4a: the scope's passes run only for these view axes.
    std::vector<FramePlanViewSelectAxis> view_select;

    bool operator==(const FramePlanPhysicalScope &) const = default;
};

struct FramePlanAliasGroup {
    std::string id;
    std::vector<std::string> resources;
    // GL-2a: the frame plan nodes its members name, together.
    std::vector<std::string> from_nodes;

    bool operator==(const FramePlanAliasGroup &) const = default;
};

struct FramePlanOpportunityPair {
    std::string first;
    std::string second;
    bool adopted = false;

    bool operator==(const FramePlanOpportunityPair &) const = default;
};

struct FramePlanResolutionPlan {
    std::string render_source_resource;
    FramePlanExtent render_extent;
    std::string output_source_resource;
    FramePlanExtent output_extent;
    std::vector<std::string> scene_resources;

    bool operator==(const FramePlanResolutionPlan &) const = default;
};

struct FramePlanWireSection {
    std::string name;
    std::string json;

    bool operator==(const FramePlanWireSection &) const = default;
};

enum class FramePlanExecutionPlanState {
    unavailable,
    available,
};

struct FramePlanExecutionPlan {
    FramePlanExecutionPlanState state =
        FramePlanExecutionPlanState::unavailable;
    std::string unavailable_reason_code = "execution_plan_missing";
    std::string unavailable_reason =
        "execution_plan_missing: execution_plan was not published";
    std::string schema;
    std::size_t schema_version = 0;
    std::string graph;
    std::string fingerprint;

    [[nodiscard]] bool available() const noexcept {
        return state == FramePlanExecutionPlanState::available;
    }

    bool operator==(const FramePlanExecutionPlan &) const = default;
};

enum class FramePlanPhysicalPlanState {
    unavailable,
    available,
};

enum class FramePlanLoweringGraphState {
    unavailable,
    available,
};

struct FramePlanPhysicalPlan {
    FramePlanPhysicalPlanState state =
        FramePlanPhysicalPlanState::unavailable;
    std::string unavailable_reason_code = "physical_plan_missing";
    std::string unavailable_reason =
        "physical_plan_missing: physical_target_plan was not published";
    std::string schema;
    std::size_t version = 0;
    std::string graph;
    std::string logical_graph_fingerprint;
    std::string automatic_plan_fingerprint;
    std::string planning_profile;
    // Endpoint of the selected backend candidate.  Planning decisions are
    // only meaningful together with both this device-facing identity and the
    // profile above; neither is a property of the authored logical graph.
    std::string planning_endpoint;
    std::optional<std::size_t> output_width;
    std::optional<std::size_t> output_height;
    // Lowering membership remains usable when only a later physical-plan
    // projection (for example runtime resolution) is unavailable. Conversely,
    // an absent physical plan must not look like a successfully joined graph
    // with zero authored regions.
    FramePlanLoweringGraphState lowering_graph_state =
        FramePlanLoweringGraphState::unavailable;
    std::string lowering_graph_unavailable_reason_code =
        "physical_plan_missing";
    std::string lowering_graph_unavailable_reason =
        "physical_plan_missing: physical_target_plan was not published";
    std::vector<FramePlanLoweringNode> lowering_nodes;
    std::vector<FramePlanPhysicalScope> scopes;
    std::vector<FramePlanAliasGroup> alias_groups;
    std::vector<FramePlanOpportunityPair> alias_candidates;
    std::vector<FramePlanOpportunityPair> fusion_candidates;
    std::vector<FramePlanOpportunityPair> parallel_candidates;
    std::optional<FramePlanResolutionPlan> resolution_plan;
    // Every top-level key/value from the current wire document is retained.
    // Promoted fields above are convenient typed projections; this inventory
    // makes key-set coverage mechanically checkable and prevents silent loss
    // when the producer adds another current-version section.
    std::vector<FramePlanWireSection> wire_sections;

    [[nodiscard]] bool available() const noexcept {
        return state == FramePlanPhysicalPlanState::available;
    }

    [[nodiscard]] bool loweringGraphAvailable() const noexcept {
        return lowering_graph_state ==
               FramePlanLoweringGraphState::available;
    }

    bool operator==(const FramePlanPhysicalPlan &) const = default;
};

struct FramePlanGpuResource {
    std::string kind;
    std::uint64_t handle = 0;
    std::string name;
    std::size_t declared_bytes = 0;

    bool operator==(const FramePlanGpuResource &) const = default;
};

struct FramePlanGpuResourceScope {
    std::string owner_scope;
    std::size_t resource_lease_count = 0;
    std::vector<FramePlanGpuResource> resources;

    bool operator==(const FramePlanGpuResourceScope &) const = default;
};

struct FramePlanGpuResourceArena {
    std::uint64_t runtime_generation = 0;
    std::size_t resource_count = 0;
    std::vector<FramePlanGpuResourceScope> scopes;

    bool operator==(const FramePlanGpuResourceArena &) const = default;
};

struct FramePlanModel {
    std::string profile;
    std::string graph;
    std::optional<std::uint64_t> runtime_generation;
    std::size_t response_bytes = 0;
    std::string raw_json;
    std::vector<FramePlanNode> nodes;
    // LP-4c: loops[], empty when the composed graph has no graph.loop.
    std::vector<FramePlanLoop> loops;
    std::vector<FramePlanBarrier> barriers;
    std::vector<FramePlanDependency> dependencies;
    std::vector<FramePlanResource> resources;
    std::vector<FramePlanMaterialRoute> material_routes;
    std::vector<FramePlanDecisionGroup> decision_groups;
    std::string selected_backend_candidate;
    std::vector<FramePlanBackendCandidate> backend_candidates;
    std::vector<FramePlanPlanningDiagnostic> backend_diagnostics;
    FramePlanExecutionPlan execution_plan;
    FramePlanPhysicalPlan physical_plan;
    std::optional<FramePlanGpuResourceArena> gpu_resource_arena;

    bool operator==(const FramePlanModel &) const = default;
};

// Builds the compact, view-independent projection used by Pelican Studio from
// the result value of get_frame_plan. Only pelican.frame_plan version 1 is
// accepted; old or future versions are not upgraded implicitly.
FramePlanModel buildFramePlanModel(std::string_view response_json);

// G-4 / GL-1d: the one judgment of when a new Forward opaque pass also sets
// the project's material_route_default to "forward" -- shared by the Frame
// Plan's material pass form and the Graph's palette, which gather the
// pipeline's material passes each from what they read.
//
// Whether a material pass of this contract takes the deferred_geometry route:
// a deferred_geometry_v1 pass, or a legacy G-buffer one (which accepts every
// material with a G-buffer shader contract). An empty contract is the legacy
// one, as a declaration without material_contract is.
bool materialContractTakesDeferredRoute(std::string_view contract);

// A Forward opaque pass in a pipeline with no pass for deferred materials:
// without the forward default, the engine's built-in materials and every
// automatic material have no pass to be drawn by, and the engine refuses the
// pass (material_route_unserved@1).
bool materialRouteOffersForwardDefault(std::string_view contract,
                                       bool pipeline_has_deferred_pass);

// The notice for a material_route_unserved@1 refusal: which materials have no
// pass, on which route, and what to do about it. Empty for any other error.
std::string materialRouteUnservedNotice(const nlohmann::json &error);

} // namespace PelicanStudio
