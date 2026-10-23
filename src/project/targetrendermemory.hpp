#pragma once

#include "targetrenderplanning.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// RC-1: how many bytes a physical target plan's images occupy, and the most
// of them alive at one node. The planner owns the arithmetic; the runtime
// arena and get_frame_plan copy its answers.
// RC-1b: what the images take together is the runtime's to say, not the
// plan's: the runtime registers every render target definition (a target no
// pass uses too) and keeps an alias group only when every graph agrees on it.
// totalRegisteredTargetMemory counts what registration made.

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

// One graph's plan: each planned image's bytes and the most of them alive at
// one node. (What they take together: totalRegisteredTargetMemory.)
struct VulkanTargetPlanMemorySummary {
    // Parallel to plan.resources: the bytes of each planned image; nullopt
    // for a resource the plan does not allocate as an image (an external
    // image or a buffer).
    std::vector<std::optional<std::uint64_t>> resource_bytes;
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
// runtime made. A caller that cannot say throws, naming the resource: a
// summary never covers part of a plan.
using VulkanPhysicalResourceByteCounter =
    std::function<std::uint64_t(const VulkanPhysicalResourcePlan &)>;

// Asks `bytes_of` only for the resources vulkanPhysicalResourceIsPlannedImage
// accepts, and passes on what it throws.
VulkanTargetPlanMemorySummary summarizeVulkanTargetPlanMemory(
    const VulkanTargetPlan &plan,
    const VulkanPhysicalResourceByteCounter &bytes_of);

// RC-1b: one render target a runtime generation registered, as it is now.
struct RegisteredTargetMemory {
    std::string name;
    // What its images occupy: every surface, mip and layer of the image
    // passes sample, and the multisampled image it renders into.
    std::uint64_t bytes = 0;
    // Of `bytes`, what the members of its alias group share: the
    // single-sample images. Each member renders into a multisampled image
    // of its own. Read only with alias_group.
    std::uint64_t alias_shared_bytes = 0;
    // The allocation it shares with the rest of its alias group (the
    // registry's alias token); nullopt for allocations of its own.
    std::optional<std::uint64_t> alias_group;
    // Whether a graph's physical plan uses it (or the engine, outside any
    // graph). A target no graph uses is still allocated.
    bool used = true;
};

struct RegisteredTargetMemoryTotal {
    // Every registered target's images, an alias group's shared images once
    // (at its largest member's).
    std::uint64_t total_bytes = 0;
    // Of total_bytes, what no graph uses: an unused target's images of its
    // own, and an alias group's shared images when none of its members is
    // used.
    std::uint64_t unused_bytes = 0;
    // The unused targets' names, sorted.
    std::vector<std::string> unused_resources;
};

// std::invalid_argument for alias_shared_bytes past bytes.
RegisteredTargetMemoryTotal totalRegisteredTargetMemory(
    std::span<const RegisteredTargetMemory> targets);

// get_frame_plan's part: on `frame_plan`, `memory_summary`
//   {total_bytes, unused_bytes, unused_resources: what `allocation` counted
//    of the generation's registered targets;
//    peak_live_bytes, peak_node (null without a peak), peak_graph
//    (plan.graph): `summary` of this graph's plan}
// and `bytes` on each physical_target_plan.resources[] entry the summary has
// bytes for. An external image or a buffer gets no `bytes`.
void appendVulkanTargetPlanMemoryJson(
    nlohmann::json &frame_plan, const VulkanTargetPlan &plan,
    const VulkanTargetPlanMemorySummary &summary,
    const RegisteredTargetMemoryTotal &allocation);

} // namespace Pelican
