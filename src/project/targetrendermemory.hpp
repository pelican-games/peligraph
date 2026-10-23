#pragma once

#include "targetrenderplanning.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// RC-1: how many bytes a physical target plan's images occupy, and the most
// of them alive at one node. The planner owns the arithmetic; the runtime
// arena and get_frame_plan copy its answers.

// The one table of texel sizes. `format` is spelled as the physical plan
// spells it: Vulkan's enumerant without its leading `e` (vk::to_string's
// form, "R16G16B16A16Sfloat"). 0 for a format the table does not list;
// every format a render target can be authored with is listed.
std::uint32_t imageFormatTexelBytes(std::string_view format);

// One image resource as the runtime allocates it.
struct ImageResourceByteShape {
    std::string_view format;
    // Of mip 0.
    ResolvedResourceExtent extent;
    // The resolved count: a full chain is already counted for `extent`.
    std::uint32_t mip_levels = 1;
    std::uint32_t array_layers = 1;
    std::uint32_t samples = 1;
    // A history target keeps two: this frame's and the previous one.
    std::uint32_t surfaces = 1;
};

// texel bytes x width x height x layers, summed over the mips (each halves
// the extent, rounding down, never below 1), times the surfaces. A
// multisampled target is two images per surface, as RenderTargetContainer
// creates it: the single-sample image passes sample (with its mips) and the
// `samples`-times image it renders into (mip 0 only). 0 for an empty extent
// or a format imageFormatTexelBytes does not list.
std::uint64_t imageResourceBytes(const ImageResourceByteShape &shape);

// Whether the plan allocates the resource as an image of its own: a
// materialized, transient or tile-local image. An external image (the
// swapchain) is the presentation engine's, and a buffer has no format or
// extent to count.
bool vulkanPhysicalResourceIsPlannedImage(
    const VulkanPhysicalResourcePlan &resource);

// What the physical plan does not say about one of its images: its extent
// as allocated (mip 0) and how many surfaces it keeps.
struct VulkanPhysicalImageAllocation {
    ResolvedResourceExtent extent;
    std::uint32_t surfaces = 1;
};

// imageResourceBytes over the resource's format, mips, layers and samples
// at `allocation`. 0 for a resource vulkanPhysicalResourceIsPlannedImage
// refuses.
std::uint64_t vulkanPhysicalResourceBytes(
    const VulkanPhysicalResourcePlan &resource,
    const VulkanPhysicalImageAllocation &allocation);

struct VulkanTargetPlanMemorySummary {
    // Parallel to plan.resources: the bytes of each planned image; nullopt
    // for a resource the plan does not allocate as an image, or one whose
    // bytes the caller could not say.
    std::vector<std::optional<std::uint64_t>> resource_bytes;
    // Every planned image once. The members of an alias group share one
    // allocation, so a group counts once, at its largest member.
    std::uint64_t total_bytes = 0;
    // The most bytes alive at one node: the images whose lifetime
    // [first_use, last_use] holds that node's order, each at its own bytes.
    std::uint64_t peak_live_bytes = 0;
    // The first order holding that peak, and its node (the lowering graph's
    // node at the order). Unset while no image has bytes.
    std::optional<std::size_t> peak_order;
    std::string peak_node;
};

// The bytes of one planned image, as the caller knows them: from the plan
// and an extent (vulkanPhysicalResourceBytes), or from the image the
// runtime made. nullopt when the caller cannot say.
using VulkanPhysicalResourceByteCounter =
    std::function<std::optional<std::uint64_t>(
        const VulkanPhysicalResourcePlan &)>;

// Asks `bytes_of` only for the resources vulkanPhysicalResourceIsPlannedImage
// accepts.
VulkanTargetPlanMemorySummary summarizeVulkanTargetPlanMemory(
    const VulkanTargetPlan &plan,
    const VulkanPhysicalResourceByteCounter &bytes_of);

// get_frame_plan's part: `memory_summary` {total_bytes, peak_live_bytes,
// peak_node} on `frame_plan` (peak_node null without a peak), and `bytes`
// on each physical_target_plan.resources[] entry the summary has bytes for.
// An external image or a buffer gets no `bytes`.
void appendVulkanTargetPlanMemoryJson(
    nlohmann::json &frame_plan, const VulkanTargetPlan &plan,
    const VulkanTargetPlanMemorySummary &summary);

} // namespace Pelican
