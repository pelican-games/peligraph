#pragma once

#include "targetrenderplanning.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican {

// NativeScope is deliberately a data contract. It does not freeze a game-DLL
// callback ABI or carry Vulkan handles. A backend executor provider may later
// consume implementation + implementation_config after this boundary has
// passed device and graph verification.
enum class VulkanNativeScopeSynchronizationMode : std::uint8_t {
    automatic,
    manual,
    unchecked,
};

std::string_view vulkanNativeScopeSynchronizationModeName(
    VulkanNativeScopeSynchronizationMode mode);

enum class VulkanNativeScopeResourceOwnership : std::uint8_t {
    engine,
    native_scope,
};

std::string_view vulkanNativeScopeResourceOwnershipName(
    VulkanNativeScopeResourceOwnership ownership);

struct VulkanNativeScopeResourceBoundary {
    std::string logical_resource;
    std::string semantic_type;
    LogicalAccessMode access = LogicalAccessMode::read;
    VulkanNativeScopeResourceOwnership ownership =
        VulkanNativeScopeResourceOwnership::engine;
    // Vulkan stage/access spellings are intentionally open strings. They are
    // required only for manual synchronization; automatic mode derives the
    // minimal outside transition from the typed access.
    std::vector<std::string> stages;
    std::vector<std::string> accesses;

    bool operator==(
        const VulkanNativeScopeResourceBoundary &) const = default;
};

struct VulkanNativeScopeDeclaration {
    // References one VulkanPhysicalScopePlan::id. That scope owns execution
    // order and node coverage; this declaration replaces only its backend
    // implementation.
    std::string scope;
    std::string implementation;
    std::string queue_capability =
        "pelican.vulkan.graphics@1";
    VulkanNativeScopeSynchronizationMode synchronization =
        VulkanNativeScopeSynchronizationMode::automatic;
    std::vector<VulkanNativeScopeResourceBoundary> resources;
    std::vector<std::string> alias_groups;
    std::vector<std::string> required_features;
    std::vector<std::string> required_extensions;
    bool capture_compatible = false;
    bool device_loss_recoverable = false;
    bool hot_reloadable = false;
    nlohmann::ordered_json implementation_config =
        nlohmann::ordered_json::object();

    bool operator==(
        const VulkanNativeScopeDeclaration &) const = default;
};

// A complete same-layer physical replacement for the current logical graph
// and target environment. Unlike VulkanPhysicalFragmentPackage, every
// engine-visible physical resource, scope, attachment, and alias group is
// present. Backend-private resources and commands remain inside NativeScope
// implementation_config and are opaque outside its declared boundary.
struct VulkanCompletePhysicalPlanPackage {
    std::uint32_t schema_version = 1;
    std::string graph;
    std::uint64_t logical_graph_fingerprint = 0;
    std::uint64_t automatic_plan_fingerprint = 0;
    std::string backend_candidate;
    std::vector<VulkanPhysicalResourcePlan> resources;
    std::vector<VulkanPhysicalScopePlan> scopes;
    std::vector<VulkanPhysicalAttachmentPlan> attachments;
    std::vector<VulkanAliasGroupPlan> alias_groups;
    std::vector<std::string> required_physical_features;
    std::optional<VulkanExternalDepthExportPlan>
        external_depth_export;
    std::vector<VulkanNativeScopeDeclaration> native_scopes;

    bool operator==(
        const VulkanCompletePhysicalPlanPackage &) const = default;
};

struct VerifiedVulkanCompletePhysicalPlanPackage {
    VulkanCompletePhysicalPlanPackage package;
    std::uint64_t package_fingerprint = 0;
    std::vector<PlanningDiagnostic> diagnostics;
};

VulkanCompletePhysicalPlanPackage
ejectVulkanCompletePhysicalPlanPackage(
    const VulkanTargetPlan &plan);

nlohmann::ordered_json
vulkanCompletePhysicalPlanPackageToJson(
    const VulkanCompletePhysicalPlanPackage &package);

VulkanCompletePhysicalPlanPackage
vulkanCompletePhysicalPlanPackageFromJson(
    const nlohmann::json &document);

std::uint64_t vulkanCompletePhysicalPlanPackageFingerprint(
    const VulkanCompletePhysicalPlanPackage &package);

// Produces a marker type only after graph coverage, dependency order,
// lifetime/alias legality, attachment contracts, device capabilities,
// environment freshness, and NativeScope boundaries have all closed.
// enabled_extensions must be the selected device's exact enabled extension
// set when a NativeScope declares extensions.
VerifiedVulkanCompletePhysicalPlanPackage
verifyVulkanCompletePhysicalPlanPackage(
    const CompiledLogicalRenderGraph &canonical_graph,
    const TargetTopologySnapshot &topology,
    const VulkanTargetPlan &automatic_plan,
    VulkanCompletePhysicalPlanPackage package,
    std::span<const VulkanPhysicalResourceFormatCapability>
        format_capabilities = {},
    std::span<const std::string> enabled_extensions = {});

// Reifies a verified same-layer replacement as the VulkanTargetPlan consumed
// by the current runtime. The automatic plan keeps compiler provenance and
// target-environment decisions; only verifier-owned physical fields change.
VulkanTargetPlan applyVerifiedVulkanCompletePhysicalPlanPackage(
    const VulkanTargetPlan &automatic_plan,
    const VerifiedVulkanCompletePhysicalPlanPackage &verified);

// Rejects a custom compiler package before mutable GPU registration when its
// runtime target plan and verified complete artifact have drifted apart.
void validateVulkanTargetPlanMatchesCompletePhysicalPackage(
    const VulkanTargetPlan &target_plan,
    const VerifiedVulkanCompletePhysicalPlanPackage &verified);

} // namespace Pelican
