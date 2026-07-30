#pragma once

#include "rendercompilerprogram.hpp"
#include "renderingsamplecount.hpp"
#include "rendertargetdefinition.hpp"
#include "../../project/vulkancompletephysicalplan.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::string_view
    vulkanRenderCompilerBackend = "vulkan";

class VulkanRenderCompilerBackendContext final
    : public RenderCompilerBackendContext {
  public:
    VulkanRenderCompilerBackendContext(
        vk::Format output_format =
            vk::Format::eUndefined,
        vk::Extent2D output_extent = {},
        vk::PhysicalDevice physical_device = {})
        : output_format{output_format},
          output_extent{output_extent},
          physical_device{physical_device} {}

    vk::Format output_format =
        vk::Format::eUndefined;
    vk::Extent2D output_extent{};
    vk::PhysicalDevice physical_device{};

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

const RenderCompilerProgram &
defaultVulkanRenderCompilerProgram();

} // namespace Pelican
