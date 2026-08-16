#pragma once

#include "rendercompilerprogram.hpp"
#include "renderingsamplecount.hpp"
#include "rendertargetdefinition.hpp"
#include "../../project/vulkancompletephysicalplan.hpp"

#include <cstdint>
#include <memory>
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
                    device_required)
        : output_format{output_format},
          output_extent{output_extent},
          physical_device{physical_device},
          enabled_device_extensions{
              std::move(enabled_device_extensions)},
          runtime_capabilities{runtime_capabilities},
          device_planning_mode{device_planning_mode} {}

    vk::Format output_format =
        vk::Format::eUndefined;
    vk::Extent2D output_extent{};
    vk::PhysicalDevice physical_device{};
    std::vector<std::string>
        enabled_device_extensions;
    VulkanRuntimeCapabilities runtime_capabilities;
    VulkanRenderCompilerDevicePlanningMode
        device_planning_mode =
            VulkanRenderCompilerDevicePlanningMode::
                device_required;

    std::string_view backend() const noexcept override {
        return vulkanRenderCompilerBackend;
    }
};

class VulkanRenderCompilerPhysicalPackage final
    : public RenderCompilerBackendPhysicalPackage {
  public:
    std::vector<RenderTargetDefinition>
        render_target_definitions;
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
