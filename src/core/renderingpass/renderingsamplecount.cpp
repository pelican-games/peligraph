#include "renderingsamplecount.hpp"

#include "logicalframegraphadapter.hpp"

#include <algorithm>
#include <array>
#include <limits>
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
constexpr std::string_view kTransientAttachmentCapability =
    "pelican.vulkan.transient_attachment@1";
constexpr std::string_view kTileBasedCapability =
    "pelican.vulkan.tile_based@1";
constexpr std::string_view kLocalReadCapability =
    "pelican.vulkan.dynamic_rendering_local_read@1";

using TransientAttachmentFormatSet =
    std::set<
        std::pair<std::string, std::string>,
        std::less<>>;
using TileLocalAttachmentFormatSet =
    std::set<
        std::pair<std::string, std::string>,
        std::less<>>;

bool isAttachmentTarget(const RenderTargetDefinition &definition) {
    return bool(definition.usage &
                (vk::ImageUsageFlagBits::eColorAttachment |
                 vk::ImageUsageFlagBits::eDepthStencilAttachment));
}

bool isDepthTarget(const RenderTargetDefinition &definition) {
    return bool(definition.usage &
                vk::ImageUsageFlagBits::eDepthStencilAttachment);
}

bool isTransientAttachmentCandidate(
    const RenderTargetDefinition &definition) {
    const auto attachment_usage =
        vk::ImageUsageFlagBits::eColorAttachment |
        vk::ImageUsageFlagBits::
            eDepthStencilAttachment;
    return !definition.history &&
           bool(definition.usage &
                attachment_usage) &&
           !bool(
               definition.usage &
               ~vk::ImageUsageFlags{
                   attachment_usage});
}

bool isTileLocalAttachmentCandidate(
    const RenderTargetDefinition &definition) {
    const auto attachment_usage =
        vk::ImageUsageFlagBits::eColorAttachment |
        vk::ImageUsageFlagBits::
            eDepthStencilAttachment;
    const auto allowed_usage =
        attachment_usage |
        vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eInputAttachment;
    return !definition.history &&
           bool(definition.usage &
                attachment_usage) &&
           !bool(
               definition.usage &
               ~vk::ImageUsageFlags{
                   allowed_usage});
}

vk::ImageUsageFlags tileLocalImageUsage(
    const RenderTargetDefinition &definition) {
    const auto attachment_usage =
        definition.usage &
        (vk::ImageUsageFlagBits::eColorAttachment |
         vk::ImageUsageFlagBits::
             eDepthStencilAttachment);
    return attachment_usage |
           vk::ImageUsageFlagBits::eInputAttachment |
           vk::ImageUsageFlagBits::
               eTransientAttachment;
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

bool supportsDynamicRenderingLocalRead(
    vk::PhysicalDevice physical_device) {
    const auto extensions =
        physical_device
            .enumerateDeviceExtensionProperties();
    const auto extension = std::find_if(
        extensions.begin(), extensions.end(),
        [](const auto &candidate) {
            return std::string_view{
                       candidate
                           .extensionName.data()} ==
                   VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
        });
    if (extension == extensions.end()) {
        return false;
    }
    const auto features =
        physical_device.getFeatures2<
            vk::PhysicalDeviceFeatures2,
            vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR>();
    return features
               .get<
                   vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR>()
               .dynamicRenderingLocalRead ==
           VK_TRUE;
}

vk::ImageUsageFlags requiredImageUsage(
    const RenderTargetDefinition &definition) {
    auto usage = definition.usage;
    if (definition.history) {
        usage |= vk::ImageUsageFlagBits::eTransferDst;
    }
    return usage;
}

RenderingImageFormatCapability
queryImageFormatCapability(
    vk::PhysicalDevice physical_device,
    const RenderTargetDefinition &definition) {
    RenderingImageFormatCapability result;
    try {
        const auto properties =
            physical_device.getImageFormatProperties(
                definition.format,
                vk::ImageType::e2D,
                vk::ImageTiling::eOptimal,
                requiredImageUsage(definition));
        result.image_usage_supported = true;
        result.supported_samples =
            sampleCounts(properties.sampleCounts);
        result.max_array_layers =
            properties.maxArrayLayers;
        if (isDepthTarget(definition) &&
            !supportsDepthResolve(
                physical_device)) {
            result.supported_samples.erase(
                std::remove_if(
                    result.supported_samples.begin(),
                    result.supported_samples.end(),
                    [](const auto samples) {
                        return samples != 1;
                    }),
                result.supported_samples.end());
        }
    } catch (const vk::SystemError &) {
        return result;
    }

    try {
        (void)physical_device
            .getImageFormatProperties(
                definition.format,
                vk::ImageType::e2D,
                vk::ImageTiling::eOptimal,
                requiredImageUsage(definition) |
                    vk::ImageUsageFlagBits::
                        eTransferSrc);
        result.external_depth_export_supported =
            true;
    } catch (const vk::SystemError &) {
        result.external_depth_export_supported =
            false;
    }
    if (isTransientAttachmentCandidate(
            definition)) {
        try {
            (void)physical_device
                .getImageFormatProperties(
                    definition.format,
                    vk::ImageType::e2D,
                    vk::ImageTiling::eOptimal,
                    requiredImageUsage(definition) |
                        vk::ImageUsageFlagBits::
                            eTransientAttachment);
            result.transient_attachment_supported =
                true;
        } catch (const vk::SystemError &) {
            result.transient_attachment_supported =
                false;
        }
    }
    if (isTileLocalAttachmentCandidate(
            definition)) {
        try {
            (void)physical_device
                .getImageFormatProperties(
                    definition.format,
                    vk::ImageType::e2D,
                    vk::ImageTiling::eOptimal,
                    tileLocalImageUsage(
                        definition));
            (void)physical_device
                .getImageFormatProperties(
                    definition.format,
                    vk::ImageType::e2D,
                    vk::ImageTiling::eOptimal,
                    requiredImageUsage(
                        definition) |
                        vk::ImageUsageFlagBits::
                            eInputAttachment);
            result.local_read_attachment_supported =
                true;
        } catch (const vk::SystemError &) {
            result.local_read_attachment_supported =
                false;
        }
    }
    return result;
}

std::vector<std::uint32_t> queryAttachmentSampleCounts(
    vk::PhysicalDevice physical_device,
    const RenderTargetDefinition &definition) {
    const auto capability =
        queryImageFormatCapability(
            physical_device, definition);
    return capability.image_usage_supported
               ? capability.supported_samples
               : std::vector<std::uint32_t>{1};
}

const CompilerProviderRegistrySnapshot &runtimeProviders() {
    static CompilerProviderRegistry registry;
    static const auto snapshot = [] {
        registry.registerProvider(CompilerProviderDescriptor{
            .id = std::string{kRuntimeProvider},
            .kind = CompilerProviderKind::target_lowering,
            .content_hash =
                "builtin:runtime-adaptive-attachment-lowering-v3",
        });
        return registry.snapshot();
    }();
    return snapshot;
}

TargetTopologySnapshot runtimeTopology(
    const RenderingTargetPlanDeviceFacts &facts,
    bool enable_tile_local) {
    std::vector<std::string> capabilities{
        "pelican.vulkan.graphics@1",
        "pelican.vulkan.sampled_image@1",
        "pelican.vulkan.storage_buffer@1",
        "pelican.vulkan.transfer_copy@1",
    };
    std::vector<TargetFact> target_facts{
        {"pelican.vulkan.max_color_attachments@1",
         std::to_string(facts.max_color_attachments)},
        {"pelican.vulkan.profile@1",
         facts.transient_attachments
             ? "adaptive_attachment_runtime"
             : "materialized_runtime"},
    };
    if (facts.transient_attachments) {
        capabilities.push_back(
            std::string{
                kTransientAttachmentCapability});
    }
    if (enable_tile_local &&
        facts.dynamic_rendering_local_read) {
        capabilities.push_back(
            std::string{kTileBasedCapability});
        capabilities.push_back(
            std::string{kLocalReadCapability});
    }
    if (facts.device_identity.vendor_id != 0) {
        target_facts.push_back(
            {std::string{vulkanVendorIdFact},
             std::to_string(
                 facts.device_identity.vendor_id)});
        target_facts.push_back(
            {std::string{vulkanDeviceIdFact},
             std::to_string(
                 facts.device_identity.device_id)});
        target_facts.push_back(
            {std::string{vulkanDriverVersionFact},
             std::to_string(
                 facts.device_identity.driver_version)});
        target_facts.push_back(
            {std::string{vulkanDeviceNameFact},
             facts.device_identity.device_name});
    }
    if (facts.multiview) {
        if (facts.max_multiview_view_count == 0) {
            throw std::runtime_error(
                "runtime Vulkan multiview capability requires a "
                "non-zero max view count");
        }
        capabilities.push_back(
            std::string{vulkanMultiviewCapability});
        target_facts.push_back(
            {std::string{vulkanMaxMultiviewViewCountFact},
             std::to_string(
                 facts.max_multiview_view_count)});
    }
    return TargetTopologySnapshot{
        .name = "runtime_vulkan_device",
        .endpoints =
            {TargetEndpoint{
                .id = "device:0",
                .kind = TargetEndpointKind::vulkan_device,
                .capabilities = std::move(capabilities),
                .facts = std::move(target_facts),
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
                   std::less<>> &render_targets,
    const TileLocalAttachmentFormatSet
        &tile_local_formats = {}) {
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
        const auto tile_local =
            tile_local_formats.contains({
                resource,
                vk::to_string(
                    target->second->format)});
        result.resource_types.push_back({
            .resource = resource,
            .type = isDepthTarget(*target->second)
                        ? deviceDepthV1(types)
                        : legacyOpaqueImageV1(types),
            .materialization =
                isTransientAttachmentCandidate(
                    *target->second) ||
                        tile_local
                    ? LogicalMaterializationRequirement::
                          virtual_resource
                    : LogicalMaterializationRequirement::
                          required,
        });
    }
    return result;
}

ResourcePattern runtimeImagePattern(
    const LogicalTypeRegistry &types,
    const LogicalType &type, vk::Format format,
    std::span<const vk::Format> authored_candidates,
    bool external,
    bool transient_attachment,
    bool tile_local_attachment) {
    auto candidates =
        std::vector<ResourceFormatCandidate>{};
    const auto append = [&](vk::Format candidate) {
        const auto name = vk::to_string(candidate);
        if (std::none_of(
                candidates.begin(), candidates.end(),
                [&](const auto &existing) {
                    return existing.format == name;
                })) {
            candidates.push_back(
                ResourceFormatCandidate{
                    .format = name,
                });
        }
    };
    append(format);
    for (const auto candidate :
         authored_candidates) {
        append(candidate);
    }
    return ResourcePattern{
        .id =
            external
                ? "pelican.render.runtime_external_image@1"
            : tile_local_attachment
                ? "pelican.render.runtime_tile_local_candidate@1"
            : transient_attachment
                ? "pelican.render.runtime_transient_attachment@1"
                : "pelican.render.runtime_materialized_image@1",
        .applicable_type =
            exactLogicalTypePattern(types, type),
        .format_candidates = std::move(candidates),
        .prefer_transient = transient_attachment,
        .allow_tile_local = tile_local_attachment,
        .allow_alias = false,
        .require_store =
            !transient_attachment &&
            !tile_local_attachment,
        .local_read_fallback =
            ResourcePatternFallback::materialize,
        .provenance =
            tile_local_attachment
                ? "builtin:runtime-tile-local-attachment-v1"
            : transient_attachment
                ? "builtin:runtime-transient-attachment-v1"
                : "builtin:runtime-materialized-render-target-v1",
    };
}

ResourceExtentPlan runtimeExtentPlan(
    const RenderTargetDefinition &target) {
    if (target.fixed_extent) {
        return ResourceExtentPlan{
            .kind = ResourceExtentKind::fixed,
            .width = target.fixed_extent->width,
            .height = target.fixed_extent->height,
        };
    }
    return ResourceExtentPlan{
        .kind = ResourceExtentKind::output_relative,
        .scale_x = target.extent_scale,
        .scale_y = target.extent_scale,
    };
}

std::vector<ResourcePatternBinding> runtimePatternBindings(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &logical_graph,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets,
    vk::Format swapchain_format,
    const TransientAttachmentFormatSet
        &transient_formats,
    const TileLocalAttachmentFormatSet
        &tile_local_formats) {
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
                    types, resource.type, swapchain_format,
                    {}, true, false, false),
                ResourceExtentPlan{},
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
                types, resource.type,
                target->second->format,
                target->second->format_candidates,
                false,
                transient_formats.contains({
                    resource.name,
                    vk::to_string(
                        target->second->format)}),
                tile_local_formats.contains({
                    resource.name,
                    vk::to_string(
                        target->second->format)})),
            runtimeExtentPlan(*target->second),
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
        if (node.kind != FramePlanNodeKind::render) {
            continue;
        }
        for (const auto &attachment :
             node.attachments) {
            const auto &resource =
                attachment.resource;
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

VulkanPhysicalAttachmentAspect physicalAttachmentAspect(
    FrameGraphAttachmentAspect aspect) {
    switch (aspect) {
    case FrameGraphAttachmentAspect::color:
        return VulkanPhysicalAttachmentAspect::color;
    case FrameGraphAttachmentAspect::depth:
        return VulkanPhysicalAttachmentAspect::depth;
    }
    throw std::runtime_error(
        "unknown frame graph attachment aspect");
}

VulkanPhysicalAttachmentLoadOp physicalAttachmentLoadOp(
    FrameGraphAttachmentLoadOp op) {
    switch (op) {
    case FrameGraphAttachmentLoadOp::load:
        return VulkanPhysicalAttachmentLoadOp::load;
    case FrameGraphAttachmentLoadOp::clear:
        return VulkanPhysicalAttachmentLoadOp::clear;
    case FrameGraphAttachmentLoadOp::discard:
        return VulkanPhysicalAttachmentLoadOp::discard;
    }
    throw std::runtime_error(
        "unknown frame graph attachment load op");
}

VulkanPhysicalAttachmentStoreOp physicalAttachmentStoreOp(
    FrameGraphAttachmentStoreOp op) {
    switch (op) {
    case FrameGraphAttachmentStoreOp::store:
        return VulkanPhysicalAttachmentStoreOp::store;
    case FrameGraphAttachmentStoreOp::discard:
        return VulkanPhysicalAttachmentStoreOp::discard;
    }
    throw std::runtime_error(
        "unknown frame graph attachment store op");
}

std::vector<VulkanPhysicalAttachmentPlan>
physicalAttachmentPlans(
    const FrameGraphDefinition &definition) {
    std::vector<VulkanPhysicalAttachmentPlan> result;
    for (const auto &node : definition.nodes) {
        result.reserve(
            result.size() +
            node.attachments.size());
        for (const auto &attachment :
             node.attachments) {
            result.push_back(
                VulkanPhysicalAttachmentPlan{
                    .node = node.name,
                    .logical_resource =
                        attachment.resource,
                    .aspect =
                        physicalAttachmentAspect(
                            attachment.aspect),
                    .load_op =
                        physicalAttachmentLoadOp(
                            attachment.load_op),
                    .store_op =
                        physicalAttachmentStoreOp(
                            attachment.store_op),
                });
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

std::vector<vk::Format> declaredFormats(
    const RenderTargetDefinition &target) {
    auto result = std::vector<vk::Format>{
        target.format};
    for (const auto format :
         target.format_candidates) {
        if (std::find(
                result.begin(), result.end(),
                format) == result.end()) {
            result.push_back(format);
        }
    }
    return result;
}

RenderingImageFormatCapability queryFormatCapability(
    const RenderingTargetPlanDeviceFacts &facts,
    const RenderTargetDefinition &target,
    vk::Format format) {
    auto candidate = target;
    candidate.format = format;
    if (facts.query_image_format_capability) {
        return facts.query_image_format_capability(
            candidate);
    }
    // Compatibility for pure callers that only provided the original
    // sample/depth callbacks. They prove the authored automatic format, but
    // never authorize an alternate format.
    if (format != target.format) {
        return {};
    }
    return RenderingImageFormatCapability{
        .image_usage_supported = true,
        .supported_samples =
            isAttachmentTarget(candidate) &&
                    facts.query_attachment_samples
                ? facts.query_attachment_samples(
                      candidate)
                : std::vector<std::uint32_t>{1},
        .max_array_layers =
            std::numeric_limits<
                std::uint32_t>::max(),
        .external_depth_export_supported =
            facts.supports_external_depth_transfer
                ? facts
                      .supports_external_depth_transfer(
                          candidate)
                : true,
    };
}

TransientAttachmentFormatSet
transientAttachmentFormats(
    std::span<const RenderTargetDefinition> render_targets,
    const RenderingTargetPlanDeviceFacts &facts) {
    TransientAttachmentFormatSet result;
    if (!facts.transient_attachments) {
        return result;
    }
    for (const auto &target : render_targets) {
        if (!isTransientAttachmentCandidate(target)) {
            continue;
        }
        for (const auto format :
             declaredFormats(target)) {
            const auto capability =
                queryFormatCapability(
                    facts, target, format);
            if (capability.image_usage_supported &&
                capability
                    .transient_attachment_supported) {
                result.emplace(
                    target.name,
                    vk::to_string(format));
            }
        }
    }
    return result;
}

TileLocalAttachmentFormatSet
tileLocalAttachmentFormats(
    std::span<const RenderTargetDefinition> render_targets,
    const RenderingTargetPlanDeviceFacts &facts) {
    TileLocalAttachmentFormatSet result;
    if (!facts.transient_attachments ||
        !facts.dynamic_rendering_local_read) {
        return result;
    }
    for (const auto &target : render_targets) {
        if (!isTileLocalAttachmentCandidate(
                target)) {
            continue;
        }
        for (const auto format :
             declaredFormats(target)) {
            const auto capability =
                queryFormatCapability(
                    facts, target, format);
            if (capability.image_usage_supported &&
                capability
                    .local_read_attachment_supported) {
                result.emplace(
                    target.name,
                    vk::to_string(format));
            }
        }
    }
    return result;
}

bool nodeHasSamePixelRead(
    const FrameGraphNodeDefinition &node,
    std::string_view resource) {
    const auto footprint = std::find_if(
        node.read_footprints.begin(),
        node.read_footprints.end(),
        [&](const auto &candidate) {
            return candidate.resource == resource;
        });
    return footprint !=
               node.read_footprints.end() &&
           footprint->footprint.kind ==
               LogicalReadFootprintKind::
                   same_pixel;
}

bool nodeAttachmentsMatchLocalReadExtent(
    const FrameGraphNodeDefinition &node,
    const RenderTargetDefinition &local_target,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets) {
    if (node.attachments.empty()) {
        return false;
    }
    const auto local_extent =
        runtimeExtentPlan(local_target);
    for (const auto &attachment :
         node.attachments) {
        if (attachment.resource == "swapchain") {
            return false;
        }
        const auto target =
            render_targets.find(
                attachment.resource);
        if (target == render_targets.end() ||
            runtimeExtentPlan(*target->second) !=
                local_extent) {
            return false;
        }
    }
    return true;
}

TileLocalAttachmentFormatSet
graphTileLocalAttachmentFormats(
    const FrameGraphDefinition &definition,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets,
    const TileLocalAttachmentFormatSet
        &device_formats) {
    TileLocalAttachmentFormatSet result;
    for (const auto &[name, target] :
         render_targets) {
        const auto format =
            vk::to_string(target->format);
        if (!device_formats.contains(
                {name, format})) {
            continue;
        }
        bool read = false;
        bool written = false;
        bool compatible = true;
        for (const auto &node : definition.nodes) {
            if (std::find(
                    node.reads_history.begin(),
                    node.reads_history.end(),
                    name) !=
                node.reads_history.end()) {
                compatible = false;
                break;
            }
            const auto reads =
                std::find(
                    node.reads.begin(),
                    node.reads.end(),
                    name) != node.reads.end();
            if (reads) {
                read = true;
                if (node.kind !=
                        FramePlanNodeKind::render ||
                    node.raster_geometry ||
                    !nodeHasSamePixelRead(
                        node, name) ||
                    !nodeAttachmentsMatchLocalReadExtent(
                        node, *target,
                        render_targets)) {
                    compatible = false;
                    break;
                }
            }
            const auto writes =
                std::find(
                    node.writes.begin(),
                    node.writes.end(),
                    name) != node.writes.end();
            if (writes) {
                written = true;
                if (std::none_of(
                        node.attachments.begin(),
                        node.attachments.end(),
                        [&](const auto &attachment) {
                            return attachment.resource ==
                                   name;
                        })) {
                    compatible = false;
                    break;
                }
            }
        }
        if (compatible && read && written) {
            result.emplace(name, format);
        }
    }
    return result;
}

std::vector<VulkanPhysicalResourceFormatCapability>
physicalFormatCapabilities(
    const CompiledLogicalRenderGraph &logical_graph,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets,
    const RenderingTargetPlanDeviceFacts &facts) {
    auto result =
        std::vector<
            VulkanPhysicalResourceFormatCapability>{};
    for (const auto &resource :
         logical_graph.resources) {
        if (resource.type.constructor !=
                LogicalTypeConstructor::image ||
            resource.name == "swapchain") {
            continue;
        }
        const auto target =
            render_targets.find(resource.name);
        if (target == render_targets.end()) {
            throw std::runtime_error(
                "runtime format capability has no "
                "RenderTargetDefinition: " +
                resource.name);
        }
        for (const auto format :
             declaredFormats(*target->second)) {
            const auto capability =
                queryFormatCapability(
                    facts, *target->second,
                    format);
            result.push_back({
                .logical_resource =
                    resource.name,
                .format = vk::to_string(format),
                .image_usage_supported =
                    capability
                        .image_usage_supported,
                .supported_samples =
                    capability
                        .supported_samples,
                .max_array_layers =
                    capability.max_array_layers,
                .external_depth_export_supported =
                    capability
                        .external_depth_export_supported,
            });
        }
    }
    return result;
}

const VulkanPhysicalResourceFormatCapability &
requireFormatCapability(
    std::span<
        const VulkanPhysicalResourceFormatCapability>
        capabilities,
    std::string_view resource,
    std::string_view format) {
    const auto found = std::find_if(
        capabilities.begin(),
        capabilities.end(),
        [&](const auto &capability) {
            return capability.logical_resource ==
                       resource &&
                   capability.format == format;
        });
    if (found == capabilities.end()) {
        throw std::runtime_error(
            "runtime target format capability is missing: " +
            std::string{resource} + " -> " +
            std::string{format});
    }
    return *found;
}

std::vector<SampleCountResourceCapability> sampleCapabilities(
    const std::set<std::string, std::less<>> &attachments,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets,
    vk::Format swapchain_format,
    std::span<
        const VulkanPhysicalResourceFormatCapability>
        format_capabilities) {
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
        const auto &capability =
            requireFormatCapability(
                format_capabilities,
                resource,
                vk::to_string(target.format));
        if (!capability.image_usage_supported) {
            throw std::runtime_error(
                "automatic render-target format does not support "
                "the required image usage: " +
                resource + " -> " +
                vk::to_string(target.format));
        }
        result.push_back({
            .resource = resource,
            .format = vk::to_string(target.format),
            .supported_samples =
                capability.supported_samples,
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

const VulkanPhysicalResourcePlan *findPhysicalResource(
    const VulkanTargetPlan &plan, std::string_view resource) {
    const auto found = std::lower_bound(
        plan.resources.begin(), plan.resources.end(), resource,
        [](const VulkanPhysicalResourcePlan &candidate,
           std::string_view name) {
            return candidate.logical_resource < name;
        });
    return found == plan.resources.end() ||
                   found->logical_resource != resource
               ? nullptr
               : &*found;
}

VulkanRenderResolutionPlan makeRuntimeResolutionPlan(
    const FrameGraphDefinition &definition,
    const VulkanTargetPlan &target_plan) {
    std::set<std::string, std::less<>> scene_resources;
    for (const auto &node : definition.nodes) {
        if (node.resolution_domain !=
            RenderResolutionDomain::scene) {
            continue;
        }
        for (const auto &resource : node.writes) {
            if (!resource.empty()) {
                scene_resources.insert(resource);
            }
        }
    }

    std::string output_resource = "swapchain";
    const auto *output =
        findPhysicalResource(target_plan, output_resource);
    if (output == nullptr) {
        for (auto node = definition.nodes.rbegin();
             node != definition.nodes.rend() &&
             output == nullptr;
             ++node) {
            for (auto resource = node->writes.rbegin();
                 resource != node->writes.rend();
                 ++resource) {
                output =
                    findPhysicalResource(
                        target_plan, *resource);
                if (output != nullptr) {
                    output_resource = *resource;
                    break;
                }
            }
        }
    }
    if (output == nullptr || !output->extent) {
        throw std::runtime_error(
            "runtime output resource has no physical extent contract");
    }

    VulkanRenderResolutionPlan result;
    result.output_source_resource =
        std::move(output_resource);
    result.output_extent = *output->extent;
    result.scene_resources.assign(
        scene_resources.begin(), scene_resources.end());
    if (scene_resources.empty()) {
        result.render_source_resource =
            result.output_source_resource;
        result.render_extent = *output->extent;
        return result;
    }

    for (const auto &resource : scene_resources) {
        const auto &physical =
            requirePhysicalResource(target_plan, resource);
        if (!physical.extent) {
            throw std::runtime_error(
                "scene resolution resource has no physical extent contract: " +
                resource);
        }
        if (result.render_source_resource.empty()) {
            result.render_source_resource = resource;
            result.render_extent = *physical.extent;
            continue;
        }
        if (result.render_extent != *physical.extent) {
            throw std::runtime_error(
                "scene resolution domain has incompatible physical extents at '" +
                result.render_source_resource + "' and '" +
                resource +
                "'; align their extents or override resolution_domain");
        }
    }
    return result;
}

void validateRuntimePhysicalPlan(
    const VulkanTargetPlan &plan,
    const std::map<std::string,
                   const RenderTargetDefinition *,
                   std::less<>> &render_targets,
    vk::Format swapchain_format,
    std::span<
        const VulkanPhysicalResourceFormatCapability>
        format_capabilities,
    const TransientAttachmentFormatSet
        &transient_formats,
    const TileLocalAttachmentFormatSet
        &tile_local_formats) {
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
            if (!resource.extent ||
                *resource.extent != ResourceExtentPlan{}) {
                throw std::runtime_error(
                    "runtime swapchain physical extent contract is incompatible");
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
        const auto transient =
            resource.representation ==
            VulkanResourceRepresentation::
                transient_attachment;
        const auto tile_local =
            resource.representation ==
            VulkanResourceRepresentation::
                tile_local_attachment;
        if (resource.representation !=
                VulkanResourceRepresentation::
                    materialized_image &&
            !transient &&
            !tile_local) {
            throw std::runtime_error(
                "current render-target runtime cannot consume "
                "physical representation for: " +
                resource.logical_resource);
        }
        if (transient || tile_local) {
            if ((transient &&
                 !isTransientAttachmentCandidate(
                     *target->second)) ||
                (tile_local &&
                 !isTileLocalAttachmentCandidate(
                     *target->second))) {
                throw std::runtime_error(
                    "runtime scope-local attachment is not supported "
                    "by the target format/usage contract: " +
                    resource.logical_resource);
            }
            if (resource.stored ||
                resource.resolve_required ||
                resource.rasterization_samples != 1) {
                throw std::runtime_error(
                    "runtime scope-local attachment must be "
                    "single-sample, unresolved, and unstored: " +
                    resource.logical_resource);
            }
            const auto first_attachment =
                std::find_if(
                    plan.attachments.begin(),
                    plan.attachments.end(),
                    [&](const auto &attachment) {
                        return attachment
                                   .logical_resource ==
                               resource.logical_resource;
                    });
            if (first_attachment ==
                    plan.attachments.end() ||
                std::any_of(
                    first_attachment,
                    plan.attachments.end(),
                    [&](const auto &attachment) {
                        return attachment
                                       .logical_resource ==
                                   resource.logical_resource &&
                               attachment.store_op !=
                                   VulkanPhysicalAttachmentStoreOp::
                                       discard;
                    })) {
                throw std::runtime_error(
                    "runtime scope-local attachment requires every "
                    "writer to discard its store: " +
                    resource.logical_resource);
            }
        }
        const auto formats =
            declaredFormats(*target->second);
        const auto selected_format =
            std::find_if(
                formats.begin(), formats.end(),
                [&](const auto format) {
                    return vk::to_string(format) ==
                           resource.format;
                });
        if (selected_format == formats.end()) {
            throw std::runtime_error(
                "runtime physical format is not a declared "
                "candidate for '" +
                resource.logical_resource + "': planned '" +
                resource.format + "'");
        }
        if (transient &&
            !transient_formats.contains({
                resource.logical_resource,
                resource.format})) {
            throw std::runtime_error(
                "runtime transient attachment format is not "
                "supported by the target device: " +
                resource.logical_resource + " -> " +
                resource.format);
        }
        if (tile_local &&
            !tile_local_formats.contains({
                resource.logical_resource,
                resource.format})) {
            throw std::runtime_error(
                "runtime tile-local attachment format is not "
                "supported by the target device or pass contract: " +
                resource.logical_resource + " -> " +
                resource.format);
        }
        const auto &capability =
            requireFormatCapability(
                format_capabilities,
                resource.logical_resource,
                resource.format);
        if (!capability.image_usage_supported) {
            throw std::runtime_error(
                "runtime physical format does not support the "
                "required image usage for '" +
                resource.logical_resource + "': " +
                resource.format);
        }
        if (std::find(
                capability.supported_samples.begin(),
                capability.supported_samples.end(),
                resource.rasterization_samples) ==
            capability.supported_samples.end()) {
            throw std::runtime_error(
                "runtime physical format does not support " +
                std::to_string(
                    resource.rasterization_samples) +
                " samples for '" +
                resource.logical_resource + "': " +
                resource.format);
        }
        if (resource.array_layers >
            capability.max_array_layers) {
            throw std::runtime_error(
                "runtime physical format does not support " +
                std::to_string(resource.array_layers) +
                " array layers for '" +
                resource.logical_resource + "': " +
                resource.format);
        }
        if (plan.external_depth_export &&
            plan.external_depth_export
                    ->source_resource ==
                resource.logical_resource &&
            !capability
                 .external_depth_export_supported) {
            throw std::runtime_error(
                "runtime physical depth format does not support "
                "the external transfer-source contract for '" +
                resource.logical_resource + "': " +
                resource.format);
        }
        if (!resource.extent ||
            *resource.extent !=
                runtimeExtentPlan(*target->second)) {
            throw std::runtime_error(
                "runtime physical extent mismatch for '" +
                resource.logical_resource + "'");
        }
    }
}

} // namespace

std::vector<CompiledLogicalRenderGraph>
compileRenderingLogicalGraphs(
    std::span<const FrameGraphDefinition> frame_graphs,
    std::span<const RenderTargetDefinition> render_targets) {
    const auto target_by_name =
        renderTargetsByName(render_targets);
    const auto types = makeBuiltinLogicalTypeRegistry();
    std::vector<CompiledLogicalRenderGraph> result;
    result.reserve(frame_graphs.size());
    std::set<std::string, std::less<>>
        graph_names;
    for (const auto &definition : frame_graphs) {
        if (definition.name.empty() ||
            !graph_names.insert(definition.name).second) {
            throw std::runtime_error(
                "logical rendering graph names must be unique and "
                "non-empty: " +
                definition.name);
        }
        result.push_back(
            compileLogicalFrameGraphShadow(
                definition, types,
                shadowOptions(
                    definition, types,
                    target_by_name)));
    }
    return result;
}

RenderingTargetPlanCompilation compileRenderingTargetPlans(
    std::span<const FrameGraphDefinition> frame_graphs,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    vk::Format swapchain_format,
    const RenderingTargetPlanDeviceFacts &device_facts,
    std::optional<VulkanViewExecutionPlanRequest>
        view_execution,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export,
    TargetPlanningPolicy target_planning,
    std::span<const VulkanTargetPlanPinPackage>
        plan_pins,
    std::span<const VulkanPhysicalFragmentPackage>
        physical_fragments) {
    const auto target_by_name =
        renderTargetsByName(render_targets);
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto transient_formats =
        transientAttachmentFormats(
            render_targets, device_facts);
    const auto device_tile_local_formats =
        tileLocalAttachmentFormats(
            render_targets, device_facts);
    if (external_depth_export &&
        (device_facts
             .query_image_format_capability ||
         device_facts
             .supports_external_depth_transfer)) {
        std::vector<std::string> compatible;
        for (const auto &target : render_targets) {
            if (isDepthTarget(target) &&
                queryFormatCapability(
                    device_facts, target,
                    target.format)
                    .external_depth_export_supported) {
                compatible.push_back(target.name);
            }
        }
        std::sort(
            compatible.begin(), compatible.end());
        external_depth_export
            ->compatible_source_resources =
            std::move(compatible);
    }

    std::vector<std::set<std::string, std::less<>>>
        graph_attachments;
    graph_attachments.reserve(frame_graphs.size());
    std::set<std::string, std::less<>> all_attachments;
    std::set<std::string, std::less<>> graph_names;
    for (const auto &definition : frame_graphs) {
        if (!graph_names.insert(definition.name).second) {
            throw std::runtime_error(
                "rendering target planning graph names must be unique: " +
                definition.name);
        }
        auto attachments =
            attachmentResources(definition, target_by_name);
        all_attachments.insert(attachments.begin(),
                               attachments.end());
        graph_attachments.push_back(std::move(attachments));
    }
    std::map<std::string, const PlanningGraphConstraints *,
             std::less<>>
        planning_by_graph;
    for (const auto &constraints : target_planning.graphs) {
        if (constraints.graph.empty() ||
            !graph_names.contains(constraints.graph)) {
            throw std::runtime_error(
                "target planning constraints reference unknown graph: " +
                constraints.graph);
        }
        if (!planning_by_graph
                 .emplace(constraints.graph, &constraints)
                 .second) {
            throw std::runtime_error(
                "duplicate target planning constraints for graph: " +
                constraints.graph);
        }
    }
    std::map<std::string,
             const VulkanTargetPlanPinPackage *,
             std::less<>>
        pins_by_graph;
    for (const auto &package : plan_pins) {
        if (package.graph.empty() ||
            !graph_names.contains(package.graph)) {
            throw std::runtime_error(
                "Vulkan target plan pins reference unknown graph: " +
                package.graph);
        }
        if (!pins_by_graph
                 .emplace(package.graph, &package)
                 .second) {
            throw std::runtime_error(
                "duplicate Vulkan target plan pins for graph: " +
                package.graph);
        }
    }
    std::map<std::string,
             const VulkanPhysicalFragmentPackage *,
             std::less<>>
        fragments_by_graph;
    for (const auto &package : physical_fragments) {
        if (package.graph.empty() ||
            !graph_names.contains(package.graph)) {
            throw std::runtime_error(
                "Vulkan physical fragments reference unknown "
                "graph: " +
                package.graph);
        }
        if (!fragments_by_graph
                 .emplace(package.graph, &package)
                 .second) {
            throw std::runtime_error(
                "duplicate Vulkan physical fragments for graph: " +
                package.graph);
        }
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
    std::map<std::string, std::uint32_t, std::less<>>
        merged_array_layers;
    std::map<std::string, vk::Format, std::less<>>
        merged_formats;
    std::map<std::string, VulkanResourceRepresentation,
             std::less<>>
        merged_representations;
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

        const auto tile_local_formats =
            graphTileLocalAttachmentFormats(
                definition, target_by_name,
                device_tile_local_formats);
        // Keep the tile backend out of candidate selection when this graph
        // has no resource that the current runtime can actually execute as a
        // local attachment. Otherwise an equivalent tile draft can win a
        // deterministic tie without providing any tile-local work.
        const auto topology =
            runtimeTopology(
                device_facts,
                !tile_local_formats.empty());
        auto logical_graph = compileLogicalFrameGraphShadow(
            definition, types,
            shadowOptions(
                definition, types, target_by_name,
                tile_local_formats));
        const auto format_capabilities =
            physicalFormatCapabilities(
                logical_graph, target_by_name,
                device_facts);
        const auto planning =
            planning_by_graph.find(definition.name);
        const auto *graph_constraints =
            planning == planning_by_graph.end()
                ? nullptr
                : planning->second;
        const auto pin =
            pins_by_graph.find(definition.name);
        const auto fragment =
            fragments_by_graph.find(
                definition.name);
        auto plan_value =
            compileVulkanTargetPlan(
                types, logical_graph, topology,
                runtimeProviders(),
                VulkanTargetPlanRequest{
                    .endpoint = "device:0",
                    .provider = std::string{kRuntimeProvider},
                    .pattern_bindings =
                        runtimePatternBindings(
                            types, logical_graph, target_by_name,
                            swapchain_format,
                            transient_formats,
                            tile_local_formats),
                    .profile = target_planning.profile,
                    .node_constraints =
                        graph_constraints == nullptr
                            ? std::vector<PlanningNodeConstraint>{}
                            : graph_constraints->nodes,
                    .resource_constraints =
                        graph_constraints == nullptr
                            ? std::vector<
                                  PlanningResourceConstraint>{}
                            : graph_constraints->resources,
                    .diagnostic_policy =
                        target_planning.diagnostic_policy,
                    .sample_count =
                        VulkanSampleCountPlanRequest{
                            .policy = std::move(local_policy),
                            .capabilities =
                                sampleCapabilities(
                                    graph_attachments[index],
                                    target_by_name,
                                    swapchain_format,
                                    format_capabilities),
                            .geometry_nodes =
                                geometryNodes(definition),
                    },
                    .view_execution = view_execution,
                    .external_depth_export =
                        external_depth_export,
                    .pin_package =
                        pin == pins_by_graph.end()
                            ? std::optional<
                                  VulkanTargetPlanPinPackage>{}
                            : std::optional<
                                  VulkanTargetPlanPinPackage>{
                                  *pin->second},
                    .fragment_package =
                        fragment ==
                                fragments_by_graph.end()
                            ? std::optional<
                                  VulkanPhysicalFragmentPackage>{}
                            : std::optional<
                                  VulkanPhysicalFragmentPackage>{
                                  *fragment->second},
                    .fragment_format_capabilities =
                        format_capabilities,
                    .automatic_attachments =
                        physicalAttachmentPlans(
                            definition),
                });
        plan_value.resolution_plan =
            makeRuntimeResolutionPlan(
                definition, plan_value);
        auto plan =
            std::make_shared<const VulkanTargetPlan>(
                std::move(plan_value));
        validateRuntimePhysicalPlan(
            *plan, target_by_name, swapchain_format,
            format_capabilities,
            transient_formats,
            tile_local_formats);
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
        for (const auto &resource : plan->resources) {
            if (resource.logical_resource == "swapchain" ||
                !target_by_name.contains(
                    resource.logical_resource)) {
                continue;
            }
            if (resource.array_layers == 0) {
                throw std::runtime_error(
                    "runtime physical array-layer contract is zero for render target '" +
                    resource.logical_resource + "'");
            }
            auto [found, inserted] =
                merged_array_layers.emplace(
                    resource.logical_resource,
                    resource.array_layers);
            if (!inserted) {
                found->second =
                    std::max(found->second,
                             resource.array_layers);
            }
            const auto &target =
                *target_by_name.at(
                    resource.logical_resource);
            const auto formats =
                declaredFormats(target);
            const auto selected_format =
                std::find_if(
                    formats.begin(), formats.end(),
                    [&](const auto format) {
                        return vk::to_string(format) ==
                               resource.format;
                    });
            if (selected_format ==
                formats.end()) {
                throw std::runtime_error(
                    "runtime physical format is not declared for "
                    "render target '" +
                    resource.logical_resource + "': " +
                    resource.format);
            }
            const auto [format_assignment,
                        format_inserted] =
                merged_formats.emplace(
                    resource.logical_resource,
                    *selected_format);
            if (!format_inserted &&
                format_assignment->second !=
                    *selected_format) {
                throw std::runtime_error(
                    "frame graphs require conflicting physical "
                    "formats for render target '" +
                    resource.logical_resource + "'");
            }
            const auto [representation_assignment,
                        representation_inserted] =
                merged_representations.emplace(
                    resource.logical_resource,
                    resource.representation);
            if (!representation_inserted &&
                representation_assignment->second !=
                    resource.representation) {
                const auto current =
                    representation_assignment->second;
                const auto incoming =
                    resource.representation;
                if (current ==
                        VulkanResourceRepresentation::
                            materialized_image ||
                    incoming ==
                        VulkanResourceRepresentation::
                            materialized_image) {
                    representation_assignment->second =
                        VulkanResourceRepresentation::
                            materialized_image;
                } else if (
                    current ==
                            VulkanResourceRepresentation::
                                tile_local_attachment ||
                    incoming ==
                            VulkanResourceRepresentation::
                                tile_local_attachment) {
                    representation_assignment->second =
                        VulkanResourceRepresentation::
                            tile_local_attachment;
                } else if (
                    current !=
                            VulkanResourceRepresentation::
                                transient_attachment ||
                    incoming !=
                            VulkanResourceRepresentation::
                                transient_attachment) {
                    throw std::runtime_error(
                        "frame graphs require incompatible physical "
                        "representations for render target '" +
                        resource.logical_resource + "'");
                }
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
    result.array_layer_assignments.reserve(
        render_targets.size());
    for (const auto &target : render_targets) {
        const auto found =
            merged_array_layers.find(target.name);
        result.array_layer_assignments.push_back({
            .resource = target.name,
            .array_layers =
                found == merged_array_layers.end()
                    ? 1u
                    : found->second,
        });
    }
    std::sort(
        result.array_layer_assignments.begin(),
        result.array_layer_assignments.end(),
        [](const auto &left, const auto &right) {
            return left.resource < right.resource;
        });
    result.format_assignments.reserve(
        render_targets.size());
    for (const auto &target : render_targets) {
        const auto found =
            merged_formats.find(target.name);
        result.format_assignments.push_back({
            .resource = target.name,
            .format =
                found == merged_formats.end()
                    ? target.format
                    : found->second,
        });
    }
    std::sort(
        result.format_assignments.begin(),
        result.format_assignments.end(),
        [](const auto &left, const auto &right) {
            return left.resource < right.resource;
        });
    result.representation_assignments.reserve(
        render_targets.size());
    for (const auto &target : render_targets) {
        const auto found =
            merged_representations.find(target.name);
        result.representation_assignments.push_back({
            .resource = target.name,
            .representation =
                found == merged_representations.end()
                    ? VulkanResourceRepresentation::
                          materialized_image
                    : found->second,
        });
    }
    std::sort(
        result.representation_assignments.begin(),
        result.representation_assignments.end(),
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
    vk::PhysicalDevice physical_device,
    std::optional<VulkanViewExecutionPlanRequest>
        view_execution,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export,
    TargetPlanningPolicy target_planning,
    std::span<const VulkanTargetPlanPinPackage>
        plan_pins,
    std::span<const VulkanPhysicalFragmentPackage>
        physical_fragments) {
    const auto features =
        physical_device.getFeatures2<
            vk::PhysicalDeviceFeatures2,
            vk::PhysicalDeviceVulkan11Features>();
    const auto properties =
        physical_device.getProperties2<
            vk::PhysicalDeviceProperties2,
            vk::PhysicalDeviceMultiviewProperties>();
    const auto multiview =
        features
            .get<vk::PhysicalDeviceVulkan11Features>()
            .multiview == VK_TRUE;
    const auto device_properties =
        physical_device.getProperties();
    const auto dynamic_rendering_local_read =
        supportsDynamicRenderingLocalRead(
            physical_device);
    return compileRenderingTargetPlans(
        frame_graphs, render_targets, policy, swapchain_format,
        RenderingTargetPlanDeviceFacts{
            .max_color_attachments =
                device_properties
                    .limits.maxColorAttachments,
            .multiview = multiview,
            .max_multiview_view_count =
                multiview
                    ? properties
                          .get<
                              vk::PhysicalDeviceMultiviewProperties>()
                          .maxMultiviewViewCount
                    : 0u,
            .device_identity = {
                .vendor_id =
                    device_properties.vendorID,
                .device_id =
                    device_properties.deviceID,
                .driver_version =
                    device_properties.driverVersion,
                .device_name =
                    device_properties
                        .deviceName.data(),
            },
            .query_attachment_samples =
                [physical_device](
                    const RenderTargetDefinition &definition) {
                    return queryAttachmentSampleCounts(
                        physical_device, definition);
                },
            .supports_external_depth_transfer =
                [physical_device](
                    const RenderTargetDefinition &definition) {
                    return bool(
                        physical_device
                            .getFormatProperties(
                                definition.format)
                            .optimalTilingFeatures &
                        vk::FormatFeatureFlagBits::
                            eTransferSrc);
                },
            .query_image_format_capability =
                [physical_device](
                    const RenderTargetDefinition &definition) {
                    return queryImageFormatCapability(
                        physical_device,
                        definition);
                },
            .transient_attachments = true,
            .dynamic_rendering_local_read =
                dynamic_rendering_local_read,
        },
        std::move(view_execution),
        std::move(external_depth_export),
        std::move(target_planning),
        plan_pins,
        physical_fragments);
}

void applyRenderingTargetPlan(
    std::span<RenderTargetDefinition> render_targets,
    const RenderingTargetPlanCompilation &compilation) {
    std::map<std::string, std::uint32_t, std::less<>>
        assignments;
    std::map<std::string, std::uint32_t, std::less<>>
        array_layer_assignments;
    std::map<std::string, vk::Format, std::less<>>
        format_assignments;
    std::map<std::string, VulkanResourceRepresentation,
             std::less<>>
        representation_assignments;
    std::set<std::string, std::less<>>
        external_depth_sources;
    std::set<std::string, std::less<>>
        local_read_resources;
    for (const auto &assignment : compilation.assignments) {
        if (!assignments.emplace(assignment.resource,
                                 assignment.samples)
                 .second) {
            throw std::runtime_error(
                "duplicate physical sample-count assignment: " +
                assignment.resource);
        }
    }
    for (const auto &assignment :
         compilation.array_layer_assignments) {
        if (assignment.array_layers == 0 ||
            !array_layer_assignments
                 .emplace(assignment.resource,
                          assignment.array_layers)
                 .second) {
            throw std::runtime_error(
                "duplicate or invalid physical array-layer assignment: " +
                assignment.resource);
        }
    }
    for (const auto &assignment :
         compilation.format_assignments) {
        if (assignment.format ==
                vk::Format::eUndefined ||
            !format_assignments
                 .emplace(assignment.resource,
                          assignment.format)
                 .second) {
            throw std::runtime_error(
                "duplicate or invalid physical format "
                "assignment: " +
                assignment.resource);
        }
    }
    for (const auto &assignment :
         compilation.representation_assignments) {
        if (!representation_assignments
                 .emplace(assignment.resource,
                          assignment.representation)
                 .second) {
            throw std::runtime_error(
                "duplicate physical representation assignment: " +
                assignment.resource);
        }
    }
    for (const auto &plan : compilation.plans) {
        if (plan == nullptr) continue;
        if (plan->external_depth_export) {
            external_depth_sources.insert(
                plan->external_depth_export
                    ->source_resource);
        }
        for (const auto &resource :
             plan->resources) {
            if (resource.representation ==
                VulkanResourceRepresentation::
                    tile_local_attachment) {
                local_read_resources.insert(
                    resource.logical_resource);
            }
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
        const auto layers =
            array_layer_assignments.find(target.name);
        if (layers ==
            array_layer_assignments.end()) {
            throw std::runtime_error(
                "missing physical array-layer assignment: " +
                target.name);
        }
        target.array_layers = layers->second;
        const auto format =
            format_assignments.find(target.name);
        if (format ==
            format_assignments.end()) {
            throw std::runtime_error(
                "missing physical format assignment: " +
                target.name);
        }
        if (std::find(
                target.format_candidates.begin(),
                target.format_candidates.end(),
                format->second) ==
                target.format_candidates.end() &&
            format->second != target.format) {
            throw std::runtime_error(
                "physical format assignment is not an authored "
                "candidate: " +
                target.name);
        }
        target.format = format->second;
        const auto representation =
            representation_assignments.find(
                target.name);
        if (representation ==
            representation_assignments.end()) {
            throw std::runtime_error(
                "missing physical representation assignment: " +
                target.name);
        }
        switch (representation->second) {
        case VulkanResourceRepresentation::
            materialized_image:
            target.storage_mode =
                RenderTargetStorageMode::materialized;
            break;
        case VulkanResourceRepresentation::
            transient_attachment:
            if (!isTransientAttachmentCandidate(target) ||
                target.samples != 1) {
                throw std::runtime_error(
                    "physical transient attachment assignment is "
                    "incompatible with RenderTargetDefinition: " +
                    target.name);
            }
            target.storage_mode =
                RenderTargetStorageMode::
                    transient_attachment;
            break;
        case VulkanResourceRepresentation::
            tile_local_attachment:
            if (!isTileLocalAttachmentCandidate(
                    target) ||
                target.samples != 1) {
                throw std::runtime_error(
                    "physical tile-local attachment assignment is "
                    "incompatible with RenderTargetDefinition: " +
                    target.name);
            }
            target.storage_mode =
                RenderTargetStorageMode::
                    tile_local_attachment;
            break;
        default:
            throw std::runtime_error(
                "physical representation assignment is not "
                "implemented by the render-target runtime: " +
                target.name);
        }
        if (local_read_resources.contains(
                target.name)) {
            target.usage |=
                vk::ImageUsageFlagBits::
                    eInputAttachment;
        }
        if (external_depth_sources.contains(
                target.name)) {
            if (!(target.usage &
                  vk::ImageUsageFlagBits::
                      eDepthStencilAttachment)) {
                throw std::runtime_error(
                    "external depth export source is not a depth "
                    "attachment RenderTargetDefinition: " +
                    target.name);
            }
            target.usage |=
                vk::ImageUsageFlagBits::eTransferSrc;
        }
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
