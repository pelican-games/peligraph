#pragma once

#include "logicalrendergraph.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Pelican {

enum class TargetEndpointKind : std::uint8_t {
    host,
    vulkan_device,
    external,
};

std::string_view targetEndpointKindName(TargetEndpointKind kind);

struct TargetFact {
    std::string name;
    std::string value;

    bool operator==(const TargetFact &) const = default;
};

struct TargetEndpoint {
    std::string id;
    TargetEndpointKind kind = TargetEndpointKind::vulkan_device;
    std::vector<std::string> capabilities;
    std::vector<TargetFact> facts;

    bool operator==(const TargetEndpoint &) const = default;
};

struct TargetEndpointLink {
    std::string id;
    std::string source_endpoint;
    std::string destination_endpoint;
    std::vector<std::string> capabilities;
    std::vector<std::string> bridge_offers;

    bool operator==(const TargetEndpointLink &) const = default;
};

struct TargetTopologySnapshot {
    std::string name;
    std::vector<TargetEndpoint> endpoints;
    std::vector<TargetEndpointLink> links;

    bool operator==(const TargetTopologySnapshot &) const = default;
};

TargetTopologySnapshot canonicalizeTargetTopology(TargetTopologySnapshot topology);
const TargetEndpoint *findTargetEndpoint(
    const TargetTopologySnapshot &topology, std::string_view id) noexcept;
const TargetEndpointLink *findTargetEndpointLink(
    const TargetTopologySnapshot &topology, std::string_view id) noexcept;
nlohmann::ordered_json targetTopologySnapshotToJson(
    const TargetTopologySnapshot &topology);

enum class CompilerProviderKind : std::uint8_t {
    conversion,
    target_lowering,
};

std::string_view compilerProviderKindName(CompilerProviderKind kind);

struct CompilerProviderDescriptor {
    std::string id;
    CompilerProviderKind kind = CompilerProviderKind::target_lowering;
    std::uint64_t owner_identity = 0;
    std::uint32_t owner_generation = 0;
    std::string content_hash;

    bool operator==(const CompilerProviderDescriptor &) const = default;
};

std::string compilerProviderFingerprint(
    const CompilerProviderDescriptor &provider);

class CompilerProviderLease {
    CompilerProviderDescriptor descriptor_;
    std::shared_ptr<const void> generation_lease_;

    CompilerProviderLease(CompilerProviderDescriptor descriptor,
                          std::shared_ptr<const void> generation_lease);
    friend class CompilerProviderRegistry;

  public:
    CompilerProviderLease() = default;

    const CompilerProviderDescriptor &descriptor() const noexcept {
        return descriptor_;
    }
    bool holdsGenerationLease() const noexcept {
        return generation_lease_ != nullptr;
    }
};

class CompilerProviderRegistrySnapshot {
    std::vector<CompilerProviderLease> providers_;

    explicit CompilerProviderRegistrySnapshot(
        std::vector<CompilerProviderLease> providers);
    friend class CompilerProviderRegistry;

  public:
    CompilerProviderRegistrySnapshot() = default;

    std::span<const CompilerProviderLease> providers() const noexcept {
        return providers_;
    }
    const CompilerProviderLease &require(
        CompilerProviderKind kind, std::string_view id) const;
};

class CompilerProviderRegistry {
    mutable std::shared_mutex mutex_;
    std::vector<CompilerProviderLease> providers_;

  public:
    CompilerProviderRegistry() = default;
    CompilerProviderRegistry(const CompilerProviderRegistry &) = delete;
    CompilerProviderRegistry &operator=(
        const CompilerProviderRegistry &) = delete;

    void registerProvider(
        CompilerProviderDescriptor descriptor,
        std::shared_ptr<const void> generation_lease = {});
    bool unregisterProvider(CompilerProviderKind kind, std::string_view id);
    CompilerProviderRegistrySnapshot snapshot() const;
};

enum class PlanningDiagnosticSeverity : std::uint8_t {
    info,
    warning,
    error,
};

std::string_view planningDiagnosticSeverityName(
    PlanningDiagnosticSeverity severity);

struct PlanningDiagnostic {
    std::string id;
    PlanningDiagnosticSeverity severity =
        PlanningDiagnosticSeverity::warning;
    std::string subject;
    std::string detail;

    bool operator==(const PlanningDiagnostic &) const = default;
};

struct PlanningDiagnosticPolicy {
    std::vector<std::string> strict_warning_ids;

    bool operator==(const PlanningDiagnosticPolicy &) const = default;
};

std::vector<PlanningDiagnostic> applyPlanningDiagnosticPolicy(
    std::span<const PlanningDiagnostic> diagnostics,
    const PlanningDiagnosticPolicy &policy = {});
void requireNoPlanningErrors(
    std::span<const PlanningDiagnostic> diagnostics);

struct TargetBridgeRequest {
    std::string source_endpoint;
    std::string destination_endpoint;
    std::vector<std::string> required_link_capabilities;

    bool operator==(const TargetBridgeRequest &) const = default;
};

struct BackendCostEstimate {
    std::uint32_t rendering_scopes = 0;
    std::uint32_t materialized_resources = 0;
    std::uint32_t external_stores = 0;
    std::uint64_t transient_bytes = 0;
    std::uint32_t bandwidth_class = 0;

    bool operator==(const BackendCostEstimate &) const = default;
};

struct BackendProbeInput {
    std::string candidate;
    std::string provider;
    CompilerProviderKind provider_kind =
        CompilerProviderKind::target_lowering;
    std::string endpoint;
    std::vector<std::string> required_endpoint_capabilities;
    std::optional<TargetBridgeRequest> bridge;
    std::vector<std::string> required_physical_features;
    BackendCostEstimate cost;
    std::vector<PlanningDiagnostic> diagnostics;
};

struct BackendConstraintFailure {
    std::string id;
    std::string subject;
    std::string detail;

    bool operator==(const BackendConstraintFailure &) const = default;
};

struct BackendProbeResult {
    std::string candidate;
    CompilerProviderDescriptor provider;
    std::string endpoint;
    bool feasible = false;
    std::optional<std::string> selected_link;
    std::vector<std::string> bridge_offers;
    std::vector<std::string> required_physical_features;
    BackendCostEstimate cost;
    std::vector<BackendConstraintFailure> failures;
    std::vector<PlanningDiagnostic> diagnostics;
};

BackendProbeResult probeVulkanBackend(
    const TargetTopologySnapshot &topology,
    const CompilerProviderRegistrySnapshot &providers,
    BackendProbeInput input);

struct PlanningDecision {
    std::string id;
    std::string subject;
    std::string selected;
    std::string detail;

    bool operator==(const PlanningDecision &) const = default;
};

struct BackendSelection {
    std::string selected_candidate;
    std::vector<BackendProbeResult> candidates;
    std::vector<PlanningDiagnostic> diagnostics;
    std::vector<PlanningDecision> decisions;
};

BackendSelection selectBackendCandidate(
    std::vector<BackendProbeResult> candidates,
    const PlanningDiagnosticPolicy &diagnostic_policy = {},
    std::optional<std::string_view> pinned_candidate =
        std::nullopt);
nlohmann::ordered_json backendSelectionToJson(
    const BackendSelection &selection);

enum class PlanningProfileKind : std::uint8_t {
    optimized,
    conservative_debug,
    hazard_stress,
};

std::string_view planningProfileKindName(PlanningProfileKind kind);

struct PlanningProfile {
    PlanningProfileKind kind = PlanningProfileKind::optimized;
    std::uint64_t seed = 0;

    bool operator==(const PlanningProfile &) const = default;
};

struct PlanningNodeConstraint {
    std::string node;
    bool serial = false;
    bool isolate = false;

    bool operator==(const PlanningNodeConstraint &) const = default;
};

struct PlanningResourceConstraint {
    std::string resource;
    bool no_alias = false;

    bool operator==(const PlanningResourceConstraint &) const = default;
};

// Authoring-facing controls shared by target compilers. Constraints are
// grouped by graph so a pipeline containing preview, XR, and presentation
// graphs does not accidentally apply a node/resource name to every graph.
// The profile and diagnostic policy are pipeline-wide defaults; a later
// physical-plan package may pin compiler-specific choices without widening
// this portable surface.
struct PlanningGraphConstraints {
    std::string graph;
    std::vector<std::string> required_capabilities;
    std::vector<PlanningNodeConstraint> nodes;
    std::vector<PlanningResourceConstraint> resources;

    bool operator==(const PlanningGraphConstraints &) const = default;
};

struct TargetPlanningPolicy {
    PlanningProfile profile;
    PlanningDiagnosticPolicy diagnostic_policy;
    std::vector<PlanningGraphConstraints> graphs;
    bool authored = false;

    bool operator==(const TargetPlanningPolicy &) const = default;
};

struct PlanningNamePair {
    std::string first;
    std::string second;

    bool operator==(const PlanningNamePair &) const = default;
    bool operator<(const PlanningNamePair &other) const {
        if (first != other.first) return first < other.first;
        return second < other.second;
    }
};

struct LogicalPlanningOpportunityInput {
    PlanningProfile profile;
    std::vector<PlanningNodeConstraint> node_constraints;
    std::vector<PlanningResourceConstraint> resource_constraints;
    // These pairs have already passed target-specific lifetime/compatibility
    // checks. RPE6c0 only applies policy; RPE6c1 will derive them.
    std::vector<PlanningNamePair> legal_alias_candidates;
};

struct LogicalPlanningOpportunityReport {
    std::string graph;
    PlanningProfile profile;
    std::vector<std::string> node_order;
    std::vector<PlanningNamePair> parallel_candidates;
    std::vector<PlanningNamePair> fusion_candidates;
    std::vector<PlanningNamePair> alias_candidates;
    std::vector<PlanningDecision> decisions;
};

LogicalPlanningOpportunityReport analyzeLogicalPlanningOpportunities(
    const CompiledLogicalRenderGraph &graph,
    LogicalPlanningOpportunityInput input = {});
nlohmann::ordered_json logicalPlanningOpportunityReportToJson(
    const LogicalPlanningOpportunityReport &report);

} // namespace Pelican
