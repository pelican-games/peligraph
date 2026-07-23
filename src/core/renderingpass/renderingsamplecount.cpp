#include "renderingsamplecount.hpp"

#include "logicalframegraphadapter.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <vulkan/vulkan.hpp>
#if __has_include(<vulkan/utility/vk_format_utils.h>)
#include <vulkan/utility/vk_format_utils.h>
#define PELICAN_HAS_VULKAN_FORMAT_UTILS 1
#endif

namespace Pelican {
namespace {

constexpr std::string_view kRuntimeProvider =
    "pelican.vulkan.runtime_target_lowering@1";

bool isAttachmentTarget(const RenderTargetDefinition &definition) {
    return bool(definition.usage &
                (vk::ImageUsageFlagBits::eColorAttachment |
                 vk::ImageUsageFlagBits::eDepthStencilAttachment));
}

bool isDepthTarget(const RenderTargetDefinition &definition) {
    return bool(definition.usage &
                vk::ImageUsageFlagBits::eDepthStencilAttachment);
}

std::vector<std::uint32_t> sampleCounts(
    vk::SampleCountFlags flags) {
    constexpr std::array candidates{
        vk::SampleCountFlagBits::e1, vk::SampleCountFlagBits::e2,
        vk::SampleCountFlagBits::e4, vk::SampleCountFlagBits::e8,
        vk::SampleCountFlagBits::e16, vk::SampleCountFlagBits::e32,
        vk::SampleCountFlagBits::e64,
    };
    std::vector<std::uint32_t> result;
    for (const auto candidate : candidates) {
        if (flags & candidate) {
            result.push_back(
                static_cast<std::uint32_t>(candidate));
        }
    }
    return result;
}

bool supportsDepthResolve(vk::PhysicalDevice physical_device) {
    vk::PhysicalDeviceDepthStencilResolveProperties resolve;
    vk::PhysicalDeviceProperties2 properties;
    properties.pNext = &resolve;
    physical_device.getProperties2(&properties);
    return bool(resolve.supportedDepthResolveModes);
}

std::vector<std::uint32_t> queryAttachmentSampleCounts(
    vk::PhysicalDevice physical_device,
    const RenderTargetDefinition &definition) {
    vk::ImageUsageFlags attachment_usage;
    if (definition.usage &
        vk::ImageUsageFlagBits::eColorAttachment) {
        attachment_usage |=
            vk::ImageUsageFlagBits::eColorAttachment;
    }
    const auto depth = isDepthTarget(definition);
    if (depth) {
        attachment_usage |=
            vk::ImageUsageFlagBits::eDepthStencilAttachment;
    }
    try {
        auto counts = sampleCounts(
            physical_device
                .getImageFormatProperties(
                    definition.format, vk::ImageType::e2D,
                    vk::ImageTiling::eOptimal, attachment_usage)
                .sampleCounts);
        if (depth && !supportsDepthResolve(physical_device)) {
            counts.erase(
                std::remove_if(
                    counts.begin(), counts.end(),
                    [](const auto samples) {
                        return samples != 1;
                    }),
                counts.end());
        }
        return counts;
    } catch (const vk::SystemError &) {
        return {1};
    }
}

const CompilerProviderRegistrySnapshot &runtimeProviders() {
    static CompilerProviderRegistry registry;
    static const auto snapshot = [] {
        registry.registerProvider(CompilerProviderDescriptor{
            .id = std::string{kRuntimeProvider},
            .kind = CompilerProviderKind::target_lowering,
            .content_hash =
                "builtin:runtime-materialized-target-lowering-v1",
        });
        return registry.snapshot();
    }();
    return snapshot;
}

TargetTopologySnapshot runtimeTopology(
    const RenderingTargetPlanDeviceFacts &facts) {
    return TargetTopologySnapshot{
        .name = "runtime_vulkan_device",
        .endpoints =
            {TargetEndpoint{
                .id = "device:0",
                .kind = TargetEndpointKind::vulkan_device,
                .capabilities =
                    {"pelican.vulkan.graphics@1",
                     "pelican.vulkan.sampled_image@1",
                     "pelican.vulkan.storage_buffer@1",
                     "pelican.vulkan.transfer_copy@1"},
                .facts =
                    {{"pelican.vulkan.max_color_attachments@1",
                      std::to_string(
                          facts.max_color_attachments)},
                     {"pelican.vulkan.profile@1",
                      "materialized_runtime"}},
            }},
    };
}

std::set<std::string, std::less<>> knownResources(
    const FrameGraphDefinition &definition) {
    std::set<std::string, std::less<>> result(
        definition.declared_resources.begin(),
        definition.declared_resources.end());
    result.insert(definition.history_resources.begin(),
                  definition.history_resources.end());
    for (const auto &node : definition.nodes) {
        result.insert(node.reads.begin(), node.reads.end());
        result.insert(node.reads_history.begin(),
                      node.reads_history.end());
        result.insert(node.writes.begin(), node.writes.end());
    }
    result.erase(std::string{});
    return result;
}

std::map<std::string, const RenderTargetDefinition *, std::less<>>
renderTargetsByName(
    std::span<const RenderTargetDefinition> render_targets) {
    std::map<std::string, const RenderTargetDefinition *,
             std::less<>>
        result;
    for (const auto &target : render_targets) {
        if (target.name.empty() ||
            !result.emplace(target.name, &target).second) {
            throw std::runtime_error(
                "runtime target planning requires unique non-empty "
                "render-target names: " +
                target.name);
        }
    }
    return result;
}

LogicalFrameGraphShadowOptions shadowOptions(
    const FrameGraphDefinition &definition,
    const LogicalTypeRegistry &types,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets) {
    LogicalFrameGraphShadowOptions result;
    for (const auto &resource : knownResources(definition)) {
        if (resource == "swapchain") {
            result.resource_types.push_back({
                .resource = resource,
                .type = displayEncodedV1(types),
                .materialization =
                    LogicalMaterializationRequirement::external,
            });
            continue;
        }
        const auto target = render_targets.find(resource);
        if (target == render_targets.end()) continue;
        result.resource_types.push_back({
            .resource = resource,
            .type = isDepthTarget(*target->second)
                        ? deviceDepthV1(types)
                        : legacyOpaqueImageV1(types),
            .materialization =
                LogicalMaterializationRequirement::required,
        });
    }
    return result;
}

ResourcePattern runtimeImagePattern(
    const LogicalTypeRegistry &types,
    const LogicalType &type, vk::Format format,
    bool external) {
    return ResourcePattern{
        .id =
            external
                ? "pelican.render.runtime_external_image@1"
                : "pelican.render.runtime_materialized_image@1",
        .applicable_type =
            exactLogicalTypePattern(types, type),
        .format_candidates =
            {ResourceFormatCandidate{
                .format = vk::to_string(format),
            }},
        .prefer_transient = false,
        .allow_tile_local = false,
        .allow_alias = false,
        .require_store = true,
        .local_read_fallback =
            ResourcePatternFallback::materialize,
        .provenance =
            "builtin:current-materialized-render-target-runtime-v1",
    };
}

std::vector<ResourcePatternBinding> runtimePatternBindings(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &logical_graph,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets,
    vk::Format swapchain_format) {
    std::vector<ResourcePatternBinding> result;
    for (const auto &resource : logical_graph.resources) {
        if (resource.type.constructor !=
            LogicalTypeConstructor::image) {
            continue;
        }
        if (resource.name == "swapchain") {
            result.push_back({
                resource.name,
                runtimeImagePattern(
                    types, resource.type, swapchain_format, true),
            });
            continue;
        }
        const auto target = render_targets.find(resource.name);
        if (target == render_targets.end()) {
            throw std::runtime_error(
                "runtime logical image has no RenderTargetDefinition: " +
                resource.name);
        }
        result.push_back({
            resource.name,
            runtimeImagePattern(
                types, resource.type, target->second->format, false),
        });
    }
    return result;
}

std::set<std::string, std::less<>> attachmentResources(
    const FrameGraphDefinition &definition,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets) {
    std::set<std::string, std::less<>> result;
    for (const auto &node : definition.nodes) {
        if (node.kind != FramePlanNodeKind::render) continue;
        for (const auto &resource : node.writes) {
            if (resource == "swapchain") {
                result.insert(resource);
                continue;
            }
            const auto target = render_targets.find(resource);
            if (target == render_targets.end() ||
                !isAttachmentTarget(*target->second)) {
                throw std::runtime_error(
                    "render node '" + node.name +
                    "' writes a resource that is not an attachment "
                    "RenderTargetDefinition: " +
                    resource);
            }
            result.insert(resource);
        }
    }
    return result;
}

std::vector<std::string> geometryNodes(
    const FrameGraphDefinition &definition) {
    std::vector<std::string> result;
    for (const auto &node : definition.nodes) {
        if (node.kind == FramePlanNodeKind::render &&
            node.raster_geometry) {
            result.push_back(node.name);
        }
    }
    return result;
}

std::vector<SampleCountResourceCapability> sampleCapabilities(
    const std::set<std::string, std::less<>> &attachments,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets,
    vk::Format swapchain_format,
    const RenderingTargetPlanDeviceFacts &facts) {
    std::vector<SampleCountResourceCapability> result;
    for (const auto &resource : attachments) {
        if (resource == "swapchain") {
            result.push_back({
                .resource = resource,
                .format = vk::to_string(swapchain_format),
                .supported_samples = {1},
            });
            continue;
        }
        const auto &target = *render_targets.at(resource);
        result.push_back({
            .resource = resource,
            .format = vk::to_string(target.format),
            .supported_samples =
                facts.query_attachment_samples
                    ? facts.query_attachment_samples(target)
                    : std::vector<std::uint32_t>{1},
        });
    }
    return result;
}

const VulkanPhysicalResourcePlan &requirePhysicalResource(
    const VulkanTargetPlan &plan, std::string_view resource) {
    const auto found = std::lower_bound(
        plan.resources.begin(), plan.resources.end(), resource,
        [](const VulkanPhysicalResourcePlan &candidate,
           std::string_view name) {
            return candidate.logical_resource < name;
        });
    if (found == plan.resources.end() ||
        found->logical_resource != resource) {
        throw std::runtime_error(
            "runtime target plan did not lower physical resource: " +
            std::string{resource});
    }
    return *found;
}

void validateRuntimePhysicalPlan(
    const VulkanTargetPlan &plan,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets,
    vk::Format swapchain_format) {
    if (!plan.alias_groups.empty()) {
        throw std::runtime_error(
            "current render-target runtime cannot consume physical alias "
            "groups");
    }
    for (const auto &resource : plan.resources) {
        if (resource.logical_resource == "swapchain") {
            if (resource.representation !=
                    VulkanResourceRepresentation::external ||
                resource.format != vk::to_string(swapchain_format) ||
                resource.rasterization_samples != 1) {
                throw std::runtime_error(
                    "runtime swapchain physical plan is incompatible "
                    "with the current external single-sample target");
            }
            continue;
        }
        const auto target =
            render_targets.find(resource.logical_resource);
        if (target == render_targets.end()) {
            throw std::runtime_error(
                "runtime physical image has no RenderTargetDefinition: " +
                resource.logical_resource);
        }
        if (resource.representation !=
            VulkanResourceRepresentation::materialized_image) {
            throw std::runtime_error(
                "current render-target runtime only consumes "
                "materialized_image plans: " +
                resource.logical_resource);
        }
        const auto format = vk::to_string(target->second->format);
        if (resource.format != format) {
            throw std::runtime_error(
                "runtime physical format mismatch for '" +
                resource.logical_resource + "': planned '" +
                resource.format + "', RenderTargetDefinition '" +
                format + "'");
        }
    }
}

} // namespace

RenderingTargetPlanCompilation compileRenderingTargetPlans(
    std::span<const FrameGraphDefinition> frame_graphs,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    vk::Format swapchain_format,
    const RenderingTargetPlanDeviceFacts &device_facts) {
    const auto target_by_name =
        renderTargetsByName(render_targets);
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto topology = runtimeTopology(device_facts);

    std::vector<std::set<std::string, std::less<>>>
        graph_attachments;
    graph_attachments.reserve(frame_graphs.size());
    std::set<std::string, std::less<>> all_attachments;
    for (const auto &definition : frame_graphs) {
        auto attachments =
            attachmentResources(definition, target_by_name);
        all_attachments.insert(attachments.begin(),
                               attachments.end());
        graph_attachments.push_back(std::move(attachments));
    }
    for (const auto &target : policy.targets) {
        if (!all_attachments.contains(target)) {
            throw std::runtime_error(
                "multisampling target is not an attachment logical "
                "resource in any frame graph: " +
                target);
        }
    }

    RenderingTargetPlanCompilation result;
    result.plans.reserve(frame_graphs.size());
    std::map<std::string, std::uint32_t, std::less<>>
        merged_samples;
    for (std::size_t index = 0; index < frame_graphs.size();
         ++index) {
        const auto &definition = frame_graphs[index];
        auto local_policy = policy;
        local_policy.targets.erase(
            std::remove_if(
                local_policy.targets.begin(),
                local_policy.targets.end(),
                [&](const std::string &target) {
                    return !graph_attachments[index].contains(target);
                }),
            local_policy.targets.end());

        auto logical_graph = compileLogicalFrameGraphShadow(
            definition, types,
            shadowOptions(definition, types, target_by_name));
        auto plan = std::make_shared<const VulkanTargetPlan>(
            compileVulkanTargetPlan(
                types, logical_graph, topology,
                runtimeProviders(),
                VulkanTargetPlanRequest{
                    .endpoint = "device:0",
                    .provider = std::string{kRuntimeProvider},
                    .pattern_bindings =
                        runtimePatternBindings(
                            types, logical_graph, target_by_name,
                            swapchain_format),
                    .sample_count =
                        VulkanSampleCountPlanRequest{
                            .policy = std::move(local_policy),
                            .capabilities =
                                sampleCapabilities(
                                    graph_attachments[index],
                                    target_by_name,
                                    swapchain_format,
                                    device_facts),
                            .geometry_nodes =
                                geometryNodes(definition),
                        },
                }));
        validateRuntimePhysicalPlan(
            *plan, target_by_name, swapchain_format);
        if (!plan->sample_count_plan) {
            throw std::runtime_error(
                "runtime Vulkan target plan lacks sample-count lowering");
        }
        for (const auto &resolved :
             plan->sample_count_plan->resources) {
            if (resolved.resource == "swapchain") continue;
            const auto &physical = requirePhysicalResource(
                *plan, resolved.resource);
            if (physical.rasterization_samples !=
                resolved.samples) {
                throw std::runtime_error(
                    "runtime physical sample-count contract mismatch: " +
                    resolved.resource);
            }
            const auto [found, inserted] =
                merged_samples.emplace(
                    resolved.resource,
                    physical.rasterization_samples);
            if (!inserted &&
                found->second !=
                    physical.rasterization_samples) {
                throw std::runtime_error(
                    "frame graphs require conflicting physical sample "
                    "counts for render target '" +
                    resolved.resource + "'");
            }
        }
        result.plans.push_back(std::move(plan));
    }

    result.assignments.reserve(render_targets.size());
    for (const auto &target : render_targets) {
        const auto found = merged_samples.find(target.name);
        result.assignments.push_back({
            .resource = target.name,
            .samples = found == merged_samples.end()
                           ? 1u
                           : found->second,
        });
    }
    std::sort(
        result.assignments.begin(), result.assignments.end(),
        [](const auto &left, const auto &right) {
            return left.resource < right.resource;
        });
    return result;
}

RenderingTargetPlanCompilation
compileRenderingTargetPlansForVulkanDevice(
    std::span<const FrameGraphDefinition> frame_graphs,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    vk::Format swapchain_format,
    vk::PhysicalDevice physical_device) {
    return compileRenderingTargetPlans(
        frame_graphs, render_targets, policy, swapchain_format,
        RenderingTargetPlanDeviceFacts{
            .max_color_attachments =
                physical_device.getProperties()
                    .limits.maxColorAttachments,
            .query_attachment_samples =
                [physical_device](
                    const RenderTargetDefinition &definition) {
                    return queryAttachmentSampleCounts(
                        physical_device, definition);
                },
        });
}

void applyRenderingTargetPlan(
    std::span<RenderTargetDefinition> render_targets,
    const RenderingTargetPlanCompilation &compilation) {
    std::map<std::string, std::uint32_t, std::less<>>
        assignments;
    for (const auto &assignment : compilation.assignments) {
        if (!assignments.emplace(assignment.resource,
                                 assignment.samples)
                 .second) {
            throw std::runtime_error(
                "duplicate physical sample-count assignment: " +
                assignment.resource);
        }
    }
    for (auto &target : render_targets) {
        const auto found = assignments.find(target.name);
        if (found == assignments.end()) {
            throw std::runtime_error(
                "missing physical sample-count assignment: " +
                target.name);
        }
        target.samples = found->second;
    }
}

vk::ResolveModeFlagBits colorAttachmentResolveMode(
    vk::Format format) {
#if defined(PELICAN_HAS_VULKAN_FORMAT_UTILS)
    const auto raw = static_cast<VkFormat>(format);
    const auto integer =
        vkuFormatIsSINT(raw) || vkuFormatIsUINT(raw);
#else
    // Vulkan's integer color format enumerants use the Uint/Sint suffix.
    // This fallback keeps minimal Vulkan-Headers distributions buildable.
    const auto name = vk::to_string(format);
    const auto integer =
        name.find("Uint") != std::string::npos ||
        name.find("Sint") != std::string::npos;
#endif
    return integer ? vk::ResolveModeFlagBits::eSampleZero
                   : vk::ResolveModeFlagBits::eAverage;
}

} // namespace Pelican
