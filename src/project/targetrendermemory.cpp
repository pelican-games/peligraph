#include "targetrendermemory.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

// Every format a render target can be authored with (the engine's format
// names), spelled as vk::to_string spells its enumerant. D32_SFLOAT_S8_UINT
// counts the 8 bytes an implementation stores, as the engine's readback
// always counted it.
constexpr std::array<std::pair<std::string_view, std::uint32_t>, 46>
    kTexelBytes{{
        {"R8Unorm", 1},
        {"R8Snorm", 1},
        {"R8Uint", 1},
        {"R8Sint", 1},
        {"R8G8Unorm", 2},
        {"R8G8Snorm", 2},
        {"R8G8Uint", 2},
        {"R8G8Sint", 2},
        {"R16Sfloat", 2},
        {"R16Unorm", 2},
        {"R16Snorm", 2},
        {"R16Uint", 2},
        {"R16Sint", 2},
        {"D16Unorm", 2},
        {"R8G8B8A8Unorm", 4},
        {"R8G8B8A8Snorm", 4},
        {"R8G8B8A8Uint", 4},
        {"R8G8B8A8Sint", 4},
        {"R8G8B8A8Srgb", 4},
        {"B8G8R8A8Unorm", 4},
        {"B8G8R8A8Srgb", 4},
        {"A2B10G10R10UnormPack32", 4},
        {"A2R10G10B10UnormPack32", 4},
        {"B10G11R11UfloatPack32", 4},
        {"R16G16Unorm", 4},
        {"R16G16Snorm", 4},
        {"R16G16Uint", 4},
        {"R16G16Sint", 4},
        {"R16G16Sfloat", 4},
        {"R32Uint", 4},
        {"R32Sint", 4},
        {"R32Sfloat", 4},
        {"D24UnormS8Uint", 4},
        {"D32Sfloat", 4},
        {"R16G16B16A16Unorm", 8},
        {"R16G16B16A16Snorm", 8},
        {"R16G16B16A16Uint", 8},
        {"R16G16B16A16Sint", 8},
        {"R16G16B16A16Sfloat", 8},
        {"R32G32Uint", 8},
        {"R32G32Sint", 8},
        {"R32G32Sfloat", 8},
        {"D32SfloatS8Uint", 8},
        {"R32G32B32A32Uint", 16},
        {"R32G32B32A32Sint", 16},
        {"R32G32B32A32Sfloat", 16},
    }};

} // namespace

std::uint32_t imageFormatTexelBytes(std::string_view format) {
    for (const auto &[name, bytes] : kTexelBytes) {
        if (name == format) return bytes;
    }
    return 0;
}

std::uint64_t imageResourceBytes(const ImageResourceByteShape &shape) {
    const std::uint64_t texel = imageFormatTexelBytes(shape.format);
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
        allocation.extent.width == 0 ||
        allocation.extent.height == 0) {
        return 0;
    }
    return imageResourceBytes(ImageResourceByteShape{
        .format = resource.format,
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
    VulkanTargetPlanMemorySummary summary;
    summary.resource_bytes.resize(plan.resources.size());
    for (std::size_t index = 0; index < plan.resources.size(); ++index) {
        const auto &resource = plan.resources[index];
        if (bytes_of && vulkanPhysicalResourceIsPlannedImage(resource)) {
            summary.resource_bytes[index] = bytes_of(resource);
        }
    }

    // An alias group is one allocation its members take turns in.
    std::map<std::string_view, std::size_t, std::less<>> group_of;
    for (std::size_t group = 0; group < plan.alias_groups.size();
         ++group) {
        for (const auto &member : plan.alias_groups[group].resources) {
            group_of.emplace(member, group);
        }
    }
    std::vector<std::uint64_t> group_bytes(plan.alias_groups.size(), 0);
    for (std::size_t index = 0; index < plan.resources.size(); ++index) {
        const auto &bytes = summary.resource_bytes[index];
        if (!bytes) continue;
        const auto group =
            group_of.find(plan.resources[index].logical_resource);
        if (group == group_of.end()) {
            summary.total_bytes += *bytes;
        } else {
            group_bytes[group->second] =
                std::max(group_bytes[group->second], *bytes);
        }
    }
    for (const auto bytes : group_bytes) summary.total_bytes += bytes;

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

void appendVulkanTargetPlanMemoryJson(
    nlohmann::json &frame_plan, const VulkanTargetPlan &plan,
    const VulkanTargetPlanMemorySummary &summary) {
    if (summary.resource_bytes.size() != plan.resources.size()) {
        throw std::invalid_argument(
            "memory summary does not describe the plan's resources");
    }
    frame_plan["memory_summary"] = {
        {"total_bytes", summary.total_bytes},
        {"peak_live_bytes", summary.peak_live_bytes},
        {"peak_node", summary.peak_node.empty()
                          ? nlohmann::json(nullptr)
                          : nlohmann::json(summary.peak_node)},
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
