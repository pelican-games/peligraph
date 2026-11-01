#include "targetrendermemory.hpp"
#include "imagesubresource.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <set>
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
        if(!target.allocations.empty()) {
            for(const auto &[identity,size]:target.allocations) {
                auto &allocation=shared[identity];
                allocation.bytes=std::max(allocation.bytes,size);
                allocation.used=allocation.used || target.used;
            }
            if(!target.used) total.unused_resources.push_back(target.name);
            continue;
        }
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

std::vector<std::string> engineHeldRenderTargetNames() {
    return {"display"};
}

std::vector<UnallocatedRenderTarget> unallocatedRenderTargets(
    const GenerationResources &resources,
    std::span<const std::shared_ptr<const VulkanTargetPlan>> plans,
    std::span<const std::string> held,
    std::span<const std::string> engine_uses) {
    std::set<std::string, std::less<>> used{held.begin(), held.end()};
    used.insert(engine_uses.begin(), engine_uses.end());
    for (const auto &plan : plans) {
        if (plan == nullptr) continue;
        for (const auto &resource : plan->resources)
            if (vulkanPhysicalResourceIsPlannedImage(resource)) used.insert(resource.logical_resource);
    }
    std::vector<UnallocatedRenderTarget> result;
    for (const auto &image : resources.images) {
        const auto &declaration = image.declaration;
        if (used.contains(declaration.name)) continue;
        result.push_back(UnallocatedRenderTarget{
            .name = declaration.name,
            .reason = "unused_in_generation",
            .payload_bytes = imageResourceBytes(ImageResourceByteShape{
                .format = image.format,
                .extent = image.extent,
                .mip_levels = resolveImageMipLevels(declaration.mip_levels, image.extent.width,
                                                    image.extent.height),
                .array_layers = image.array_layers,
                .samples = image.samples,
                .surfaces = declaration.history ? 2u : 1u,
            }),
        });
    }
    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) { return left.name < right.name; });
    return result;
}

void appendVulkanTargetPlanMemoryJson(
    nlohmann::json &frame_plan, const VulkanTargetPlan &plan,
    const VulkanTargetPlanMemorySummary &summary,
    const RegisteredTargetMemoryTotal &allocation,
    std::span<const UnallocatedRenderTarget> unallocated) {
    // payload_bytes is the estimate made at registration, at the extent the
    // generation registered at; it is not recomputed at the current extent.
    auto unallocated_json = nlohmann::json::array();
    for (const auto &target : unallocated)
        unallocated_json.push_back({{"name", target.name}, {"reason", target.reason},
                                    {"payload_bytes", target.payload_bytes}});
    if (summary.resource_bytes.size() != plan.resources.size()) {
        throw std::invalid_argument(
            "memory summary does not describe the plan's resources");
    }
    frame_plan["memory_summary"] = {
        {"total_bytes", allocation.total_bytes},
        {"unused_bytes", allocation.unused_bytes},
        {"unused_resources", allocation.unused_resources},
        {"unallocated_resources", std::move(unallocated_json)},
        {"peak_live_bytes", summary.peak_live_bytes},
        {"peak_node", summary.peak_node.empty()
                          ? nlohmann::json(nullptr)
                          : nlohmann::json(summary.peak_node)},
        {"peak_graph", plan.graph},
        {"payload_bytes", [&] { std::uint64_t total=0; for (const auto &b : summary.resource_bytes) total+=b.value_or(0); return total; }()},
        {"planned_region_bytes", [&] { std::uint64_t total=0; for (const auto &r : plan.memory_regions) total+=r.size; return total; }()},
        {"planned_region_count", plan.memory_regions.size()},
        {"actual_placement", nullptr}, // Step 3 publishes measured bindings here.
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
            resource["payload_bytes"] = found->second;
        }
    }
}

} // namespace Pelican
