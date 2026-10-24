#include "targetrendermemory.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <utility>

namespace Pelican {

std::uint64_t imageResourceBytes(const ImageResourceByteShape &shape) {
    const std::uint64_t texel =
        imageFormatEstimatedTexelBytes(shape.format);
    if (texel == 0 || shape.extent.width == 0 ||
        shape.extent.height == 0) {
        return 0;
    }
    const std::uint64_t layers = shape.array_layers;
    std::uint64_t sampled = 0;
    std::uint64_t width = shape.extent.width;
    std::uint64_t height = shape.extent.height;
    for (std::uint32_t mip = 0;
         mip < std::max(shape.mip_levels, 1u); ++mip) {
        sampled += texel * width * height * layers;
        width = std::max<std::uint64_t>(1, width / 2);
        height = std::max<std::uint64_t>(1, height / 2);
    }
    const std::uint64_t multisampled =
        shape.samples > 1
            ? texel * shape.extent.width * shape.extent.height *
                  layers * shape.samples
            : 0;
    return (sampled + multisampled) * shape.surfaces;
}

bool vulkanPhysicalResourceIsPlannedImage(
    const VulkanPhysicalResourcePlan &resource) {
    switch (resource.representation) {
    case VulkanResourceRepresentation::materialized_image:
    case VulkanResourceRepresentation::transient_attachment:
    case VulkanResourceRepresentation::tile_local_attachment:
        return true;
    case VulkanResourceRepresentation::materialized_buffer:
    case VulkanResourceRepresentation::external:
        return false;
    }
    return false;
}

std::uint64_t vulkanPhysicalResourceBytes(
    const VulkanPhysicalResourcePlan &resource,
    const VulkanPhysicalImageAllocation &allocation) {
    if (!vulkanPhysicalResourceIsPlannedImage(resource) ||
        !resource.format || allocation.extent.width == 0 ||
        allocation.extent.height == 0) {
        return 0;
    }
    return imageResourceBytes(ImageResourceByteShape{
        .format = *resource.format,
        .extent = allocation.extent,
        .mip_levels =
            resource.mip_levels.mode == ImageMipLevelMode::full_chain
                ? maximumImageMipLevels(allocation.extent.width,
                                        allocation.extent.height)
                : resource.mip_levels.count,
        .array_layers = resource.array_layers,
        .samples = resource.rasterization_samples,
        .surfaces = allocation.surfaces,
    });
}

VulkanTargetPlanMemorySummary summarizeVulkanTargetPlanMemory(
    const VulkanTargetPlan &plan,
    const VulkanPhysicalResourceByteCounter &bytes_of) {
    if (!bytes_of) {
        throw std::invalid_argument(
            "memory summary requires a byte counter");
    }
    VulkanTargetPlanMemorySummary summary;
    summary.resource_bytes.resize(plan.resources.size());
    for (std::size_t index = 0; index < plan.resources.size(); ++index) {
        const auto &resource = plan.resources[index];
        if (vulkanPhysicalResourceIsPlannedImage(resource)) {
            summary.resource_bytes[index] = bytes_of(resource);
        }
    }

    std::size_t orders = 0;
    for (std::size_t index = 0; index < plan.resources.size(); ++index) {
        const auto &lifetime = plan.resources[index].lifetime;
        if (summary.resource_bytes[index] && lifetime.used) {
            orders = std::max(orders, lifetime.last_use + 1);
        }
    }
    for (std::size_t order = 0; order < orders; ++order) {
        std::uint64_t live = 0;
        for (std::size_t index = 0; index < plan.resources.size();
             ++index) {
            const auto &lifetime = plan.resources[index].lifetime;
            const auto &bytes = summary.resource_bytes[index];
            if (bytes && lifetime.used && lifetime.first_use <= order &&
                order <= lifetime.last_use) {
                live += *bytes;
            }
        }
        if (live > summary.peak_live_bytes) {
            summary.peak_live_bytes = live;
            summary.peak_order = order;
        }
    }
    if (summary.peak_order &&
        *summary.peak_order < plan.lowering_graph.nodes.size()) {
        summary.peak_node =
            plan.lowering_graph.nodes[*summary.peak_order].logical.name;
    }
    return summary;
}

RegisteredTargetMemoryTotal totalRegisteredTargetMemory(
    std::span<const RegisteredTargetMemory> targets) {
    RegisteredTargetMemoryTotal total;
    // An alias group is one allocation of single-sample images its members
    // take turns in; the multisampled images stay each member's own.
    struct SharedAllocation {
        std::uint64_t bytes = 0;
        bool used = false;
    };
    std::map<std::uint64_t, SharedAllocation> shared;
    for (const auto &target : targets) {
        std::uint64_t own = target.bytes;
        if (target.alias_group) {
            if (target.alias_shared_bytes > target.bytes) {
                throw std::invalid_argument(
                    "registered target shares more bytes than it holds: " +
                    target.name);
            }
            auto &allocation = shared[*target.alias_group];
            allocation.bytes =
                std::max(allocation.bytes, target.alias_shared_bytes);
            allocation.used = allocation.used || target.used;
            own -= target.alias_shared_bytes;
        }
        total.total_bytes += own;
        if (!target.used) {
            total.unused_bytes += own;
            total.unused_resources.push_back(target.name);
        }
    }
    for (const auto &[group, allocation] : shared) {
        (void)group;
        total.total_bytes += allocation.bytes;
        if (!allocation.used) total.unused_bytes += allocation.bytes;
    }
    std::sort(total.unused_resources.begin(),
              total.unused_resources.end());
    return total;
}

void appendVulkanTargetPlanMemoryJson(
    nlohmann::json &frame_plan, const VulkanTargetPlan &plan,
    const VulkanTargetPlanMemorySummary &summary,
    const RegisteredTargetMemoryTotal &allocation) {
    if (summary.resource_bytes.size() != plan.resources.size()) {
        throw std::invalid_argument(
            "memory summary does not describe the plan's resources");
    }
    frame_plan["memory_summary"] = {
        {"total_bytes", allocation.total_bytes},
        {"unused_bytes", allocation.unused_bytes},
        {"unused_resources", allocation.unused_resources},
        {"peak_live_bytes", summary.peak_live_bytes},
        {"peak_node", summary.peak_node.empty()
                          ? nlohmann::json(nullptr)
                          : nlohmann::json(summary.peak_node)},
        {"peak_graph", plan.graph},
    };
    const auto physical = frame_plan.find("physical_target_plan");
    if (physical == frame_plan.end() || !physical->is_object()) return;
    const auto resources = physical->find("resources");
    if (resources == physical->end() || !resources->is_array()) return;
    std::map<std::string_view, std::uint64_t, std::less<>> bytes_by_name;
    for (std::size_t index = 0; index < plan.resources.size(); ++index) {
        if (summary.resource_bytes[index]) {
            bytes_by_name.emplace(plan.resources[index].logical_resource,
                                  *summary.resource_bytes[index]);
        }
    }
    for (auto &resource : *resources) {
        if (!resource.is_object()) continue;
        const auto name = resource.find("logical_resource");
        if (name == resource.end() || !name->is_string()) continue;
        const auto found = bytes_by_name.find(
            name->get_ref<const std::string &>());
        if (found != bytes_by_name.end()) {
            resource["bytes"] = found->second;
        }
    }
}

} // namespace Pelican
