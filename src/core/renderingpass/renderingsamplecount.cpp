#include "../../project/graphcomposition.hpp"
#include "renderingsamplecount.hpp"

#include "logicalframegraphadapter.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "vulkanimagevocabulary.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <vulkan/vulkan.hpp>

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
        std::pair<std::string, ImageFormat>,
        std::less<>>;
using TileLocalAttachmentFormatSet =
    std::set<
        std::pair<std::string, ImageFormat>,
        std::less<>>;

bool isAttachmentTarget(const RenderTargetDefinition &definition) {
    return bool(definition.usage &
                (ImageUsageBit::color_attachment |
                 ImageUsageBit::depth_stencil_attachment));
}

bool isDepthTarget(const RenderTargetDefinition &definition) {
    return bool(definition.usage &
                ImageUsageBit::depth_stencil_attachment);
}

// RC-3c1b: the eligibility rules are project's (rendergraphir.hpp), over
// the declaration facts; this is core's one copy of a render target's.
ImageDeclarationFacts declarationFactsOf(
    const RenderTargetDefinition &definition) {
    return ImageDeclarationFacts{
        .history = definition.history,
        .persistent = definition.persistent,
        .has_source = definition.source.has_value(),
        .usage = definition.usage,
        .format_class = definition.format_class,
    };
}

bool isTransientAttachmentCandidate(
    const RenderTargetDefinition &definition) {
    return Pelican::isTransientAttachmentCandidate(
        declarationFactsOf(definition));
}

bool isTileLocalAttachmentCandidate(
    const RenderTargetDefinition &definition) {
    return Pelican::isTileLocalAttachmentCandidate(
        declarationFactsOf(definition));
}

ImageUsage runtimeAliasSafeUsage() {
    return ImageUsageBit::color_attachment |
           ImageUsageBit::sampled;
}

// The declaration's eligibility, and after physical planning the target is
// still materialized.
bool isRuntimeAliasCandidate(
    const RenderTargetDefinition &definition) {
    return isRuntimeAliasEligible(
               declarationFactsOf(definition)) &&
           definition.storage_mode ==
               RenderTargetStorageMode::materialized;
}

std::string joinNames(const std::vector<std::string> &names) {
    std::string result;
    for (const auto &name : names) {
        if (!result.empty()) result += ", ";
        result += name;
    }
    return result;
}

// PO-1: what kept a target out of isRuntimeAliasCandidate, in words the
// plan's reason carries (empty for a candidate). It names the checks of that
// rule one by one and decides nothing itself.
std::string runtimeAliasExclusion(
    const RenderTargetDefinition &definition) {
    if (isRuntimeAliasCandidate(definition)) return {};
    std::vector<std::string> causes;
    if (definition.history) {
        causes.push_back(
            "history target keeps its contents for the next frame");
    }
    if (definition.source) {
        causes.push_back(
            "image read from a file keeps its contents");
    }
    // PS-1: what a once pass wrote stays for the frames it does not run.
    if (definition.persistent) {
        causes.push_back("persistent lifetime");
    }
    if (definition.format_class == "display") {
        causes.push_back("display-format target is not aliased");
    }
    if (definition.storage_mode !=
        RenderTargetStorageMode::materialized) {
        causes.push_back("storage mode is not materialized");
    }
    const auto safe = runtimeAliasSafeUsage();
    const auto safe_set = "{" + joinNames(imageUsageNames(safe)) + "}";
    if (const auto extra = definition.usage & ~safe) {
        const auto names = imageUsageNames(extra);
        causes.push_back("usage " + joinNames(names) +
                         (names.size() == 1 ? " is" : " are") +
                         " outside the alias-safe set " + safe_set);
    }
    if (const auto missing = safe & ~definition.usage) {
        causes.push_back("usage lacks " +
                         joinNames(imageUsageNames(missing)) +
                         " of the alias-safe set " + safe_set);
    }
    std::string result;
    for (const auto &cause : causes) {
        if (!result.empty()) result += "; ";
        result += cause;
    }
    return result;
}

// PS-1: a frame that skips a once node records nothing for it, so the node
// must not share a physical scope (begun, cleared, fused) with a node that
// runs. Scope-owning implementations must likewise begin and end their own
// scope. Both are isolated on top of authored target_planning constraints.
std::vector<PlanningNodeConstraint> isolatedNodeConstraints(
    const FrameGraphDefinition &definition, const GraphIR &graph,
    std::vector<PlanningNodeConstraint> constraints, std::vector<PlanningPolicyEntry> &policy) {
    for (const auto &node : definition.nodes) {
        const auto typed = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const auto &entry) { return entry.name == node.name; });
        const bool owns_scope = typed != graph.nodes.end() &&
            (typed->shape == RenderNodeShape::sprite || typed->shape == RenderNodeShape::ui ||
             typed->shape == RenderNodeShape::imgui);
        if (!node.once && !owns_scope) continue;
        const auto found = std::find_if(
            constraints.begin(), constraints.end(),
            [&](const PlanningNodeConstraint &constraint) {
                return constraint.node == node.name;
            });
        for (auto &e : policy) {
            const auto *value = std::get_if<bool>(&e.value);
            if (e.target_kind != PlanningTargetKind::node || e.target != node.name || e.key != "isolate" ||
                e.status != PlanningPolicyStatus::adopted || !value || *value) continue;
            if (e.strength == PlanningStrength::require)
                throw GraphRegistryError("target_planning.policy_conflicts_engine@1",
                    {{"graph", definition.name}, {"target", node.name}, {"key", e.key},
                     {"origin", {{"document", e.origin.document}, {"pointer", e.origin.pointer}}}});
            e.status = PlanningPolicyStatus::engine_forced;
        }
        const PlanningDeclaration<bool> forced{true, PlanningStrength::require,
            {PlanningOriginKind::engine, "engine", "/isolate"}};
        if (found != constraints.end()) {
            found->isolate = forced;
        } else {
            constraints.push_back(PlanningNodeConstraint{
                .node = node.name, .isolate = forced});
        }
    }
    return constraints;
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

// RC-3c2a: what the device is asked of a candidate is the engine's usage
// rule (engineImageUsage) for the storage, local read or export asked about.
ImageUsage queriedImageUsage(
    const RenderTargetDefinition &definition,
    ImageUsageFacts facts = {}) {
    facts.declared = definition.usage;
    facts.history = definition.history;
    return engineImageUsage(facts);
}

vk::ImageCreateFlags requiredImageCreateFlags(
    const RenderTargetDefinition &definition) {
    return definition.dimension ==
                   ImageResourceDimension::cube
               ? vk::ImageCreateFlags{
                     vk::ImageCreateFlagBits::
                         eCubeCompatible}
               : vk::ImageCreateFlags{};
}

RenderingImageFormatCapability
queryImageFormatCapability(
    vk::PhysicalDevice physical_device,
    const RenderTargetDefinition &definition) {
    RenderingImageFormatCapability result;
    try {
        const auto properties =
            physical_device.getImageFormatProperties(
                toVulkanFormat(definition.format),
                vk::ImageType::e2D,
                vk::ImageTiling::eOptimal,
                toVulkanImageUsage(
                    queriedImageUsage(definition)),
                requiredImageCreateFlags(
                    definition));
        result.image_usage_supported = true;
        result.supported_samples =
            sampleCounts(properties.sampleCounts);
        result.max_mip_levels =
            properties.maxMipLevels;
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
                toVulkanFormat(definition.format),
                vk::ImageType::e2D,
                vk::ImageTiling::eOptimal,
                toVulkanImageUsage(
                    queriedImageUsage(
                        definition,
                        {.external_depth_source = true})),
                requiredImageCreateFlags(
                    definition));
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
                toVulkanFormat(definition.format),
                    vk::ImageType::e2D,
                    vk::ImageTiling::eOptimal,
                    toVulkanImageUsage(
                    queriedImageUsage(
                        definition,
                        {.storage = RenderTargetStorageMode::
                             transient_attachment})),
                    requiredImageCreateFlags(
                        definition));
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
                toVulkanFormat(definition.format),
                    vk::ImageType::e2D,
                    vk::ImageTiling::eOptimal,
                    toVulkanImageUsage(
                    queriedImageUsage(
                        definition,
                        {.storage = RenderTargetStorageMode::
                             tile_local_attachment})),
                    requiredImageCreateFlags(
                        definition));
            (void)physical_device
                .getImageFormatProperties(
                toVulkanFormat(definition.format),
                    vk::ImageType::e2D,
                    vk::ImageTiling::eOptimal,
                    toVulkanImageUsage(
                    queriedImageUsage(
                        definition,
                        {.local_read = true})),
                    requiredImageCreateFlags(
                        definition));
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

TargetEndpoint makeRuntimeEndpoint(
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
    if (facts.ray_query) {
        capabilities.push_back(
            std::string{vulkanRayQueryCapability});
    }
    if (facts.ray_tracing_pipeline) {
        capabilities.push_back(
            std::string{
                vulkanRayTracingPipelineCapability});
    }
    if (facts.ray_material_bindless) {
        capabilities.push_back(
            std::string{vulkanRayMaterialBindlessCapability});
    }
    return TargetEndpoint{
        .id = "device:0",
        .kind = TargetEndpointKind::vulkan_device,
        .capabilities = std::move(capabilities),
        .facts = std::move(target_facts),
    };
}

TargetTopologySnapshot runtimeTopology(
    const RenderingTargetPlanDeviceFacts &facts,
    bool enable_tile_local) {
    return TargetTopologySnapshot{
        .name = "runtime_vulkan_device",
        .endpoints = {
            makeRuntimeEndpoint(facts, enable_tile_local)},
    };
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

ResourcePattern runtimeImagePattern(
    const LogicalTypeRegistry &types,
    const LogicalType &type, std::optional<ImageFormat> format,
    std::span<const ImageFormat> authored_candidates,
    bool external,
    bool transient_attachment,
    bool tile_local_attachment,
    bool allow_alias) {
    auto candidates =
        std::vector<ResourceFormatCandidate>{};
    const auto append = [&](ImageFormat candidate) {
        if (std::none_of(
                candidates.begin(), candidates.end(),
                [&](const auto &existing) {
                    return existing.format == candidate;
                })) {
            candidates.push_back(
                ResourceFormatCandidate{
                    .format = candidate,
                });
        }
    };
    if (format) append(*format);
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
        .allow_alias = allow_alias,
        .require_store =
            !transient_attachment &&
            !tile_local_attachment &&
            !allow_alias,
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
                    types, resource.type,
                    imageFormatFromVulkan(swapchain_format),
                    {}, true, false, false, false),
                ResourceExtentPlan{},
                ImageMipLevelCount{},
                1,
                ImageResourceDimension::two_d,
            });
            continue;
        }
        const auto target = render_targets.find(resource.name);
        if (target == render_targets.end()) {
            throw std::runtime_error(
                "runtime logical image has no RenderTargetDefinition: " +
                resource.name);
        }
        const auto transient =
            transient_formats.contains({
                resource.name,
                target->second->format}) &&
            target->second->mip_levels ==
                ImageMipLevelCount{};
        const auto tile_local =
            tile_local_formats.contains({
                resource.name,
                target->second->format}) &&
            target->second->mip_levels ==
                ImageMipLevelCount{};
        auto pattern = runtimeImagePattern(
            types, resource.type,
            target->second->format,
            target->second->format_candidates,
            false,
            transient,
            tile_local,
            !transient && !tile_local &&
                isRuntimeAliasCandidate(
                    *target->second));
        pattern.store_reason =
            runtimeAliasExclusion(*target->second);
        result.push_back({
            resource.name,
            std::move(pattern),
            runtimeExtentPlan(*target->second),
            target->second->mip_levels,
            target->second->array_layers,
            target->second->dimension,
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
                    .subresource =
                        attachment.subresource,
                    .aspect = attachment.aspect,
                    .operations = attachment.operations,
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

std::vector<ImageFormat> declaredFormats(
    const RenderTargetDefinition &target) {
    auto result = std::vector<ImageFormat>{
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
    ImageFormat format) {
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
        .max_mip_levels =
            std::numeric_limits<
                std::uint32_t>::max(),
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
                    format);
            }
        }
    }
    return result;
}

// RC-3c1b: the device facts project's tile-local rule asks: an image of a
// format, usage and kind is supported as a local-read attachment when the
// device keeps attachments tile-local and supports both the image and the
// local read (the query this compilation was given).
RenderGraphDeviceFacts renderGraphDeviceFactsOf(
    const RenderingTargetPlanDeviceFacts &facts) {
    return RenderGraphDeviceFacts{
        .tile_local_attachments =
            facts.transient_attachments &&
            facts.dynamic_rendering_local_read,
        .supports_local_read_attachment =
            [&facts](const RenderGraphImageCapabilityQuery &query) {
                RenderTargetDefinition candidate;
                candidate.format = query.format;
                candidate.usage = query.usage;
                candidate.dimension =
                    query.cube_compatible
                        ? ImageResourceDimension::cube
                        : ImageResourceDimension::two_d;
                const auto capability = queryFormatCapability(
                    facts, candidate, query.format);
                return capability.image_usage_supported &&
                       capability.local_read_attachment_supported;
            },
    };
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
                .format = format,
                .image_usage_supported =
                    capability
                        .image_usage_supported,
                .supported_samples =
                    capability
                        .supported_samples,
                .max_mip_levels =
                    capability.max_mip_levels,
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
    ImageFormat format) {
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
            std::string{imageFormatName(format)});
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
                .format = imageFormatFromVulkan(swapchain_format),
                .supported_samples = {1},
            });
            continue;
        }
        const auto &target = *render_targets.at(resource);
        const auto &capability =
            requireFormatCapability(
                format_capabilities,
                resource,
                target.format);
        if (!capability.image_usage_supported) {
            throw std::runtime_error(
                "automatic render-target format does not support "
                "the required image usage: " +
                resource + " -> " +
                std::string{imageFormatName(target.format)});
        }
        result.push_back({
            .resource = resource,
            .format = target.format,
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
    std::set<std::string, std::less<>>
        aliased_resources;
    for (const auto &group : plan.alias_groups) {
        if (group.resources.size() < 2) {
            throw std::runtime_error(
                "runtime physical alias group requires at least two resources: " +
                group.id);
        }
        const VulkanPhysicalResourcePlan *contract =
            nullptr;
        for (const auto &name : group.resources) {
            if (!aliased_resources.insert(name).second) {
                throw std::runtime_error(
                    "runtime physical resource belongs to multiple alias groups: " +
                    name);
            }
            const auto &resource =
                requirePhysicalResource(plan, name);
            const auto target =
                render_targets.find(name);
            if (target == render_targets.end() ||
                !isRuntimeAliasCandidate(
                    *target->second) ||
                !resource.aliasable ||
                resource.representation !=
                    VulkanResourceRepresentation::
                        materialized_image ||
                resource.rasterization_samples != 1 ||
                resource.resolve_required) {
                throw std::runtime_error(
                    "runtime physical alias group contains an unsupported resource: " +
                    name);
            }
            if (contract == nullptr) {
                contract = &resource;
                continue;
            }
            if (resource.format != contract->format ||
                resource.view_layout !=
                    contract->view_layout ||
                resource.mip_levels !=
                    contract->mip_levels ||
                resource.array_layers !=
                    contract->array_layers ||
                resource.dimension !=
                    contract->dimension ||
                resource.extent != contract->extent) {
                throw std::runtime_error(
                    "runtime physical alias group members have incompatible image contracts: " +
                    group.id);
            }
        }
    }
    for (const auto &resource : plan.resources) {
        if (resource.logical_resource == "swapchain") {
            if (resource.representation !=
                    VulkanResourceRepresentation::external ||
                resource.format !=
                    imageFormatFromVulkan(swapchain_format) ||
                resource.rasterization_samples != 1 ||
                resource.dimension !=
                    ImageResourceDimension::two_d) {
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
                resource.rasterization_samples != 1 ||
                resource.mip_levels !=
                    ImageMipLevelCount{}) {
                throw std::runtime_error(
                    "runtime scope-local attachment must be "
                    "single-mip, single-sample, unresolved, and unstored: " +
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
                               attachment.operations.store_op !=
                                   AttachmentStoreOp::
                                       dont_care;
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
                    return format == resource.format;
                });
        if (selected_format == formats.end()) {
            throw std::runtime_error(
                "runtime physical format is not a declared "
                "candidate for '" +
                resource.logical_resource + "': planned '" +
                imageFormatNameOrEmpty(resource.format) + "'");
        }
        const auto planned_format = *selected_format;
        const auto planned_format_name =
            std::string{imageFormatName(planned_format)};
        if (transient &&
            !transient_formats.contains({
                resource.logical_resource,
                planned_format})) {
            throw std::runtime_error(
                "runtime transient attachment format is not "
                "supported by the target device: " +
                resource.logical_resource + " -> " +
                planned_format_name);
        }
        if (tile_local &&
            !tile_local_formats.contains({
                resource.logical_resource,
                planned_format})) {
            throw std::runtime_error(
                "runtime tile-local attachment format is not "
                "supported by the target device or pass contract: " +
                resource.logical_resource + " -> " +
                planned_format_name);
        }
        const auto &capability =
            requireFormatCapability(
                format_capabilities,
                resource.logical_resource,
                planned_format);
        if (!capability.image_usage_supported) {
            throw std::runtime_error(
                "runtime physical format does not support the "
                "required image usage for '" +
                resource.logical_resource + "': " +
                planned_format_name);
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
                planned_format_name);
        }
        if (resource.array_layers >
            capability.max_array_layers) {
            throw std::runtime_error(
                "runtime physical format does not support " +
                std::to_string(resource.array_layers) +
                " array layers for '" +
                resource.logical_resource + "': " +
                planned_format_name);
        }
        if (capability.max_mip_levels == 0 ||
            (resource.mip_levels.mode ==
                 ImageMipLevelMode::fixed &&
             resource.mip_levels.count >
                 capability.max_mip_levels)) {
            throw std::runtime_error(
                "runtime physical format does not support the "
                "authored mip-level contract for '" +
                resource.logical_resource + "': " +
                planned_format_name);
        }
        if (resource.mip_levels !=
            target->second->mip_levels) {
            throw std::runtime_error(
                "runtime physical mip-level contract mismatch for '" +
                resource.logical_resource + "'");
        }
        if (resource.array_layers <
            target->second->array_layers) {
            throw std::runtime_error(
                "runtime physical array-layer contract is smaller than "
                "the authored contract for '" +
                resource.logical_resource + "'");
        }
        if (resource.dimension !=
                target->second->dimension ||
            (resource.dimension ==
                 ImageResourceDimension::cube &&
             resource.array_layers != 6)) {
            throw std::runtime_error(
                "runtime physical image dimension contract "
                "mismatch for '" +
                resource.logical_resource + "'");
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
                planned_format_name);
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

std::vector<RenderingTargetAliasGroupAssignment>
mergeRuntimeAliasGroups(
    std::span<
        const std::shared_ptr<const VulkanTargetPlan>>
        plans) {
    using Group = std::vector<std::string>;
    std::set<Group> candidates;
    for (const auto &plan : plans) {
        if (plan == nullptr) continue;
        for (const auto &source : plan->alias_groups) {
            auto group = source.resources;
            std::sort(group.begin(), group.end());
            group.erase(
                std::unique(group.begin(), group.end()),
                group.end());
            if (group.size() >= 2) {
                candidates.insert(std::move(group));
            }
        }
    }

    std::vector<Group> compatible;
    for (const auto &candidate : candidates) {
        bool valid = true;
        bool observed = false;
        for (const auto &plan : plans) {
            if (plan == nullptr) continue;
            std::set<std::string, std::less<>>
                resources;
            for (const auto &resource : plan->resources) {
                resources.insert(
                    resource.logical_resource);
            }
            const auto any_member =
                std::any_of(
                    candidate.begin(), candidate.end(),
                    [&](const auto &member) {
                        return resources.contains(member);
                    });
            if (!any_member) continue;
            observed = true;
            if (!std::all_of(
                    candidate.begin(), candidate.end(),
                    [&](const auto &member) {
                        return resources.contains(member);
                    })) {
                valid = false;
                break;
            }

            bool exact_group = false;
            bool conflicting_group = false;
            for (const auto &source :
                 plan->alias_groups) {
                auto group = source.resources;
                std::sort(group.begin(), group.end());
                group.erase(
                    std::unique(
                        group.begin(), group.end()),
                    group.end());
                const auto intersects =
                    std::any_of(
                        group.begin(), group.end(),
                        [&](const auto &member) {
                            return std::binary_search(
                                candidate.begin(),
                                candidate.end(),
                                member);
                        });
                if (!intersects) continue;
                if (group == candidate) {
                    exact_group = true;
                } else {
                    conflicting_group = true;
                }
            }
            if (!exact_group || conflicting_group) {
                valid = false;
                break;
            }
        }
        if (valid && observed) {
            compatible.push_back(candidate);
        }
    }

    std::map<std::string, std::size_t, std::less<>>
        membership_count;
    for (const auto &group : compatible) {
        for (const auto &resource : group) {
            ++membership_count[resource];
        }
    }

    std::vector<RenderingTargetAliasGroupAssignment>
        result;
    for (const auto &group : compatible) {
        if (std::any_of(
                group.begin(), group.end(),
                [&](const auto &resource) {
                    return membership_count.at(resource) !=
                           1;
                })) {
            continue;
        }
        std::string id = "runtime-alias";
        for (const auto &resource : group) {
            id += "/" + resource;
        }
        result.push_back({
            .id = std::move(id),
            .resources = group,
        });
    }
    return result;
}

} // namespace

TargetEndpoint renderingTargetRuntimeEndpoint(
    const RenderingTargetPlanDeviceFacts &facts,
    bool enable_tile_local) {
    return makeRuntimeEndpoint(facts, enable_tile_local);
}

RenderingTargetPlanCompilation compileRenderingTargetPlans(
    const RenderGraphIR &render_graph,
    const std::unordered_map<std::string, FramePlan> &frame_plans,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    vk::Format swapchain_format,
    const RenderingTargetPlanDeviceFacts &device_facts,
    std::optional<VulkanViewExecutionPlanRequest>
        view_execution,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export,
    TargetPlanningPolicy target_planning) {
    const auto target_by_name =
        renderTargetsByName(render_targets);
    const auto types = makeBuiltinLogicalTypeRegistry();
    // The planner's view of each graph (RC-3c1b: derived from the IR; the
    // parts below that read it move to the IR with the physical plan).
    std::vector<FrameGraphDefinition> frame_graphs;
    frame_graphs.reserve(render_graph.graphs.size());
    for (const auto &graph : render_graph.graphs)
        frame_graphs.push_back(
            frameGraphDefinitionOf(render_graph, graph));
    const auto transient_formats =
        transientAttachmentFormats(
            render_targets, device_facts);
    const auto device = renderGraphDeviceFactsOf(device_facts);
    const auto device_tile_local_formats =
        renderGraphDeviceTileLocalFormats(render_graph, device);
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
        if (!graph_names.contains(constraints.graph) && constraints.nodes.empty() && constraints.resources.empty() &&
            constraints.required_capabilities.empty() && !constraints.declarations.empty() &&
            std::all_of(constraints.declarations.begin(), constraints.declarations.end(), [](const auto &e) { return e.status == PlanningPolicyStatus::absent; }))
            continue;
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
    result.verification_contexts.reserve(
        frame_graphs.size());
    std::map<std::string, std::uint32_t, std::less<>>
        merged_samples;
    std::map<std::string, std::uint32_t, std::less<>>
        merged_array_layers;
    std::map<std::string, ImageFormat, std::less<>>
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
            renderGraphTileLocalFormats(
                render_graph, render_graph.graphs[index],
                device_tile_local_formats);
        // Keep the tile backend out of candidate selection when this graph
        // has no resource that the current runtime can actually execute as a
        // local attachment. Otherwise an equivalent tile draft can win a
        // deterministic tie without providing any tile-local work.
        auto topology =
            runtimeTopology(
                device_facts,
                !tile_local_formats.empty());
        auto logical_graph = toLogicalGraph(
            render_graph, render_graph.graphs[index],
            ToLogicalGraphOptions{.device = device});
        auto format_capabilities =
            physicalFormatCapabilities(
                logical_graph, target_by_name,
                device_facts);
        const auto planning =
            planning_by_graph.find(definition.name);
        const auto *graph_constraints =
            planning == planning_by_graph.end()
                ? nullptr
                : planning->second;
        const auto frame = frame_plans.find(definition.name);
        if (frame == frame_plans.end())
            throw GraphRegistryError{"target_plan.node_order_missing@1", {{"graph", definition.name}}};
        const FramePlanNodeOrder node_order{frame->second};
        auto policy_entries = graph_constraints ? graph_constraints->declarations : std::vector<PlanningPolicyEntry>{};
        auto node_constraints = isolatedNodeConstraints(definition, *findGraph(render_graph, definition.name),
            graph_constraints ? graph_constraints->nodes : std::vector<PlanningNodeConstraint>{}, policy_entries);
        auto plan_value =
            compileVulkanTargetPlan(
                types, logical_graph, topology,
                runtimeProviders(),
                VulkanTargetPlanRequest{
                    .policy = std::move(policy_entries),
                    .node_order = node_order,
                    .endpoint = "device:0",
                    .provider = std::string{kRuntimeProvider},
                    .required_endpoint_capabilities =
                        graph_constraints == nullptr
                            ? std::vector<std::string>{}
                            : graph_constraints
                                  ->required_capabilities,
                    .pattern_bindings =
                        runtimePatternBindings(
                            types, logical_graph, target_by_name,
                            swapchain_format,
                            transient_formats,
                            tile_local_formats),
                    .profile = target_planning.profile,
                    .node_constraints = std::move(node_constraints),
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
                    .automatic_attachments =
                        physicalAttachmentPlans(
                            definition),
                });
        plan_value.resolution_plan =
            makeRuntimeResolutionPlan(
                definition, plan_value);
        for (const auto &node : findGraph(render_graph, definition.name)->nodes) {
            if (node.shape != RenderNodeShape::sprite) continue;
            const auto scope = std::find_if(plan_value.scopes.begin(), plan_value.scopes.end(),
                [&](const auto &s) { return std::find(s.nodes.begin(), s.nodes.end(), node.name) != s.nodes.end(); });
            for (const auto &use : node.uses)
                if ((use.role == ResourceUseRole::color_attachment ||
                     use.role == ResourceUseRole::depth_attachment) &&
                    use.subresource && scope != plan_value.scopes.end() &&
                    (use.subresource->layer_count != scope->view_count ||
                     (scope->rasterization_samples > 1 && use.subresource->base_mip_level != 0)))
                    throw GraphRegistryError("render_graph.sprite_attachment_range_unsupported@1",
                        {{"graph", definition.name}, {"node", node.name}, {"resource", use.resource}});
        }
        for (const auto &scope : plan_value.scopes) {
            if (scope.nodes.size() < 2) continue;
            for (const auto &name : scope.nodes) {
                const auto &nodes = findGraph(render_graph, definition.name)->nodes;
                const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto &entry) { return entry.name == name; });
                if (node != nodes.end() && (node->shape == RenderNodeShape::sprite ||
                    node->shape == RenderNodeShape::ui || node->shape == RenderNodeShape::imgui))
                    throw GraphRegistryError("render_graph.scope_implementation_unfusible@1", {{"graph", definition.name}, {"node", name}});
            }
        }
        auto plan =
            std::make_shared<const VulkanTargetPlan>(
                std::move(plan_value));
        const auto automatic_plan = plan;
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
                        return format == resource.format;
                    });
            if (selected_format ==
                formats.end()) {
                throw std::runtime_error(
                    "runtime physical format is not declared for "
                    "render target '" +
                    resource.logical_resource + "': " +
                    imageFormatNameOrEmpty(resource.format));
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
        result.verification_contexts.push_back(
            RenderingTargetPlanVerificationContext{
                .node_order = node_order,
                .logical_graph =
                    std::make_shared<
                        const CompiledLogicalRenderGraph>(
                        std::move(logical_graph)),
                .topology = std::move(topology),
                .automatic_plan =
                    std::move(automatic_plan),
                .compiled_plan = plan,
                .format_capabilities =
                    std::move(format_capabilities),
            });
        for (const auto &node : findGraph(render_graph, definition.name)->nodes)
            if (node.shape == RenderNodeShape::sprite || node.shape == RenderNodeShape::ui || node.shape == RenderNodeShape::imgui)
                result.verification_contexts.back().scope_owning_nodes.push_back(node.name);
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
                    ? target.array_layers
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
    result.alias_group_assignments =
        mergeRuntimeAliasGroups(result.plans);
    return result;
}

RenderingTargetPlanCompilation
compileRenderingTargetPlansForVulkanDevice(
    const RenderGraphIR &render_graph,
    const std::unordered_map<std::string, FramePlan> &frame_plans,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    vk::Format swapchain_format,
    vk::PhysicalDevice physical_device,
    const VulkanRuntimeCapabilities &runtime_capabilities,
    std::optional<VulkanViewExecutionPlanRequest>
        view_execution,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export,
    TargetPlanningPolicy target_planning) {
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
        render_graph, frame_plans, render_targets, policy, swapchain_format,
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
            .ray_query = runtime_capabilities.ray_query,
            .ray_tracing_pipeline =
                runtime_capabilities.ray_tracing_pipeline,
            .ray_material_bindless =
                runtime_capabilities.ray_material_bindless,
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
                                toVulkanFormat(
                                    definition.format))
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
        std::move(target_planning));
}

std::vector<ImagePlacement> imagePlacementsOf(
    std::span<const RenderTargetDefinition> render_targets,
    const RenderingTargetPlanCompilation &compilation) {
    std::map<std::string, std::uint32_t, std::less<>>
        assignments;
    std::map<std::string, std::uint32_t, std::less<>>
        array_layer_assignments;
    std::map<std::string, ImageFormat, std::less<>>
        format_assignments;
    std::map<std::string, VulkanResourceRepresentation,
             std::less<>>
        representation_assignments;
    std::map<std::string, std::string, std::less<>>
        alias_group_by_resource;
    std::set<std::string, std::less<>>
        alias_group_ids;
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
        if (!format_assignments
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
    for (const auto &assignment :
         compilation.alias_group_assignments) {
        if (assignment.id.empty() ||
            assignment.resources.size() < 2) {
            throw std::runtime_error(
                "physical alias group assignment is empty or has fewer than two members");
        }
        if (!alias_group_ids.insert(assignment.id).second) {
            throw std::runtime_error(
                "duplicate physical alias group assignment id: " +
                assignment.id);
        }
        std::set<std::string, std::less<>>
            unique_resources;
        for (const auto &resource :
             assignment.resources) {
            if (resource.empty() ||
                !unique_resources.insert(resource).second ||
                !alias_group_by_resource
                     .emplace(resource, assignment.id)
                     .second) {
                throw std::runtime_error(
                    "duplicate or invalid physical alias group member: " +
                    resource);
            }
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
    std::vector<ImagePlacement> placements;
    placements.reserve(render_targets.size());
    for (const auto &target : render_targets) {
        ImagePlacement placement{.resource = target.name};
        const auto found = assignments.find(target.name);
        if (found == assignments.end()) {
            throw std::runtime_error(
                "missing physical sample-count assignment: " +
                target.name);
        }
        placement.samples = found->second;
        const auto layers =
            array_layer_assignments.find(target.name);
        if (layers ==
            array_layer_assignments.end()) {
            throw std::runtime_error(
                "missing physical array-layer assignment: " +
                target.name);
        }
        placement.array_layers = layers->second;
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
        placement.format = format->second;
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
            placement.storage =
                RenderTargetStorageMode::materialized;
            break;
        case VulkanResourceRepresentation::
            transient_attachment:
            if (!isTransientAttachmentCandidate(target) ||
                placement.samples != 1) {
                throw std::runtime_error(
                    "physical transient attachment assignment is "
                    "incompatible with RenderTargetDefinition: " +
                    target.name);
            }
            placement.storage =
                RenderTargetStorageMode::
                    transient_attachment;
            break;
        case VulkanResourceRepresentation::
            tile_local_attachment:
            if (!isTileLocalAttachmentCandidate(
                    target) ||
                placement.samples != 1) {
                throw std::runtime_error(
                    "physical tile-local attachment assignment is "
                    "incompatible with RenderTargetDefinition: " +
                    target.name);
            }
            placement.storage =
                RenderTargetStorageMode::
                    tile_local_attachment;
            break;
        default:
            throw std::runtime_error(
                "physical representation assignment is not "
                "implemented by the render-target runtime: " +
                target.name);
        }
        placement.local_read =
            local_read_resources.contains(target.name);
        if (external_depth_sources.contains(
                target.name)) {
            if (!(target.usage &
                  ImageUsageBit::
                      depth_stencil_attachment)) {
                throw std::runtime_error(
                    "external depth export source is not a depth "
                    "attachment RenderTargetDefinition: " +
                    target.name);
            }
            placement.external_depth_source = true;
        }
        placements.push_back(std::move(placement));
    }

    std::map<std::string, std::size_t, std::less<>>
        index_by_name;
    for (std::size_t index = 0; index < render_targets.size(); ++index) {
        index_by_name.emplace(render_targets[index].name, index);
    }
    std::map<std::string,
             std::vector<std::size_t>,
             std::less<>>
        alias_groups;
    for (const auto &[resource, group] :
         alias_group_by_resource) {
        const auto target =
            index_by_name.find(resource);
        if (target == index_by_name.end()) {
            throw std::runtime_error(
                "physical alias group references an unknown render target: " +
                resource);
        }
        alias_groups[group].push_back(target->second);
    }
    for (auto &[group, members] : alias_groups) {
        if (members.size() < 2) {
            throw std::runtime_error(
                "physical alias group requires at least two render targets: " +
                group);
        }
        // An alias candidate keeps its declared usage (the alias-safe set):
        // no local read and no export adds a bit to it.
        const auto &contract = render_targets[members.front()];
        const auto &contract_placement = placements[members.front()];
        for (const auto index : members) {
            const auto &target = render_targets[index];
            const auto &placement = placements[index];
            if (!isRuntimeAliasEligible(declarationFactsOf(target)) ||
                placement.storage != RenderTargetStorageMode::materialized ||
                placement.local_read || placement.external_depth_source ||
                placement.format != contract_placement.format ||
                target.usage != contract.usage ||
                target.extent_scale !=
                    contract.extent_scale ||
                target.fixed_extent !=
                    contract.fixed_extent ||
                placement.samples != contract_placement.samples ||
                target.mip_levels !=
                    contract.mip_levels ||
                placement.array_layers != contract_placement.array_layers ||
                target.dimension !=
                    contract.dimension) {
                throw std::runtime_error(
                    "physical alias group members have incompatible runtime image contracts: " +
                    group);
            }
            placements[index].alias_group = group;
        }
    }
    return placements;
}

vk::ResolveModeFlagBits colorAttachmentResolveMode(
    ImageFormat format) {
    // An integer color format resolves one sample; any other averages.
    return imageFormatShaderNumeric(format) !=
                   ImageShaderNumeric::floating
               ? vk::ResolveModeFlagBits::eSampleZero
               : vk::ResolveModeFlagBits::eAverage;
}

} // namespace Pelican
