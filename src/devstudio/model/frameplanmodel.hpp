#pragma once

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

struct FramePlanShaderResolution {
    FramePlanShaderResolutionState state =
        FramePlanShaderResolutionState::not_applicable;
    std::vector<FramePlanShaderStage> stages;

    [[nodiscard]] bool resolved() const noexcept {
        return state == FramePlanShaderResolutionState::resolved;
    }

    bool operator==(const FramePlanShaderResolution &) const = default;
};

struct FramePlanNode {
    std::string name;
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
    std::string snapshot_after;
    std::size_t byte_size = 0;
    std::optional<FramePlanMaterialFilter> material_filter;
    std::string material_variant;
    FramePlanShaderResolution shader_resolution;

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

    bool operator==(const FramePlanPhysicalScope &) const = default;
};

struct FramePlanAliasGroup {
    std::string id;
    std::vector<std::string> resources;

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

} // namespace PelicanStudio
