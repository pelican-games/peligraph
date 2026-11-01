#pragma once

#include "rendercompilerprogram.hpp"
#include "renderingsamplecount.hpp"
#include "rendertargetdefinition.hpp"
#include "../../project/vulkancompletephysicalplan.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::string_view
    vulkanRenderCompilerBackend = "vulkan";

enum class VulkanRenderCompilerDevicePlanningMode
    : std::uint8_t {
    device_required,
    compiler_only,
};

class VulkanRenderCompilerBackendContext final
    : public RenderCompilerBackendContext {
  public:
    VulkanRenderCompilerBackendContext(
        vk::Format output_format =
            vk::Format::eUndefined,
        vk::Extent2D output_extent = {},
        vk::PhysicalDevice physical_device = {},
        std::vector<std::string>
            enabled_device_extensions = {},
        VulkanRuntimeCapabilities runtime_capabilities = {},
        VulkanRenderCompilerDevicePlanningMode
            device_planning_mode =
                VulkanRenderCompilerDevicePlanningMode::
                    device_required,
        vk::Device device = {}, std::uint32_t graphics_queue_family = 0)
        : output_format{output_format},
          output_extent{output_extent},
          physical_device{physical_device},
          enabled_device_extensions{
              std::move(enabled_device_extensions)},
          runtime_capabilities{runtime_capabilities},
          device_planning_mode{device_planning_mode}, device{device}, graphics_queue_family{graphics_queue_family} {}

    vk::Format output_format =
        vk::Format::eUndefined;
    vk::Extent2D output_extent{};
    vk::PhysicalDevice physical_device{};
    std::vector<std::string>
        enabled_device_extensions;
    VulkanRuntimeCapabilities runtime_capabilities;
    VulkanRenderCompilerDevicePlanningMode
        device_planning_mode;
    vk::Device device;
    std::uint32_t graphics_queue_family = 0;
    // CPU tests may supply device facts without constructing a Vulkan device.
    std::optional<RenderingTargetPlanDeviceFacts> planning_facts;

    std::string_view backend() const noexcept override {
        return vulkanRenderCompilerBackend;
    }
    ResolvedResourceExtent outputExtent() const noexcept override {
        return {output_extent.width, output_extent.height};
    }
};

class VulkanRenderCompilerPhysicalPackage final
    : public RenderCompilerBackendPhysicalPackage {
  public:
    // RC-3c2a: what the plan placed of each config image of the variant's
    // IR, in its declaration order (the registration merges the variants'
    // into the generation's resource description).
    std::vector<ImagePlacement> image_placements;
    RenderingTargetPlanCompilation
        target_plan_compilation;
    std::unordered_map<
        std::string,
        std::shared_ptr<const VulkanTargetPlan>>
        target_plans;
    // Optional same-layer complete artifacts. Presence means the matching
    // target plan was produced with
    // applyVerifiedVulkanCompletePhysicalPlanPackage and may contain
    // executable NativeScope declarations.
    std::unordered_map<
        std::string,
        std::shared_ptr<
            const VerifiedVulkanCompletePhysicalPlanPackage>>
        verified_complete_physical_plans;

    std::string_view backend() const noexcept override {
        return vulkanRenderCompilerBackend;
    }
    void validate(
        std::span<const std::string>
            frame_graph_names) const override;
    PhysicalNodeOrder physicalNodeOrder(std::string_view graph) const override;
    std::span<const ImagePlacement> imagePlacements() const noexcept override {
        return image_placements;
    }
};

const VulkanRenderCompilerBackendContext &
requireVulkanRenderCompilerBackendContext(
    const RenderCompilerBackendContext &context);

VulkanRenderCompilerPhysicalPackage &
requireVulkanRenderCompilerPhysicalPackage(
    RenderCompilerBackendPhysicalPackage &package);
const VulkanRenderCompilerPhysicalPackage &
requireVulkanRenderCompilerPhysicalPackage(
    const RenderCompilerBackendPhysicalPackage &package);

// RC-7e: every final target plan of `variants` (the runtime variants a
// generation registers), in their order.
std::vector<std::shared_ptr<const VulkanTargetPlan>>
vulkanRuntimeVariantTargetPlans(
    std::span<const RenderCompilerProgramVariantOutput> variants);

// RC-7e: the one place the engine's render target uses outside every plan
// of `variants` (the runtime variants) come from: for each runtime variant's
// pipeline, what the view-family providers declare
// (viewFamilyProviderRenderTargets). Registration counts them in U.
std::vector<std::string> vulkanRuntimeVariantEngineRenderTargetUses(
    std::span<const RenderCompilerProgramVariantOutput> variants);

const RenderingTargetPlanVerificationContext &
requireVulkanTargetPlanVerificationContext(
    const VulkanRenderCompilerPhysicalPackage &physical,
    std::string_view graph);

// Verifies a complete same-layer replacement against the exact logical graph
// and device topology used by the delegated automatic compiler, then updates
// every target-plan index as one package mutation.
void installVerifiedVulkanCompletePhysicalPlanPackage(
    VulkanRenderCompilerPhysicalPackage &physical,
    std::string_view graph,
    VulkanCompletePhysicalPlanPackage package);

const RenderCompilerProgram &
defaultVulkanRenderCompilerProgram();

} // namespace Pelican
