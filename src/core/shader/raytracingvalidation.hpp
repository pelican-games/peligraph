#pragma once

#include "rtspirvinspection.hpp"
#include "raygrouplayout.hpp"
#include "../vkcore/runtimecapabilities.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Pelican {

// Pure validation entry points. Synthetic facts are accepted only here;
// production wiring obtains the fact object from VulkanManageCore.
void validateRayTracingRequest(
    std::uint32_t requested_depth,
    const RayTracingRuntimeFacts &facts);
void validateTraceExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts);

struct RayTracingShaderOccurrence {
    ShaderStage stage = ShaderStage::raygen;
    std::uint32_t group_index = 0;
    std::string_view diagnostic_uri;
    const RayTracingEntryPointReflection *entry = nullptr;
};

struct RayTracingPipelineInterfaceRequest {
    RayTracingShaderOccurrence raygen;
    std::span<const RayTracingShaderOccurrence> misses;
    std::span<const RayTracingShaderOccurrence> hit_groups;
    const RayGroupLayout *group_layout = nullptr;
    std::span<const std::uint32_t> traced_ray_type_indices;
};

// Returns a stable digest of the validated structural contract. Equality is
// always canonical-tree equality; the digest is evidence/indexing only.
std::string validateRayTracingPipelineInterfaces(
    const RayTracingPipelineInterfaceRequest &request,
    const RayTracingRuntimeFacts &facts);

enum class RayTracingValidationSite {
    pipeline_build,
    initial_extent,
    rebind,
    final_command,
};

struct RayTracingValidationObservation {
    RayTracingValidationSite site =
        RayTracingValidationSite::pipeline_build;
    RayTracingRuntimeFacts facts;
    std::optional<std::uint32_t> requested_depth;
    std::optional<std::array<std::uint32_t, 3>> extent;
};

using RayTracingValidationObserver =
    std::function<void(const RayTracingValidationObservation &)>;

// Observation is thread-local and cannot replace production facts.
class ScopedRayTracingValidationObserver {
    RayTracingValidationObserver previous_;

  public:
    explicit ScopedRayTracingValidationObserver(
        RayTracingValidationObserver observer);
    ~ScopedRayTracingValidationObserver();
    ScopedRayTracingValidationObserver(
        const ScopedRayTracingValidationObserver &) = delete;
    ScopedRayTracingValidationObserver &operator=(
        const ScopedRayTracingValidationObserver &) = delete;
};

// Test-only seam for proving each production validation delegate is live.
// Every site is enabled by default; a scoped override is thread-local.
class ScopedRayTracingValidationDelegateOverrideForTesting {
    RayTracingValidationSite site_;
    bool previous_ = true;

  public:
    ScopedRayTracingValidationDelegateOverrideForTesting(
        RayTracingValidationSite site, bool enabled);
    ~ScopedRayTracingValidationDelegateOverrideForTesting();
    ScopedRayTracingValidationDelegateOverrideForTesting(
        const ScopedRayTracingValidationDelegateOverrideForTesting &) =
        delete;
    ScopedRayTracingValidationDelegateOverrideForTesting &operator=(
        const ScopedRayTracingValidationDelegateOverrideForTesting &) =
        delete;
};

void validateProductionRayTracingRequest(
    std::uint32_t requested_depth,
    const RayTracingRuntimeFacts &facts);
void validateProductionTraceExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts,
    RayTracingValidationSite site);

// Site-specific production entry points make all four mandatory wires
// independently observable and mutation-testable.
void validateRayTracingPipelineBuild(
    std::uint32_t requested_depth,
    const RayTracingRuntimeFacts &facts);
void validateRayTracingInitialExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts);
void validateRayTracingRebindExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts);
void validateRayTracingFinalCommandExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts);

struct RayTracingPipelineCreateObservation {
    std::uint32_t max_pipeline_ray_recursion_depth = 0;
    std::string validator_digest;
};
using RayTracingPipelineCreateObserver =
    std::function<void(const RayTracingPipelineCreateObservation &)>;

class ScopedRayTracingPipelineCreateObserver {
    RayTracingPipelineCreateObserver previous_;

  public:
    explicit ScopedRayTracingPipelineCreateObserver(
        RayTracingPipelineCreateObserver observer);
    ~ScopedRayTracingPipelineCreateObserver();
    ScopedRayTracingPipelineCreateObserver(
        const ScopedRayTracingPipelineCreateObserver &) = delete;
    ScopedRayTracingPipelineCreateObserver &operator=(
        const ScopedRayTracingPipelineCreateObserver &) = delete;
};

void observeRayTracingPipelineCreate(
    std::uint32_t resolved_depth,
    std::string_view validator_digest);

} // namespace Pelican
