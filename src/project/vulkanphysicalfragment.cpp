#include "targetrenderplanning.hpp"

#include "imagesubresourcejson.hpp"
#include "stablefingerprint.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

namespace Pelican {

std::string_view vulkanPhysicalScopeEditModeName(
    VulkanPhysicalScopeEditMode mode) {
    switch (mode) {
    case VulkanPhysicalScopeEditMode::split_only:
        return "split_only";
    case VulkanPhysicalScopeEditMode::dependency_safe:
        return "dependency_safe";
    }
    throw std::runtime_error(
        "unknown Vulkan physical scope edit mode");
}

namespace {

constexpr std::string_view kFragmentSchema =
    "pelican.vulkan_physical_fragment";
constexpr std::uint32_t kFragmentVersionV1 = 1;
constexpr std::uint32_t kFragmentVersionV2 = 2;
constexpr std::uint32_t kFragmentVersionV3 = 3;
constexpr std::string_view kSampledImageCapability =
    "pelican.vulkan.sampled_image@1";
constexpr std::string_view kStorageBufferCapability =
    "pelican.vulkan.storage_buffer@1";
constexpr std::string_view kTransferCopyCapability =
    "pelican.vulkan.transfer_copy@1";
constexpr std::string_view kTileBasedCapability =
    "pelican.vulkan.tile_based@1";
constexpr std::string_view kLocalReadCapability =
    "pelican.vulkan.dynamic_rendering_local_read@1";
constexpr std::string_view kTransientAttachmentCapability =
    "pelican.vulkan.transient_attachment@1";

void requireNonEmpty(
    std::string_view value, std::string_view subject) {
    if (value.empty()) {
        throw std::runtime_error(
            std::string{subject} + " must not be empty");
    }
}

void requireVersionedName(
    std::string_view value, std::string_view subject) {
    requireNonEmpty(value, subject);
    try {
        const auto parsed = parseSemanticTypeId(value);
        if (semanticTypeIdName(parsed) != value) {
            throw std::runtime_error("name is not canonical");
        }
    } catch (const std::runtime_error &error) {
        throw std::runtime_error(
            std::string{subject} +
            " must use namespace.name@major: " +
            std::string{value} + " (" + error.what() + ")");
    }
}

void requireOnlyKeys(
    const nlohmann::json &object,
    std::initializer_list<std::string_view> allowed,
    std::string_view context) {
    for (auto field = object.begin();
         field != object.end(); ++field) {
        if (std::find(
                allowed.begin(), allowed.end(),
                field.key()) == allowed.end()) {
            throw std::runtime_error(
                std::string{context} +
                " has unknown key '" + field.key() + "'");
        }
    }
}

std::string requireJsonString(
    const nlohmann::json &object,
    std::string_view key, std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            std::string{context} +
            " requires non-empty string " +
            std::string{key});
    }
    return found->get<std::string>();
}

std::uint64_t requireFingerprint(
    const nlohmann::json &object,
    std::string_view key, std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        throw std::runtime_error(
            std::string{context} +
            " requires fingerprint string " +
            std::string{key});
    }
    return parseStableFingerprint64(
        found->get_ref<const std::string &>(),
        std::string{context} + " " + std::string{key});
}

VulkanResourceRepresentation parseRepresentation(
    std::string_view value) {
    if (value == "materialized_image") {
        return VulkanResourceRepresentation::materialized_image;
    }
    if (value == "materialized_buffer") {
        return VulkanResourceRepresentation::materialized_buffer;
    }
    if (value == "transient_attachment") {
        return VulkanResourceRepresentation::transient_attachment;
    }
    if (value == "tile_local_attachment") {
        return VulkanResourceRepresentation::tile_local_attachment;
    }
    if (value == "external") {
        return VulkanResourceRepresentation::external;
    }
    throw std::runtime_error(
        "Vulkan physical resource fragment has unknown "
        "representation: " +
        std::string{value});
}

VulkanPhysicalAttachmentLoadOp parseAttachmentLoadOp(
    std::string_view value) {
    if (value == "load") {
        return VulkanPhysicalAttachmentLoadOp::load;
    }
    if (value == "clear") {
        return VulkanPhysicalAttachmentLoadOp::clear;
    }
    if (value == "discard") {
        return VulkanPhysicalAttachmentLoadOp::discard;
    }
    throw std::runtime_error(
        "Vulkan physical attachment fragment has unknown load_op: " +
        std::string{value});
}

VulkanPhysicalAttachmentStoreOp parseAttachmentStoreOp(
    std::string_view value) {
    if (value == "store") {
        return VulkanPhysicalAttachmentStoreOp::store;
    }
    if (value == "discard") {
        return VulkanPhysicalAttachmentStoreOp::discard;
    }
    throw std::runtime_error(
        "Vulkan physical attachment fragment has unknown store_op: " +
        std::string{value});
}

VulkanPhysicalScopeEditMode parseScopeEditMode(
    std::string_view value) {
    if (value == "split_only") {
        return VulkanPhysicalScopeEditMode::split_only;
    }
    if (value == "dependency_safe") {
        return VulkanPhysicalScopeEditMode::dependency_safe;
    }
    throw std::runtime_error(
        "Vulkan physical fragment has unknown scope_edit_mode: " +
        std::string{value});
}

template <typename Range>
void requireNoDuplicateStrings(
    const Range &values, std::string_view context) {
    std::set<std::string, std::less<>> unique;
    for (const auto &value : values) {
        requireNonEmpty(value, context);
        if (!unique.insert(value).second) {
            throw std::runtime_error(
                std::string{context} +
                " contains duplicate value: " + value);
        }
    }
}

void sortAndUniqueCapabilities(
    std::vector<std::string> &capabilities,
    std::string_view context) {
    for (const auto &capability : capabilities) {
        requireVersionedName(capability, context);
    }
    std::sort(capabilities.begin(), capabilities.end());
    capabilities.erase(
        std::unique(
            capabilities.begin(), capabilities.end()),
        capabilities.end());
}

VulkanPhysicalFragmentPackage canonicalizePackage(
    VulkanPhysicalFragmentPackage package) {
    if (package.schema_version != kFragmentVersionV1 &&
        package.schema_version != kFragmentVersionV2 &&
        package.schema_version != kFragmentVersionV3) {
        throw std::runtime_error(
            "Vulkan physical fragment package version must be 1, 2, or 3");
    }
    if (package.schema_version < kFragmentVersionV3 &&
        package.scope_edit_mode !=
            VulkanPhysicalScopeEditMode::split_only) {
        throw std::runtime_error(
            "Vulkan physical fragment dependency-safe scope edits "
            "require package version 3");
    }
    requireNonEmpty(package.graph,
                    "Vulkan physical fragment graph");
    requireVersionedName(
        package.backend_candidate,
        "Vulkan physical fragment backend candidate");

    for (const auto &resource : package.resources) {
        requireNonEmpty(
            resource.logical_resource,
            "Vulkan physical resource fragment name");
        if (!resource.format && !resource.representation) {
            throw std::runtime_error(
                "Vulkan physical resource fragment must override "
                "format or representation: " +
                resource.logical_resource);
        }
        if (resource.format && resource.format->empty()) {
            throw std::runtime_error(
                "Vulkan physical resource fragment format must not "
                "be empty: " +
                resource.logical_resource);
        }
    }
    std::sort(
        package.resources.begin(), package.resources.end(),
        [](const auto &left, const auto &right) {
            return left.logical_resource <
                   right.logical_resource;
        });
    if (std::adjacent_find(
            package.resources.begin(),
            package.resources.end(),
            [](const auto &left, const auto &right) {
                return left.logical_resource ==
                       right.logical_resource;
            }) != package.resources.end()) {
        throw std::runtime_error(
            "Vulkan physical fragment has duplicate resource "
            "overrides");
    }

    if (package.scopes) {
        std::set<std::string, std::less<>> ids;
        for (const auto &scope : *package.scopes) {
            requireNonEmpty(
                scope.id,
                "Vulkan physical scope fragment id");
            if (!ids.insert(scope.id).second) {
                throw std::runtime_error(
                    "Vulkan physical fragment has duplicate scope "
                    "id: " +
                    scope.id);
            }
            if (scope.nodes.empty()) {
                throw std::runtime_error(
                    "Vulkan physical scope fragment has no nodes: " +
                    scope.id);
            }
            requireNoDuplicateStrings(
                scope.nodes,
                "Vulkan physical scope fragment nodes");
        }
    }

    if (package.alias_groups) {
        for (auto &group : *package.alias_groups) {
            requireNonEmpty(
                group.id,
                "Vulkan physical alias fragment id");
            if (group.resources.size() < 2) {
                throw std::runtime_error(
                    "Vulkan physical alias fragment requires at "
                    "least two resources: " +
                    group.id);
            }
            requireNoDuplicateStrings(
                group.resources,
                "Vulkan physical alias fragment resources");
            std::sort(
                group.resources.begin(),
                group.resources.end());
        }
        std::sort(
            package.alias_groups->begin(),
            package.alias_groups->end(),
            [](const auto &left, const auto &right) {
                return left.id < right.id;
            });
        if (std::adjacent_find(
                package.alias_groups->begin(),
                package.alias_groups->end(),
                [](const auto &left, const auto &right) {
                    return left.id == right.id;
                }) != package.alias_groups->end()) {
            throw std::runtime_error(
                "Vulkan physical fragment has duplicate alias "
                "group ids");
        }
    }

    if (package.attachments) {
        if (package.schema_version < kFragmentVersionV2) {
            throw std::runtime_error(
                "Vulkan physical attachment fragments require package "
                "version 2 or newer");
        }
        for (const auto &attachment : *package.attachments) {
            requireNonEmpty(
                attachment.node,
                "Vulkan physical attachment fragment node");
            requireNonEmpty(
                attachment.logical_resource,
                "Vulkan physical attachment fragment resource");
            if (!attachment.load_op &&
                !attachment.store_op) {
                throw std::runtime_error(
                    "Vulkan physical attachment fragment must override "
                    "load_op or store_op: " +
                    attachment.node + " -> " +
                    attachment.logical_resource);
            }
        }
        std::sort(
            package.attachments->begin(),
            package.attachments->end(),
            [](const auto &left, const auto &right) {
                return std::tie(
                           left.node,
                           left.logical_resource) <
                       std::tie(
                           right.node,
                           right.logical_resource);
            });
        if (std::adjacent_find(
                package.attachments->begin(),
                package.attachments->end(),
                [](const auto &left, const auto &right) {
                    return left.node == right.node &&
                           left.logical_resource ==
                               right.logical_resource;
                }) != package.attachments->end()) {
            throw std::runtime_error(
                "Vulkan physical fragment has duplicate attachment "
                "overrides");
        }
    }

    if (package.resources.empty() &&
        !package.scopes && !package.alias_groups &&
        !package.attachments) {
        throw std::runtime_error(
            "Vulkan physical fragment package has no edits");
    }
    return package;
}

nlohmann::ordered_json extentToJson(
    const std::optional<ResourceExtentPlan> &extent) {
    if (!extent) return nullptr;
    return {
        {"kind", resourceExtentKindName(extent->kind)},
        {"scale_x", extent->scale_x},
        {"scale_y", extent->scale_y},
        {"width", extent->width},
        {"height", extent->height},
    };
}

nlohmann::ordered_json physicalResourceToJson(
    const VulkanPhysicalResourcePlan &resource) {
    nlohmann::ordered_json lifetime{
        {"used", resource.lifetime.used},
    };
    if (resource.lifetime.used) {
        lifetime["first_use"] =
            resource.lifetime.first_use;
        lifetime["last_use"] =
            resource.lifetime.last_use;
    }
    return {
        {"logical_resource", resource.logical_resource},
        {"pattern", resource.pattern},
        {"format", resource.format},
        {"representation",
         vulkanResourceRepresentationName(
             resource.representation)},
        {"widest_read",
         logicalReadFootprintKindName(
             resource.widest_read)},
        {"lifetime", std::move(lifetime)},
        {"stored", resource.stored},
        {"aliasable", resource.aliasable},
        {"required_physical_features",
         resource.required_physical_features},
        {"reason", resource.reason},
        {"rasterization_samples",
         resource.rasterization_samples},
        {"resolve_required",
         resource.resolve_required},
        {"view_layout",
         vulkanResourceViewLayoutName(
             resource.view_layout)},
        {"mip_levels",
         nlohmann::ordered_json{
             {"mode",
              imageMipLevelModeName(
                  resource.mip_levels.mode)},
             {"count",
              resource.mip_levels.count},
         }},
        {"array_layers", resource.array_layers},
        {"extent", extentToJson(resource.extent)},
    };
}

nlohmann::ordered_json physicalScopeToJson(
    const VulkanPhysicalScopePlan &scope) {
    return {
        {"id", scope.id},
        {"kind", vulkanPhysicalScopeKindName(scope.kind)},
        {"nodes", scope.nodes},
        {"single_rendering_instance",
         scope.single_rendering_instance},
        {"local_reads", scope.local_reads},
        {"region_tags", scope.region_tags},
        {"rasterization_samples",
         scope.rasterization_samples},
        {"view_execution",
         vulkanScopeViewExecutionName(
             scope.view_execution)},
        {"view_count", scope.view_count},
        {"execution_count", scope.execution_count},
        {"view_mask", scope.view_mask},
    };
}

nlohmann::ordered_json physicalAttachmentToJson(
    const VulkanPhysicalAttachmentPlan &attachment) {
    nlohmann::ordered_json result{
        {"node", attachment.node},
        {"logical_resource",
         attachment.logical_resource},
        {"aspect",
         vulkanPhysicalAttachmentAspectName(
             attachment.aspect)},
        {"load_op",
         vulkanPhysicalAttachmentLoadOpName(
             attachment.load_op)},
        {"store_op",
         vulkanPhysicalAttachmentStoreOpName(
             attachment.store_op)},
    };
    if (attachment.subresource) {
        result["subresource"] =
            imageSubresourceToJson(
                *attachment.subresource);
    }
    return result;
}

nlohmann::ordered_json automaticPlanPayload(
    const TargetTopologySnapshot &topology,
    const VulkanTargetPlan &plan) {
    auto selection =
        backendSelectionToJson(plan.backend_selection);
    selection.erase("decisions");

    nlohmann::ordered_json payload{
        {"schema",
         "pelican.vulkan_automatic_target_plan_fingerprint"},
        {"version", 1},
        {"topology",
         targetTopologySnapshotToJson(
             canonicalizeTargetTopology(topology))},
        {"graph", plan.graph},
        {"logical_graph_fingerprint",
         stableFingerprint64String(
             plan.logical_graph_fingerprint)},
        {"backend_selection", std::move(selection)},
        {"lowering_graph",
         targetLoweringGraphToJson(plan.lowering_graph)},
        {"required_physical_features",
         plan.required_physical_features},
        {"resources", nlohmann::ordered_json::array()},
        {"scopes", nlohmann::ordered_json::array()},
        {"alias_groups", nlohmann::ordered_json::array()},
        {"view_execution",
         nlohmann::ordered_json{
             {"view_count",
              plan.view_execution_plan.view_count},
             {"requested",
              xrViewExecutionPreferenceName(
                  plan.view_execution_plan.requested)},
             {"endpoint_supports_multiview",
              plan.view_execution_plan
                  .endpoint_supports_multiview},
             {"max_multiview_view_count",
              plan.view_execution_plan
                  .max_multiview_view_count},
             {"uses_multiview",
              plan.view_execution_plan.uses_multiview},
             {"mixed_execution",
              plan.view_execution_plan.mixed_execution},
             {"automatic_policy",
              resolvedXrMultiviewAutoPolicyToJson(
                  plan.view_execution_plan
                      .automatic_policy)},
             {"reason",
              plan.view_execution_plan.reason},
         }},
    };
    for (const auto &resource : plan.resources) {
        payload["resources"].push_back(
            physicalResourceToJson(resource));
    }
    for (const auto &scope : plan.scopes) {
        payload["scopes"].push_back(
            physicalScopeToJson(scope));
    }
    if (!plan.attachments.empty()) {
        payload["attachments"] =
            nlohmann::ordered_json::array();
        for (const auto &attachment :
             plan.attachments) {
            payload["attachments"].push_back(
                physicalAttachmentToJson(
                    attachment));
        }
    }
    for (const auto &group : plan.alias_groups) {
        payload["alias_groups"].push_back(
            nlohmann::ordered_json{
                {"id", group.id},
                {"resources", group.resources},
            });
    }
    if (plan.sample_count_plan) {
        payload["sample_count_plan"] =
            resolvedSampleCountPlanToJson(
                *plan.sample_count_plan);
    }
    if (plan.external_depth_export) {
        payload["external_depth_export"] =
            nlohmann::ordered_json{
                {"source_resource",
                 plan.external_depth_export
                     ->source_resource},
                {"format",
                 plan.external_depth_export->format},
                {"view_layout",
                 vulkanResourceViewLayoutName(
                     plan.external_depth_export
                         ->view_layout)},
                {"array_layers",
                 plan.external_depth_export
                     ->array_layers},
            };
    }
    return payload;
}

const BackendProbeResult &selectedProbe(
    const VulkanTargetPlan &plan) {
    const auto found = std::find_if(
        plan.backend_selection.candidates.begin(),
        plan.backend_selection.candidates.end(),
        [&](const BackendProbeResult &candidate) {
            return candidate.candidate ==
                   plan.backend_selection.selected_candidate;
        });
    if (found == plan.backend_selection.candidates.end()) {
        throw std::runtime_error(
            "Vulkan physical fragment base plan has no selected "
            "probe");
    }
    return *found;
}

bool isMaterialized(
    VulkanResourceRepresentation representation) {
    return representation ==
               VulkanResourceRepresentation::
                   materialized_image ||
           representation ==
               VulkanResourceRepresentation::
                   materialized_buffer;
}

bool isConservativeRepresentationChange(
    VulkanResourceRepresentation automatic,
    VulkanResourceRepresentation requested) {
    if (automatic == requested) return true;
    return requested ==
               VulkanResourceRepresentation::
                   materialized_image &&
           (automatic ==
                VulkanResourceRepresentation::
                    transient_attachment ||
            automatic ==
                VulkanResourceRepresentation::
                    tile_local_attachment);
}

const ResourceFormatCandidate &requireFormatCandidate(
    const TargetLoweringResource &lowering,
    std::string_view format) {
    const auto found = std::find_if(
        lowering.pattern.format_candidates.begin(),
        lowering.pattern.format_candidates.end(),
        [&](const ResourceFormatCandidate &candidate) {
            return candidate.format == format;
        });
    if (found ==
        lowering.pattern.format_candidates.end()) {
        throw std::runtime_error(
            "Vulkan physical fragment format is not declared by "
            "ResourcePattern '" +
            lowering.pattern.id + "' for resource '" +
            lowering.logical.name + "': " +
            std::string{format});
    }
    return *found;
}

using FormatCapabilityKey =
    std::pair<std::string, std::string>;

std::map<
    FormatCapabilityKey,
    const VulkanPhysicalResourceFormatCapability *>
indexFormatCapabilities(
    std::span<
        const VulkanPhysicalResourceFormatCapability>
        capabilities) {
    std::map<
        FormatCapabilityKey,
        const VulkanPhysicalResourceFormatCapability *>
        result;
    for (const auto &capability : capabilities) {
        requireNonEmpty(
            capability.logical_resource,
            "Vulkan physical format capability resource");
        requireNonEmpty(
            capability.format,
            "Vulkan physical format capability format");
        if (capability.max_mip_levels == 0) {
            throw std::runtime_error(
                "Vulkan physical format capability max mip "
                "levels must be positive: " +
                capability.logical_resource + " -> " +
                capability.format);
        }
        if (capability.max_array_layers == 0) {
            throw std::runtime_error(
                "Vulkan physical format capability max array "
                "layers must be positive: " +
                capability.logical_resource + " -> " +
                capability.format);
        }
        std::set<std::uint32_t> sample_counts;
        for (const auto samples :
             capability.supported_samples) {
            if (samples == 0 || samples > 64 ||
                (samples & (samples - 1)) != 0) {
                throw std::runtime_error(
                    "Vulkan physical format capability has an "
                    "invalid sample count: " +
                    capability.logical_resource + " -> " +
                    capability.format + " -> " +
                    std::to_string(samples));
            }
            if (!sample_counts.insert(samples).second) {
                throw std::runtime_error(
                    "Vulkan physical format capability has a "
                    "duplicate sample count: " +
                    capability.logical_resource + " -> " +
                    capability.format + " -> " +
                    std::to_string(samples));
            }
        }
        const auto key = FormatCapabilityKey{
            capability.logical_resource,
            capability.format,
        };
        if (!result.emplace(key, &capability).second) {
            throw std::runtime_error(
                "Vulkan physical format capabilities contain a "
                "duplicate resource/format pair: " +
                capability.logical_resource + " -> " +
                capability.format);
        }
    }
    return result;
}

const VulkanPhysicalResourceFormatCapability &
requireAlternateFormatCapability(
    const std::map<
        FormatCapabilityKey,
        const VulkanPhysicalResourceFormatCapability *>
        &capabilities,
    const VulkanPhysicalResourcePlan &resource,
    std::string_view format,
    bool exports_depth) {
    const auto found = capabilities.find(
        FormatCapabilityKey{
            resource.logical_resource,
            std::string{format},
        });
    if (found == capabilities.end()) {
        throw std::runtime_error(
            "Vulkan physical fragment has no target format "
            "capability for alternate format: " +
            resource.logical_resource + " -> " +
            std::string{format});
    }
    const auto &capability = *found->second;
    if (!capability.image_usage_supported) {
        throw std::runtime_error(
            "Vulkan physical fragment alternate format does not "
            "support the required image usage: " +
            resource.logical_resource + " -> " +
            std::string{format});
    }
    if (std::find(
            capability.supported_samples.begin(),
            capability.supported_samples.end(),
            resource.rasterization_samples) ==
        capability.supported_samples.end()) {
        throw std::runtime_error(
            "Vulkan physical fragment alternate format does not "
            "support the selected rasterization sample count: " +
            resource.logical_resource + " -> " +
            std::string{format} + " -> " +
            std::to_string(
                resource.rasterization_samples) +
            " samples");
    }
    if (resource.array_layers >
        capability.max_array_layers) {
        throw std::runtime_error(
            "Vulkan physical fragment alternate format does not "
            "support the selected array layer count: " +
            resource.logical_resource + " -> " +
            std::string{format} + " -> " +
            std::to_string(resource.array_layers));
    }
    if (resource.mip_levels.mode ==
            ImageMipLevelMode::fixed &&
        resource.mip_levels.count >
            capability.max_mip_levels) {
        throw std::runtime_error(
            "Vulkan physical fragment alternate format does not "
            "support the selected mip-level count: " +
            resource.logical_resource + " -> " +
            std::string{format} + " -> " +
            std::to_string(
                resource.mip_levels.count));
    }
    if (exports_depth &&
        !capability.external_depth_export_supported) {
        throw std::runtime_error(
            "Vulkan physical fragment alternate depth format "
            "does not support the external transfer-source "
            "contract: " +
            resource.logical_resource + " -> " +
            std::string{format});
    }
    return capability;
}

std::vector<std::string> resourceFeatures(
    const TargetLoweringResource &lowering,
    const VulkanPhysicalResourcePlan &resource,
    bool exports_depth) {
    auto features =
        requireFormatCandidate(
            lowering, resource.format)
            .required_capabilities;
    switch (resource.representation) {
    case VulkanResourceRepresentation::materialized_image:
    case VulkanResourceRepresentation::external:
        features.push_back(
            std::string{kSampledImageCapability});
        break;
    case VulkanResourceRepresentation::materialized_buffer:
        features.push_back(
            std::string{kStorageBufferCapability});
        break;
    case VulkanResourceRepresentation::transient_attachment:
        features.push_back(
            std::string{kTransientAttachmentCapability});
        break;
    case VulkanResourceRepresentation::tile_local_attachment:
        features.insert(
            features.end(),
            {
                std::string{kTileBasedCapability},
                std::string{kLocalReadCapability},
                std::string{
                    kTransientAttachmentCapability},
            });
        break;
    }
    if (lowering.uses.produced_by_snapshot ||
        lowering.uses.transfer_access || exports_depth) {
        features.push_back(
            std::string{kTransferCopyCapability});
    }
    sortAndUniqueCapabilities(
        features,
        "Vulkan physical fragment resource feature");
    return features;
}

void validateRepresentation(
    const TargetLoweringResource &lowering,
    VulkanResourceRepresentation automatic,
    VulkanResourceRepresentation requested) {
    if (!isConservativeRepresentationChange(
            automatic, requested)) {
        throw std::runtime_error(
            "Vulkan physical fragment may only preserve the "
            "automatic representation or conservatively "
            "materialize an automatic transient/tile-local image: " +
            lowering.logical.name);
    }
    const auto constructor =
        lowering.logical.type.constructor;
    if (constructor == LogicalTypeConstructor::image &&
        requested ==
            VulkanResourceRepresentation::
                materialized_buffer) {
        throw std::runtime_error(
            "Vulkan physical fragment cannot represent an image as "
            "a buffer: " +
            lowering.logical.name);
    }
    if (constructor == LogicalTypeConstructor::buffer &&
        requested !=
            VulkanResourceRepresentation::
                materialized_buffer &&
        requested !=
            VulkanResourceRepresentation::external) {
        throw std::runtime_error(
            "Vulkan physical fragment cannot represent a buffer as "
            "an image attachment: " +
            lowering.logical.name);
    }
    if (lowering.logical.materialization ==
            LogicalMaterializationRequirement::external &&
        requested !=
            VulkanResourceRepresentation::external) {
        throw std::runtime_error(
            "Vulkan physical fragment cannot replace an external "
            "logical resource: " +
            lowering.logical.name);
    }
}

bool lifetimesDoNotOverlap(
    const TargetResourceLifetime &left,
    const TargetResourceLifetime &right) {
    return left.used && right.used &&
           (left.last_use < right.first_use ||
            right.last_use < left.first_use);
}

void validateAliasGroups(
    const VulkanTargetPlan &plan) {
    std::map<std::string,
             const VulkanPhysicalResourcePlan *,
             std::less<>>
        resources;
    for (const auto &resource : plan.resources) {
        resources.emplace(
            resource.logical_resource, &resource);
    }
    std::set<std::string, std::less<>> memberships;
    std::set<std::string, std::less<>> ids;
    for (const auto &group : plan.alias_groups) {
        requireNonEmpty(
            group.id, "Vulkan physical alias group id");
        if (!ids.insert(group.id).second) {
            throw std::runtime_error(
                "Vulkan physical fragment produced duplicate alias "
                "group id: " +
                group.id);
        }
        if (group.resources.size() < 2) {
            throw std::runtime_error(
                "Vulkan physical alias group requires at least two "
                "resources: " +
                group.id);
        }
        std::vector<const VulkanPhysicalResourcePlan *> members;
        for (const auto &name : group.resources) {
            const auto found = resources.find(name);
            if (found == resources.end()) {
                throw std::runtime_error(
                    "Vulkan physical alias group names an unknown "
                    "resource: " +
                    name);
            }
            if (!memberships.insert(name).second) {
                throw std::runtime_error(
                    "Vulkan physical resource belongs to more than "
                    "one alias group: " +
                    name);
            }
            if (!found->second->aliasable) {
                throw std::runtime_error(
                    "Vulkan physical alias group contains a "
                    "non-aliasable resource: " +
                    name);
            }
            members.push_back(found->second);
        }
        for (std::size_t left = 0; left < members.size();
             ++left) {
            for (std::size_t right = left + 1;
                 right < members.size(); ++right) {
                const auto &a = *members[left];
                const auto &b = *members[right];
                if (a.representation != b.representation ||
                    a.format != b.format ||
                    a.rasterization_samples !=
                        b.rasterization_samples ||
                    a.view_layout != b.view_layout ||
                    a.mip_levels != b.mip_levels ||
                    a.array_layers != b.array_layers ||
                    a.extent != b.extent) {
                    throw std::runtime_error(
                        "Vulkan physical alias group has "
                        "incompatible resource contracts: " +
                        a.logical_resource + " / " +
                        b.logical_resource);
                }
                if (!lifetimesDoNotOverlap(
                        a.lifetime, b.lifetime)) {
                    throw std::runtime_error(
                        "Vulkan physical alias group has overlapping "
                        "resource lifetimes: " +
                        a.logical_resource + " / " +
                        b.logical_resource);
                }
            }
        }
    }
}

using ScopeAttachmentContract =
    std::vector<std::tuple<
        std::string,
        VulkanPhysicalAttachmentAspect,
        std::optional<ImageSubresourceRange>>>;

ScopeAttachmentContract scopeAttachmentContract(
    const VulkanTargetPlan &plan,
    std::string_view node) {
    ScopeAttachmentContract result;
    for (const auto &attachment : plan.attachments) {
        if (attachment.node == node) {
            result.emplace_back(
                attachment.logical_resource,
                attachment.aspect,
                attachment.subresource);
        }
    }
    return result;
}

bool hasMaterializedAttachmentOperations(
    VulkanResourceRepresentation representation) {
    return representation ==
               VulkanResourceRepresentation::materialized_image ||
           representation ==
               VulkanResourceRepresentation::external;
}

const VulkanPhysicalResourcePlan &
requireScopeAttachmentResource(
    const VulkanTargetPlan &plan,
    std::string_view name) {
    const auto found = std::find_if(
        plan.resources.begin(), plan.resources.end(),
        [&](const auto &resource) {
            return resource.logical_resource == name;
        });
    if (found == plan.resources.end()) {
        throw std::runtime_error(
            "Vulkan physical attachment references an unknown "
            "resource: " +
            std::string{name});
    }
    return *found;
}

void validateDependencySafeNodeOrder(
    const CompiledLogicalRenderGraph &canonical_graph,
    const std::map<std::string, std::size_t, std::less<>>
        &position) {
    const auto require_before =
        [&](std::string_view source,
            std::string_view destination,
            std::string_view dependency_kind) {
            if (source == destination) return;
            const auto from = position.find(source);
            const auto to = position.find(destination);
            if (from == position.end() ||
                to == position.end()) {
                throw std::runtime_error(
                    "Vulkan physical scope partition lost a " +
                    std::string{dependency_kind} +
                    " dependency endpoint: " +
                    std::string{source} + " -> " +
                    std::string{destination});
            }
            if (from->second >= to->second) {
                throw std::runtime_error(
                    "Vulkan physical dependency-safe scope order "
                    "reverses " +
                    std::string{dependency_kind} +
                    " dependency: " +
                    std::string{source} + " -> " +
                    std::string{destination});
            }
        };

    for (const auto &edge :
         deriveLogicalDataEdges(canonical_graph)) {
        require_before(
            edge.producer_node,
            edge.consumer_node,
            "data");
    }
    for (const auto &node : canonical_graph.nodes) {
        for (const auto &dependency : node.after) {
            require_before(
                dependency, node.name, "after");
        }
        for (const auto &dependent : node.before) {
            require_before(
                node.name, dependent, "before");
        }
    }
}

void validateCompatibleRenderingScopeFusion(
    const VulkanTargetPlan &plan,
    const VulkanPhysicalScopeFragment &fragment,
    const std::set<std::size_t> &automatic_scope_indices) {
    if (automatic_scope_indices.size() < 2) return;

    const auto &first =
        plan.scopes[*automatic_scope_indices.begin()];
    if (first.kind !=
            VulkanPhysicalScopeKind::rendering ||
        first.rasterization_samples != 1 ||
        !first.local_reads.empty()) {
        throw std::runtime_error(
            "Vulkan physical dependency-safe scope fusion currently "
            "requires materialized single-sample rendering scopes: " +
            fragment.id);
    }
    for (const auto scope_index :
         automatic_scope_indices) {
        const auto &candidate =
            plan.scopes[scope_index];
        if (candidate.kind != first.kind ||
            candidate.rasterization_samples !=
                first.rasterization_samples ||
            candidate.view_execution !=
                first.view_execution ||
            candidate.view_count != first.view_count ||
            candidate.execution_count !=
                first.execution_count ||
            candidate.view_mask != first.view_mask ||
            !candidate.local_reads.empty()) {
            throw std::runtime_error(
                "Vulkan physical dependency-safe scope fusion has "
                "incompatible kind/sample/view contracts: " +
                fragment.id);
        }
    }

    ScopeAttachmentContract expected;
    bool first_node = true;
    std::map<std::string,
             const VulkanPhysicalResourcePlan *,
             std::less<>>
        resources;
    for (const auto &resource : plan.resources) {
        resources.emplace(
            resource.logical_resource, &resource);
    }
    for (const auto &node : fragment.nodes) {
        const auto contract =
            scopeAttachmentContract(plan, node);
        if (contract.empty()) {
            throw std::runtime_error(
                "Vulkan physical dependency-safe rendering-scope "
                "fusion requires at least one attachment per node: " +
                node);
        }
        if (first_node) {
            expected = contract;
            first_node = false;
        } else if (contract != expected) {
            throw std::runtime_error(
                "Vulkan physical dependency-safe rendering-scope "
                "fusion requires identical ordered attachments: " +
                fragment.id);
        }
        for (const auto &[resource, aspect, subresource] :
             contract) {
            (void)aspect;
            (void)subresource;
            const auto found = resources.find(resource);
            if (found == resources.end() ||
                found->second->representation !=
                    VulkanResourceRepresentation::
                        materialized_image) {
                throw std::runtime_error(
                    "Vulkan physical dependency-safe rendering-scope "
                    "fusion requires internal materialized images: " +
                    resource);
            }
        }
    }
}

void recomputePhysicalResourceLifetimes(
    VulkanTargetPlan &plan) {
    std::map<std::string,
             TargetLoweringResource *,
             std::less<>>
        resources;
    for (auto &resource :
         plan.lowering_graph.resources) {
        resource.lifetime = {};
        resources.emplace(
            resource.logical.name, &resource);
    }
    const auto include =
        [&](std::string_view name,
            std::size_t position) {
            const auto found = resources.find(name);
            if (found == resources.end()) return;
            auto &lifetime =
                found->second->lifetime;
            if (!lifetime.used) {
                lifetime = {
                    true, position, position};
            } else {
                lifetime.first_use =
                    std::min(
                        lifetime.first_use,
                        position);
                lifetime.last_use =
                    std::max(
                        lifetime.last_use,
                        position);
            }
        };
    for (std::size_t node_index = 0;
         node_index <
         plan.lowering_graph.nodes.size();
         ++node_index) {
        for (const auto &use :
             plan.lowering_graph.nodes[node_index]
                 .logical.uses) {
            if (use.input_value) {
                include(
                    use.input_value->resource,
                    node_index);
            }
            if (use.output_value) {
                include(
                    use.output_value->resource,
                    node_index);
            }
        }
    }
    if (!plan.lowering_graph.nodes.empty()) {
        const auto terminal =
            plan.lowering_graph.nodes.size() - 1;
        for (auto &[name, resource] :
             resources) {
            (void)name;
            if (!resource->lifetime.used) continue;
            if (resource->logical.materialization ==
                    LogicalMaterializationRequirement::
                        required ||
                resource->logical.materialization ==
                    LogicalMaterializationRequirement::
                        external ||
                resource->pattern.require_store) {
                resource->lifetime.last_use =
                    terminal;
            }
        }
    }
    for (auto &physical : plan.resources) {
        const auto found =
            resources.find(
                physical.logical_resource);
        if (found != resources.end()) {
            physical.lifetime =
                found->second->lifetime;
        }
    }
}

void applyScopePartition(
    const CompiledLogicalRenderGraph &canonical_graph,
    VulkanTargetPlan &plan,
    const std::vector<VulkanPhysicalScopeFragment>
        &fragments,
    VulkanPhysicalScopeEditMode edit_mode) {
    std::vector<std::string> canonical_nodes;
    canonical_nodes.reserve(plan.lowering_graph.nodes.size());
    std::map<std::string,
             std::size_t, std::less<>>
        lowering_node_indices;
    for (const auto &node : plan.lowering_graph.nodes) {
        canonical_nodes.push_back(node.logical.name);
        lowering_node_indices.emplace(
            node.logical.name,
            canonical_nodes.size() - 1);
    }

    std::map<std::string, std::size_t, std::less<>>
        automatic_scope_by_node;
    for (std::size_t scope_index = 0;
         scope_index < plan.scopes.size();
         ++scope_index) {
        for (const auto &node :
             plan.scopes[scope_index].nodes) {
            if (!automatic_scope_by_node
                     .emplace(node, scope_index)
                     .second) {
                throw std::runtime_error(
                    "automatic Vulkan plan assigns a node to more "
                    "than one scope: " +
                    node);
            }
        }
    }

    std::vector<std::string> authored_nodes;
    std::vector<VulkanPhysicalScopePlan> scopes;
    scopes.reserve(fragments.size());
    for (const auto &fragment : fragments) {
        authored_nodes.insert(
            authored_nodes.end(),
            fragment.nodes.begin(), fragment.nodes.end());
        const auto first =
            automatic_scope_by_node.find(
                fragment.nodes.front());
        if (first == automatic_scope_by_node.end()) {
            throw std::runtime_error(
                "Vulkan physical scope fragment names an unknown "
                "node: " +
                fragment.nodes.front());
        }
        std::set<std::size_t>
            source_scope_indices;
        for (const auto &node : fragment.nodes) {
            const auto found =
                automatic_scope_by_node.find(node);
            if (found == automatic_scope_by_node.end()) {
                throw std::runtime_error(
                    "Vulkan physical scope fragment names an "
                    "unknown node: " +
                    node);
            }
            source_scope_indices.insert(
                found->second);
            if (edit_mode ==
                    VulkanPhysicalScopeEditMode::
                        split_only &&
                found->second != first->second) {
                throw std::runtime_error(
                    "Vulkan physical fragments may split an "
                    "automatic scope but cannot fuse nodes from "
                    "different automatic scopes: " +
                    fragment.id);
            }
        }
        if (source_scope_indices.size() > 1) {
            validateCompatibleRenderingScopeFusion(
                plan, fragment,
                source_scope_indices);
        }

        auto scope = plan.scopes[first->second];
        scope.id = fragment.id;
        scope.nodes = fragment.nodes;
        scope.single_rendering_instance =
            fragment.nodes.size() > 1 &&
            (source_scope_indices.size() > 1 ||
             scope.single_rendering_instance);
        scope.local_reads.clear();
        scope.region_tags.clear();
        for (const auto &node : fragment.nodes) {
            const auto lowering =
                lowering_node_indices.find(node);
            if (lowering ==
                lowering_node_indices.end()) {
                throw std::runtime_error(
                    "Vulkan physical scope fragment node is absent "
                    "from lowering graph: " +
                    node);
            }
            scope.region_tags.insert(
                scope.region_tags.end(),
                plan.lowering_graph
                    .nodes[lowering->second]
                    .logical.region_tags.begin(),
                plan.lowering_graph
                    .nodes[lowering->second]
                    .logical.region_tags.end());
        }
        std::sort(
            scope.region_tags.begin(),
            scope.region_tags.end());
        scope.region_tags.erase(
            std::unique(
                scope.region_tags.begin(),
                scope.region_tags.end()),
            scope.region_tags.end());
        scopes.push_back(std::move(scope));
    }
    std::map<std::string, std::size_t, std::less<>>
        authored_position;
    for (std::size_t index = 0;
         index < authored_nodes.size(); ++index) {
        if (!lowering_node_indices.contains(
                authored_nodes[index])) {
            throw std::runtime_error(
                "Vulkan physical scope fragment names an unknown "
                "node: " +
                authored_nodes[index]);
        }
        if (!authored_position
                 .emplace(authored_nodes[index], index)
                 .second) {
            throw std::runtime_error(
                "Vulkan physical scope fragments contain a node "
                "more than once: " +
                authored_nodes[index]);
        }
    }
    if (authored_nodes.size() !=
            canonical_nodes.size() ||
        authored_position.size() !=
            canonical_nodes.size()) {
        throw std::runtime_error(
            "Vulkan physical scope fragments must form an exact "
            "partition of the lowered graph nodes");
    }
    if (edit_mode ==
        VulkanPhysicalScopeEditMode::split_only) {
        if (authored_nodes != canonical_nodes) {
            throw std::runtime_error(
                "Vulkan physical split-only scope fragments must "
                "preserve lowered graph node order");
        }
    } else {
        validateDependencySafeNodeOrder(
            canonical_graph,
            authored_position);
    }

    plan.scopes = std::move(scopes);
    if (authored_nodes != canonical_nodes) {
        std::vector<TargetLoweringNode>
            reordered_nodes;
        reordered_nodes.reserve(
            authored_nodes.size());
        for (const auto &node :
             authored_nodes) {
            reordered_nodes.push_back(
                std::move(
                    plan.lowering_graph.nodes[
                        lowering_node_indices.at(
                            node)]));
        }
        plan.lowering_graph.nodes =
            std::move(reordered_nodes);
    }
    recomputePhysicalResourceLifetimes(plan);
}

void validateScopeResourceBoundaries(
    const CompiledLogicalRenderGraph &canonical_graph,
    VulkanTargetPlan &plan) {
    std::map<std::string, std::size_t, std::less<>>
        scope_by_node;
    for (std::size_t scope_index = 0;
         scope_index < plan.scopes.size(); ++scope_index) {
        auto &scope = plan.scopes[scope_index];
        scope.local_reads.clear();
        for (const auto &node : scope.nodes) {
            if (!scope_by_node.emplace(
                    node, scope_index)
                     .second) {
                throw std::runtime_error(
                    "Vulkan physical fragment assigns a node to "
                    "more than one scope: " +
                    node);
            }
        }
    }

    std::map<std::string,
             const VulkanPhysicalResourcePlan *,
             std::less<>>
        resources;
    for (const auto &resource : plan.resources) {
        resources.emplace(
            resource.logical_resource, &resource);
    }
    std::map<std::string,
             const LogicalGraphNode *, std::less<>>
        nodes;
    for (const auto &node : canonical_graph.nodes) {
        nodes.emplace(node.name, &node);
    }

    for (const auto &edge :
         deriveLogicalDataEdges(canonical_graph)) {
        const auto resource =
            resources.find(edge.value.resource);
        if (resource == resources.end()) continue;
        const auto producer =
            scope_by_node.find(edge.producer_node);
        const auto consumer =
            scope_by_node.find(edge.consumer_node);
        if (producer == scope_by_node.end() ||
            consumer == scope_by_node.end()) {
            throw std::runtime_error(
                "Vulkan physical fragment lost a node used by a "
                "resource dependency");
        }

        if (producer->second != consumer->second) {
            if (resource->second->representation ==
                    VulkanResourceRepresentation::
                        tile_local_attachment ||
                resource->second->representation ==
                    VulkanResourceRepresentation::
                        transient_attachment) {
                throw std::runtime_error(
                    "Vulkan physical scope split exposes "
                    "scope-local resource across scopes; "
                    "materialize it first: " +
                    edge.value.resource);
            }
            continue;
        }

        if (resource->second->representation ==
            VulkanResourceRepresentation::
                tile_local_attachment) {
            plan.scopes[consumer->second]
                .local_reads.push_back(
                    edge.value.resource);
            continue;
        }

        const auto consumer_node =
            nodes.find(edge.consumer_node);
        if (consumer_node == nodes.end()) {
            throw std::runtime_error(
                "Vulkan physical fragment cannot inspect consumer "
                "node: " +
                edge.consumer_node);
        }
        const auto use = std::find_if(
            consumer_node->second->uses.begin(),
            consumer_node->second->uses.end(),
            [&](const LogicalResourceUse &candidate) {
                return candidate.port ==
                       edge.consumer_port;
            });
        const auto attachment_read_write =
            use != consumer_node->second->uses.end() &&
            resource->second->representation ==
                VulkanResourceRepresentation::
                    materialized_image &&
            use->access ==
                LogicalAccessMode::read_write &&
            use->footprint.kind ==
                LogicalReadFootprintKind::same_pixel &&
            (use->intent ==
                 LogicalAccessIntent::automatic ||
             use->intent ==
                 LogicalAccessIntent::attachment);
        if (!attachment_read_write) {
            throw std::runtime_error(
                "Vulkan physical fragment keeps nodes fused after "
                "materializing a dependency that requires separate "
                "scopes: " +
                edge.value.resource);
        }
    }
    for (auto &scope : plan.scopes) {
        std::sort(
            scope.local_reads.begin(),
            scope.local_reads.end());
        scope.local_reads.erase(
            std::unique(
                scope.local_reads.begin(),
                scope.local_reads.end()),
            scope.local_reads.end());
    }
}

void validateMaterializedRenderingScopeOperations(
    const VulkanTargetPlan &plan) {
    if (plan.attachments.empty()) return;

    for (const auto &scope : plan.scopes) {
        if (!scope.single_rendering_instance) {
            continue;
        }
        const auto local_read_scope =
            !scope.local_reads.empty();
        if (local_read_scope) {
            std::map<
                std::tuple<
                    std::string,
                    VulkanPhysicalAttachmentAspect,
                    std::optional<
                        ImageSubresourceRange>>,
                const VulkanPhysicalAttachmentPlan *>
                previous_writers;
            for (const auto &node : scope.nodes) {
                for (const auto &attachment :
                     plan.attachments) {
                    if (attachment.node != node) continue;
                    if (!hasMaterializedAttachmentOperations(
                            requireScopeAttachmentResource(
                                plan,
                                attachment.logical_resource)
                                .representation)) {
                        continue;
                    }
                    const auto key = std::tuple{
                        attachment.logical_resource,
                        attachment.aspect,
                        attachment.subresource};
                    const auto previous =
                        previous_writers.find(key);
                    if (previous !=
                        previous_writers.end()) {
                        if (previous->second->store_op !=
                                VulkanPhysicalAttachmentStoreOp::
                                    store ||
                            attachment.load_op !=
                                VulkanPhysicalAttachmentLoadOp::
                                    load) {
                            throw std::runtime_error(
                                "Vulkan physical local-read scope "
                                "cannot preserve intermediate "
                                "materialized attachment operations: " +
                                scope.id + " -> " +
                                attachment.logical_resource);
                        }
                    }
                    previous_writers[key] =
                        &attachment;
                }
            }
            continue;
        }

        ScopeAttachmentContract expected;
        for (std::size_t node_index = 0;
             node_index < scope.nodes.size();
             ++node_index) {
            const auto &node =
                scope.nodes[node_index];
            const auto contract =
                scopeAttachmentContract(
                    plan, node);
            if (contract.empty()) {
                throw std::runtime_error(
                    "Vulkan physical materialized rendering scope "
                    "has a node without attachments: " +
                    node);
            }
            if (node_index == 0) {
                expected = contract;
            } else if (contract != expected) {
                throw std::runtime_error(
                    "Vulkan physical materialized rendering scope "
                    "requires identical ordered attachments: " +
                    scope.id);
            }
            for (const auto &attachment :
                 plan.attachments) {
                if (attachment.node != node) continue;
                if (node_index > 0 &&
                    attachment.load_op !=
                        VulkanPhysicalAttachmentLoadOp::
                            load) {
                    throw std::runtime_error(
                        "Vulkan physical fused rendering scope "
                        "requires every later attachment to Load: " +
                        node + " -> " +
                        attachment.logical_resource);
                }
                if (node_index + 1 <
                        scope.nodes.size() &&
                    attachment.store_op !=
                        VulkanPhysicalAttachmentStoreOp::
                            store) {
                    throw std::runtime_error(
                        "Vulkan physical fused rendering scope "
                        "requires every non-final attachment to Store: " +
                        node + " -> " +
                        attachment.logical_resource);
                }
            }
        }
    }
}

} // namespace

void validateVulkanPhysicalAttachmentPlans(
    const CompiledLogicalRenderGraph &canonical_graph,
    std::span<const VulkanPhysicalResourcePlan> resources,
    std::span<const VulkanPhysicalAttachmentPlan> attachments) {
    std::map<std::string,
             const LogicalGraphNode *, std::less<>>
        nodes;
    for (const auto &node : canonical_graph.nodes) {
        nodes.emplace(node.name, &node);
    }
    std::map<
        std::string,
        const VulkanPhysicalResourcePlan *,
        std::less<>>
        physical_resources;
    for (const auto &resource : resources) {
        physical_resources.emplace(
            resource.logical_resource, &resource);
    }
    std::set<
        std::pair<std::string, std::string>>
        identities;
    for (const auto &attachment : attachments) {
        requireNonEmpty(
            attachment.node,
            "Vulkan physical attachment node");
        requireNonEmpty(
            attachment.logical_resource,
            "Vulkan physical attachment resource");
        if (!identities.emplace(
                attachment.node,
                attachment.logical_resource)
                 .second) {
            throw std::runtime_error(
                "Vulkan physical attachment plan has duplicate "
                "node/resource identity: " +
                attachment.node + " -> " +
                attachment.logical_resource);
        }
        const auto physical_resource =
            physical_resources.find(
                attachment.logical_resource);
        if (physical_resource ==
            physical_resources.end()) {
            throw std::runtime_error(
                "Vulkan physical attachment plan references an "
                "unknown physical resource: " +
                attachment.logical_resource);
        }
        if (attachment.subresource) {
            const auto &range =
                *attachment.subresource;
            if (attachment.logical_resource ==
                    "swapchain" ||
                range.mip_count_mode !=
                    ImageSubresourceMipCountMode::fixed ||
                range.level_count != 1 ||
                range.layer_count == 0 ||
                range.base_array_layer >=
                    physical_resource->second
                        ->array_layers ||
                range.layer_count >
                    physical_resource->second
                            ->array_layers -
                        range.base_array_layer ||
                (physical_resource->second
                         ->mip_levels.mode ==
                     ImageMipLevelMode::fixed &&
                 range.base_mip_level >=
                     physical_resource->second
                         ->mip_levels.count) ||
                (physical_resource->second
                         ->rasterization_samples >
                     1 &&
                 range.base_mip_level != 0)) {
                throw std::runtime_error(
                    "Vulkan physical attachment plan has an "
                    "unsupported image subresource: " +
                    attachment.node + " -> " +
                    attachment.logical_resource);
            }
        }
        const auto found_node =
            nodes.find(attachment.node);
        if (found_node == nodes.end()) {
            throw std::runtime_error(
                "Vulkan physical attachment plan references an "
                "unknown logical node: " +
                attachment.node);
        }
        if (found_node->second->kind !=
                LogicalGraphNodeKind::render &&
            found_node->second->kind !=
                LogicalGraphNodeKind::output_transform) {
            throw std::runtime_error(
                "Vulkan physical attachment plan references a "
                "non-raster logical node: " +
                attachment.node);
        }
        const auto writes_resource =
            std::any_of(
                found_node->second->uses.begin(),
                found_node->second->uses.end(),
                [&](const LogicalResourceUse &use) {
                    return use.output_value &&
                           use.output_value->resource ==
                               attachment.logical_resource;
                });
        if (!writes_resource) {
            throw std::runtime_error(
                "Vulkan physical attachment plan resource is not "
                "written by its logical node: " +
                attachment.node + " -> " +
                attachment.logical_resource);
        }
        const auto loads_existing_attachment =
            std::any_of(
                found_node->second->uses.begin(),
                found_node->second->uses.end(),
                [&](const LogicalResourceUse &use) {
                    return use.access ==
                               LogicalAccessMode::read_write &&
                           use.input_value &&
                           use.output_value &&
                           use.input_value->resource ==
                               attachment.logical_resource &&
                           use.output_value->resource ==
                               attachment.logical_resource &&
                           use.footprint.kind ==
                               LogicalReadFootprintKind::same_pixel &&
                           (use.intent ==
                                LogicalAccessIntent::automatic ||
                            use.intent ==
                                LogicalAccessIntent::attachment);
                });
        const auto physically_loads =
            attachment.load_op ==
            VulkanPhysicalAttachmentLoadOp::load;
        if (loads_existing_attachment != physically_loads) {
            throw std::runtime_error(
                physically_loads
                    ? "Vulkan physical attachment Load has no "
                      "logical read dependency: " +
                          attachment.node + " -> " +
                          attachment.logical_resource
                    : "Vulkan physical attachment cannot discard or "
                      "clear a logical read dependency: " +
                          attachment.node + " -> " +
                          attachment.logical_resource);
        }
    }
}

std::uint64_t vulkanAutomaticTargetPlanFingerprint(
    const TargetTopologySnapshot &topology,
    const VulkanTargetPlan &automatic_plan) {
    StableFingerprint64 fingerprint;
    fingerprint.appendString(
        "pelican.vulkan_automatic_target_plan@1");
    fingerprint.appendString(
        automaticPlanPayload(
            topology, automatic_plan)
            .dump());
    return fingerprint.value();
}

VulkanPhysicalFragmentPackage
ejectVulkanPhysicalFragmentPackage(
    const VulkanTargetPlan &plan) {
    requireNonEmpty(
        plan.graph, "Vulkan target plan graph");
    requireNonEmpty(
        plan.backend_selection.selected_candidate,
        "Vulkan target plan selected backend candidate");

    VulkanPhysicalFragmentPackage package{
        .schema_version = kFragmentVersionV3,
        .scope_edit_mode =
            VulkanPhysicalScopeEditMode::dependency_safe,
        .graph = plan.graph,
        .logical_graph_fingerprint =
            plan.logical_graph_fingerprint,
        .automatic_plan_fingerprint =
            plan.automatic_plan_fingerprint,
        .backend_candidate =
            plan.backend_selection.selected_candidate,
        .scopes =
            std::vector<VulkanPhysicalScopeFragment>{},
        .alias_groups =
            std::vector<
                VulkanPhysicalAliasGroupFragment>{},
        .attachments = std::nullopt,
    };
    package.resources.reserve(plan.resources.size());
    for (const auto &resource : plan.resources) {
        package.resources.push_back(
            VulkanPhysicalResourceFragment{
                .logical_resource =
                    resource.logical_resource,
                .format =
                    resource.format.empty()
                        ? std::nullopt
                        : std::optional{
                              resource.format},
                .representation =
                    resource.representation,
            });
    }
    package.scopes->reserve(plan.scopes.size());
    for (const auto &scope : plan.scopes) {
        package.scopes->push_back(
            VulkanPhysicalScopeFragment{
                .id = scope.id,
                .nodes = scope.nodes,
            });
    }
    package.alias_groups->reserve(
        plan.alias_groups.size());
    for (const auto &group : plan.alias_groups) {
        package.alias_groups->push_back(
            VulkanPhysicalAliasGroupFragment{
                .id = group.id,
                .resources = group.resources,
            });
    }
    if (!plan.attachments.empty()) {
        package.attachments.emplace();
        package.attachments->reserve(
            plan.attachments.size());
        for (const auto &attachment :
             plan.attachments) {
            package.attachments->push_back(
                VulkanPhysicalAttachmentFragment{
                    .node = attachment.node,
                    .logical_resource =
                        attachment.logical_resource,
                    .load_op =
                        attachment.load_op,
                    .store_op =
                        attachment.store_op,
                });
        }
    }
    return canonicalizePackage(std::move(package));
}

nlohmann::ordered_json
vulkanPhysicalFragmentPackageToJson(
    const VulkanPhysicalFragmentPackage &source) {
    const auto package =
        canonicalizePackage(source);
    nlohmann::ordered_json result{
        {"schema", kFragmentSchema},
        {"version", package.schema_version},
        {"graph", package.graph},
        {"logical_graph_fingerprint",
         stableFingerprint64String(
             package.logical_graph_fingerprint)},
        {"automatic_plan_fingerprint",
         stableFingerprint64String(
             package.automatic_plan_fingerprint)},
        {"backend_candidate",
         package.backend_candidate},
        {"resources", nlohmann::ordered_json::array()},
    };
    if (package.schema_version >= kFragmentVersionV3) {
        result["scope_edit_mode"] =
            vulkanPhysicalScopeEditModeName(
                package.scope_edit_mode);
    }
    for (const auto &resource : package.resources) {
        nlohmann::ordered_json entry{
            {"logical_resource",
             resource.logical_resource},
        };
        if (resource.format) {
            entry["format"] = *resource.format;
        }
        if (resource.representation) {
            entry["representation"] =
                vulkanResourceRepresentationName(
                    *resource.representation);
        }
        result["resources"].push_back(
            std::move(entry));
    }
    if (package.scopes) {
        result["scopes"] =
            nlohmann::ordered_json::array();
        for (const auto &scope : *package.scopes) {
            result["scopes"].push_back(
                nlohmann::ordered_json{
                    {"id", scope.id},
                    {"nodes", scope.nodes},
                });
        }
    }
    if (package.alias_groups) {
        result["alias_groups"] =
            nlohmann::ordered_json::array();
        for (const auto &group :
             *package.alias_groups) {
            result["alias_groups"].push_back(
                nlohmann::ordered_json{
                    {"id", group.id},
                    {"resources", group.resources},
                });
        }
    }
    if (package.attachments) {
        result["attachments"] =
            nlohmann::ordered_json::array();
        for (const auto &attachment :
             *package.attachments) {
            nlohmann::ordered_json entry{
                {"node", attachment.node},
                {"logical_resource",
                 attachment.logical_resource},
            };
            if (attachment.load_op) {
                entry["load_op"] =
                    vulkanPhysicalAttachmentLoadOpName(
                        *attachment.load_op);
            }
            if (attachment.store_op) {
                entry["store_op"] =
                    vulkanPhysicalAttachmentStoreOpName(
                        *attachment.store_op);
            }
            result["attachments"].push_back(
                std::move(entry));
        }
    }
    return result;
}

VulkanPhysicalFragmentPackage
vulkanPhysicalFragmentPackageFromJson(
    const nlohmann::json &document) {
    constexpr std::string_view context =
        "Vulkan physical fragment package";
    if (!document.is_object()) {
        throw std::runtime_error(
            std::string{context} + " must be an object");
    }
    if (requireJsonString(
            document, "schema", context) !=
        kFragmentSchema) {
        throw std::runtime_error(
            std::string{context} + " schema must be '" +
            std::string{kFragmentSchema} + "'");
    }
    const auto version = document.find("version");
    if (version == document.end() ||
        !version->is_number_integer()) {
        throw std::runtime_error(
            std::string{context} +
            " version must be 1, 2, or 3");
    }
    const auto schema_version =
        version->get<std::int64_t>();
    if (schema_version != kFragmentVersionV1 &&
        schema_version != kFragmentVersionV2 &&
        schema_version != kFragmentVersionV3) {
        throw std::runtime_error(
            std::string{context} +
            " version must be 1, 2, or 3");
    }
    if (schema_version == kFragmentVersionV1) {
        requireOnlyKeys(
            document,
            {
                "schema",
                "version",
                "graph",
                "logical_graph_fingerprint",
                "automatic_plan_fingerprint",
                "backend_candidate",
                "resources",
                "scopes",
                "alias_groups",
            },
            context);
    } else if (schema_version == kFragmentVersionV2) {
        requireOnlyKeys(
            document,
            {
                "schema",
                "version",
                "graph",
                "logical_graph_fingerprint",
                "automatic_plan_fingerprint",
                "backend_candidate",
                "resources",
                "scopes",
                "alias_groups",
                "attachments",
            },
            context);
    } else {
        requireOnlyKeys(
            document,
            {
                "schema",
                "version",
                "scope_edit_mode",
                "graph",
                "logical_graph_fingerprint",
                "automatic_plan_fingerprint",
                "backend_candidate",
                "resources",
                "scopes",
                "alias_groups",
                "attachments",
            },
            context);
    }

    VulkanPhysicalFragmentPackage package{
        .schema_version =
            static_cast<std::uint32_t>(
                schema_version),
        .scope_edit_mode =
            schema_version >= kFragmentVersionV3
                ? parseScopeEditMode(
                      requireJsonString(
                          document,
                          "scope_edit_mode",
                          context))
                : VulkanPhysicalScopeEditMode::
                      split_only,
        .graph =
            requireJsonString(
                document, "graph", context),
        .logical_graph_fingerprint =
            requireFingerprint(
                document,
                "logical_graph_fingerprint",
                context),
        .automatic_plan_fingerprint =
            requireFingerprint(
                document,
                "automatic_plan_fingerprint",
                context),
        .backend_candidate =
            requireJsonString(
                document,
                "backend_candidate",
                context),
    };

    const auto resources = document.find("resources");
    if (resources == document.end() ||
        !resources->is_array()) {
        throw std::runtime_error(
            std::string{context} +
            " resources must be an array");
    }
    for (const auto &entry : *resources) {
        if (!entry.is_object()) {
            throw std::runtime_error(
                "Vulkan physical resource fragment must be an "
                "object");
        }
        requireOnlyKeys(
            entry,
            {
                "logical_resource",
                "format",
                "representation",
            },
            "Vulkan physical resource fragment");
        VulkanPhysicalResourceFragment resource{
            .logical_resource =
                requireJsonString(
                    entry, "logical_resource",
                    "Vulkan physical resource fragment"),
        };
        if (const auto format = entry.find("format");
            format != entry.end()) {
            if (!format->is_string() ||
                format->get_ref<const std::string &>()
                    .empty()) {
                throw std::runtime_error(
                    "Vulkan physical resource fragment format "
                    "must be a non-empty string");
            }
            resource.format =
                format->get<std::string>();
        }
        if (const auto representation =
                entry.find("representation");
            representation != entry.end()) {
            if (!representation->is_string()) {
                throw std::runtime_error(
                    "Vulkan physical resource fragment "
                    "representation must be a string");
            }
            resource.representation =
                parseRepresentation(
                    representation
                        ->get_ref<
                            const std::string &>());
        }
        package.resources.push_back(
            std::move(resource));
    }

    if (const auto scopes = document.find("scopes");
        scopes != document.end()) {
        if (!scopes->is_array()) {
            throw std::runtime_error(
                std::string{context} +
                " scopes must be an array");
        }
        package.scopes =
            std::vector<VulkanPhysicalScopeFragment>{};
        for (const auto &entry : *scopes) {
            if (!entry.is_object()) {
                throw std::runtime_error(
                    "Vulkan physical scope fragment must be an "
                    "object");
            }
            requireOnlyKeys(
                entry, {"id", "nodes"},
                "Vulkan physical scope fragment");
            const auto nodes = entry.find("nodes");
            if (nodes == entry.end() ||
                !nodes->is_array()) {
                throw std::runtime_error(
                    "Vulkan physical scope fragment nodes must "
                    "be an array");
            }
            VulkanPhysicalScopeFragment scope{
                .id =
                    requireJsonString(
                        entry, "id",
                        "Vulkan physical scope fragment"),
            };
            for (const auto &node : *nodes) {
                if (!node.is_string() ||
                    node.get_ref<const std::string &>()
                        .empty()) {
                    throw std::runtime_error(
                        "Vulkan physical scope fragment node must "
                        "be a non-empty string");
                }
                scope.nodes.push_back(
                    node.get<std::string>());
            }
            package.scopes->push_back(
                std::move(scope));
        }
    }

    if (const auto groups =
            document.find("alias_groups");
        groups != document.end()) {
        if (!groups->is_array()) {
            throw std::runtime_error(
                std::string{context} +
                " alias_groups must be an array");
        }
        package.alias_groups =
            std::vector<
                VulkanPhysicalAliasGroupFragment>{};
        for (const auto &entry : *groups) {
            if (!entry.is_object()) {
                throw std::runtime_error(
                    "Vulkan physical alias fragment must be an "
                    "object");
            }
            requireOnlyKeys(
                entry, {"id", "resources"},
                "Vulkan physical alias fragment");
            const auto resources_value =
                entry.find("resources");
            if (resources_value == entry.end() ||
                !resources_value->is_array()) {
                throw std::runtime_error(
                    "Vulkan physical alias fragment resources "
                    "must be an array");
            }
            VulkanPhysicalAliasGroupFragment group{
                .id =
                    requireJsonString(
                        entry, "id",
                        "Vulkan physical alias fragment"),
            };
            for (const auto &resource :
                 *resources_value) {
                if (!resource.is_string() ||
                    resource
                        .get_ref<
                            const std::string &>()
                        .empty()) {
                    throw std::runtime_error(
                        "Vulkan physical alias fragment resource "
                        "must be a non-empty string");
                }
                group.resources.push_back(
                    resource.get<std::string>());
            }
            package.alias_groups->push_back(
                std::move(group));
        }
    }
    if (const auto attachments =
            document.find("attachments");
        attachments != document.end()) {
        if (!attachments->is_array()) {
            throw std::runtime_error(
                std::string{context} +
                " attachments must be an array");
        }
        package.attachments =
            std::vector<
                VulkanPhysicalAttachmentFragment>{};
        for (const auto &entry : *attachments) {
            if (!entry.is_object()) {
                throw std::runtime_error(
                    "Vulkan physical attachment fragment must be "
                    "an object");
            }
            requireOnlyKeys(
                entry,
                {
                    "node",
                    "logical_resource",
                    "load_op",
                    "store_op",
                },
                "Vulkan physical attachment fragment");
            VulkanPhysicalAttachmentFragment attachment{
                .node =
                    requireJsonString(
                        entry, "node",
                        "Vulkan physical attachment fragment"),
                .logical_resource =
                    requireJsonString(
                        entry, "logical_resource",
                        "Vulkan physical attachment fragment"),
            };
            if (const auto load =
                    entry.find("load_op");
                load != entry.end()) {
                if (!load->is_string()) {
                    throw std::runtime_error(
                        "Vulkan physical attachment fragment "
                        "load_op must be a string");
                }
                attachment.load_op =
                    parseAttachmentLoadOp(
                        load->get_ref<
                            const std::string &>());
            }
            if (const auto store =
                    entry.find("store_op");
                store != entry.end()) {
                if (!store->is_string()) {
                    throw std::runtime_error(
                        "Vulkan physical attachment fragment "
                        "store_op must be a string");
                }
                attachment.store_op =
                    parseAttachmentStoreOp(
                        store->get_ref<
                            const std::string &>());
            }
            package.attachments->push_back(
                std::move(attachment));
        }
    }
    return canonicalizePackage(
        std::move(package));
}

namespace {

using AttachmentIdentity =
    std::pair<std::string, std::string>;

bool hasDownstreamAttachmentLoad(
    const CompiledLogicalRenderGraph &graph,
    std::span<const VulkanPhysicalAttachmentPlan> attachments,
    std::string_view producer_node,
    std::string_view resource) {
    for (const auto &edge :
         deriveLogicalDataEdges(graph)) {
        if (edge.producer_node != producer_node ||
            edge.value.resource != resource) {
            continue;
        }
        const auto consumer = std::find_if(
            attachments.begin(), attachments.end(),
            [&](const auto &attachment) {
                return attachment.node ==
                           edge.consumer_node &&
                       attachment.logical_resource ==
                           resource &&
                       attachment.load_op ==
                           VulkanPhysicalAttachmentLoadOp::
                               load;
            });
        if (consumer != attachments.end()) {
            return true;
        }
    }
    return false;
}

} // namespace

VulkanTargetPlan linkVulkanPhysicalFragment(
    const CompiledLogicalRenderGraph &canonical_graph,
    const TargetTopologySnapshot &topology,
    VulkanTargetPlan automatic_plan,
    VulkanPhysicalFragmentPackage source_package,
    std::span<
        const VulkanPhysicalResourceFormatCapability>
        format_capabilities) {
    auto package =
        canonicalizePackage(
            std::move(source_package));
    if (package.graph != automatic_plan.graph ||
        package.graph != canonical_graph.name) {
        throw std::runtime_error(
            "Vulkan physical fragment graph mismatch: expected '" +
            canonical_graph.name + "', got '" +
            package.graph + "'");
    }
    const auto logical_fingerprint =
        vulkanTargetPlanLogicalGraphFingerprint(
            canonical_graph);
    if (package.logical_graph_fingerprint !=
            logical_fingerprint ||
        automatic_plan.logical_graph_fingerprint !=
            logical_fingerprint) {
        throw std::runtime_error(
            "Vulkan physical fragment is stale for logical graph '" +
            canonical_graph.name + "'");
    }
    if (package.backend_candidate !=
        automatic_plan.backend_selection
            .selected_candidate) {
        throw std::runtime_error(
            "Vulkan physical fragment backend candidate mismatch: "
            "expected '" +
            automatic_plan.backend_selection
                .selected_candidate +
            "', got '" + package.backend_candidate + "'");
    }
    const auto automatic_fingerprint =
        vulkanAutomaticTargetPlanFingerprint(
            topology, automatic_plan);
    if (automatic_plan.automatic_plan_fingerprint !=
        automatic_fingerprint) {
        throw std::runtime_error(
            "Vulkan physical fragment received an inconsistent "
            "automatic base plan fingerprint");
    }
    if (package.automatic_plan_fingerprint !=
        automatic_fingerprint) {
        throw std::runtime_error(
            "Vulkan physical fragment is stale for the current "
            "target facts/provider generation: expected " +
            stableFingerprint64String(
                automatic_fingerprint) +
            ", got " +
            stableFingerprint64String(
                package.automatic_plan_fingerprint));
    }

    const auto canonical_topology =
        canonicalizeTargetTopology(topology);
    const auto &probe = selectedProbe(automatic_plan);
    const auto *endpoint =
        findTargetEndpoint(
            canonical_topology, probe.endpoint);
    if (endpoint == nullptr) {
        throw std::runtime_error(
            "Vulkan physical fragment selected endpoint is absent "
            "from target topology: " +
            probe.endpoint);
    }
    std::set<std::string, std::less<>>
        endpoint_capabilities(
            endpoint->capabilities.begin(),
            endpoint->capabilities.end());
    const auto indexed_format_capabilities =
        indexFormatCapabilities(
            format_capabilities);
    validateVulkanPhysicalAttachmentPlans(
        canonical_graph, automatic_plan.resources,
        automatic_plan.attachments);

    std::map<std::string,
             TargetLoweringResource *,
             std::less<>>
        lowering_resources;
    for (auto &resource :
         automatic_plan.lowering_graph.resources) {
        lowering_resources.emplace(
            resource.logical.name, &resource);
    }
    std::map<std::string,
             VulkanPhysicalResourcePlan *,
             std::less<>>
        physical_resources;
    for (auto &resource : automatic_plan.resources) {
        physical_resources.emplace(
            resource.logical_resource, &resource);
    }

    for (const auto &fragment : package.resources) {
        const auto physical =
            physical_resources.find(
                fragment.logical_resource);
        const auto lowering =
            lowering_resources.find(
                fragment.logical_resource);
        if (physical == physical_resources.end() ||
            lowering == lowering_resources.end()) {
            throw std::runtime_error(
                "Vulkan physical fragment references an unknown "
                "lowered resource: " +
                fragment.logical_resource);
        }
        const auto automatic_representation =
            physical->second->representation;
        const auto automatic_format =
            physical->second->format;
        const auto exports_depth =
            automatic_plan.external_depth_export &&
            automatic_plan.external_depth_export
                    ->source_resource ==
                fragment.logical_resource;
        if (fragment.representation) {
            validateRepresentation(
                *lowering->second,
                automatic_representation,
                *fragment.representation);
            physical->second->representation =
                *fragment.representation;
        }
        if (fragment.format) {
            (void)requireFormatCandidate(
                *lowering->second, *fragment.format);
            if (*fragment.format != automatic_format) {
                if (physical->second->representation !=
                    VulkanResourceRepresentation::
                        materialized_image) {
                    throw std::runtime_error(
                        "Vulkan physical fragment alternate format "
                        "requires a materialized_image "
                        "representation: " +
                        fragment.logical_resource);
                }
                (void)requireAlternateFormatCapability(
                    indexed_format_capabilities,
                    *physical->second,
                    *fragment.format,
                    exports_depth);
            }
            physical->second->format =
                *fragment.format;
            if (*fragment.format != automatic_format &&
                automatic_plan.sample_count_plan) {
                for (auto &resolved :
                     automatic_plan.sample_count_plan
                         ->resources) {
                    if (resolved.resource ==
                        fragment.logical_resource) {
                        resolved.format =
                            *fragment.format;
                    }
                }
            }
        }

        physical->second->stored =
            physical->second->representation ==
                VulkanResourceRepresentation::external ||
            isMaterialized(
                physical->second->representation);
        physical->second->aliasable =
            isMaterialized(
                physical->second->representation) &&
            physical->second->representation !=
                VulkanResourceRepresentation::external &&
            lowering->second->pattern.allow_alias &&
            !lowering->second->pattern.require_store &&
            !exports_depth &&
            lowering->second->logical.materialization !=
                LogicalMaterializationRequirement::external &&
            lowering->second->uses.widest_read !=
                LogicalReadFootprintKind::temporal &&
            physical->second->rasterization_samples == 1;
        physical->second->required_physical_features =
            resourceFeatures(
                *lowering->second,
                *physical->second,
                exports_depth);
        lowering->second
            ->required_physical_features =
            physical->second
                ->required_physical_features;
        for (const auto &feature :
             physical->second
                 ->required_physical_features) {
            if (!endpoint_capabilities.contains(feature)) {
                throw std::runtime_error(
                    "Vulkan physical fragment resource requires an "
                    "endpoint capability that is unavailable: " +
                    fragment.logical_resource + " -> " +
                    feature);
            }
        }
        if (automatic_representation !=
                physical->second->representation ||
            automatic_format != physical->second->format) {
            physical->second->reason =
                "user-authored physical fragment override verified "
                "against the automatic plan";
            automatic_plan.decisions.push_back(
                PlanningDecision{
                    "pelican.plan.physical_resource_fragment@1",
                    fragment.logical_resource,
                    std::string{
                        vulkanResourceRepresentationName(
                            physical->second
                                ->representation)} +
                        ":" + physical->second->format,
                    "same-layer fragment passed conservative "
                    "representation/format verification",
                });
        }
        if (exports_depth) {
            automatic_plan.external_depth_export->format =
                physical->second->format;
        }
    }

    if (package.scopes) {
        applyScopePartition(
            canonical_graph, automatic_plan,
            *package.scopes,
            package.scope_edit_mode);
        automatic_plan.decisions.push_back(
            PlanningDecision{
                package.scope_edit_mode ==
                        VulkanPhysicalScopeEditMode::
                            dependency_safe
                    ? "pelican.plan.physical_scope_dependency_safe@1"
                    : "pelican.plan.physical_scope_fragment@1",
                automatic_plan.graph,
                std::to_string(
                    automatic_plan.scopes.size()),
                package.scope_edit_mode ==
                        VulkanPhysicalScopeEditMode::
                            dependency_safe
                    ? "same-layer fragment supplied a dependency-safe "
                      "scope order with compatible rendering fusion"
                    : "same-layer fragment supplied a verified split-only "
                      "scope partition",
            });
    }
    validateScopeResourceBoundaries(
        canonical_graph, automatic_plan);

    if (package.alias_groups) {
        automatic_plan.alias_groups.clear();
        automatic_plan.alias_groups.reserve(
            package.alias_groups->size());
        for (const auto &group :
             *package.alias_groups) {
            automatic_plan.alias_groups.push_back(
                VulkanAliasGroupPlan{
                    .id = group.id,
                    .resources = group.resources,
                });
        }
        automatic_plan.decisions.push_back(
            PlanningDecision{
                "pelican.plan.physical_alias_fragment@1",
                automatic_plan.graph,
                std::to_string(
                    automatic_plan.alias_groups.size()),
                "same-layer fragment supplied alias groups for "
                "lifetime/compatibility verification",
            });
    }
    validateAliasGroups(automatic_plan);

    if (package.attachments) {
        std::map<
            AttachmentIdentity,
            VulkanPhysicalAttachmentPlan *>
            attachments;
        std::map<
            AttachmentIdentity,
            VulkanPhysicalAttachmentPlan>
            automatic_attachments;
        for (auto &attachment :
             automatic_plan.attachments) {
            const AttachmentIdentity identity{
                attachment.node,
                attachment.logical_resource};
            attachments.emplace(
                identity, &attachment);
            automatic_attachments.emplace(
                identity, attachment);
        }

        bool changed = false;
        for (const auto &fragment :
             *package.attachments) {
            const AttachmentIdentity identity{
                fragment.node,
                fragment.logical_resource};
            const auto found =
                attachments.find(identity);
            if (found == attachments.end()) {
                throw std::runtime_error(
                    "Vulkan physical attachment fragment references "
                    "an unknown node/resource attachment: " +
                    fragment.node + " -> " +
                    fragment.logical_resource);
            }
            const auto resource =
                physical_resources.find(
                    fragment.logical_resource);
            if (resource ==
                physical_resources.end()) {
                throw std::runtime_error(
                    "Vulkan physical attachment fragment references "
                    "an unknown physical resource: " +
                    fragment.logical_resource);
            }
            const auto original =
                automatic_attachments.at(identity);
            const auto requested_load =
                fragment.load_op.value_or(
                    found->second->load_op);
            const auto requested_store =
                fragment.store_op.value_or(
                    found->second->store_op);
            const auto entry_changed =
                requested_load !=
                    original.load_op ||
                requested_store !=
                    original.store_op;
            if (entry_changed &&
                resource->second->representation !=
                    VulkanResourceRepresentation::
                        materialized_image &&
                resource->second->representation !=
                    VulkanResourceRepresentation::
                        external) {
                throw std::runtime_error(
                    "Vulkan physical attachment edits currently "
                    "require a materialized_image or external "
                    "resource: " +
                    fragment.logical_resource);
            }
            found->second->load_op =
                requested_load;
            found->second->store_op =
                requested_store;
            changed = changed || entry_changed;
        }

        validateVulkanPhysicalAttachmentPlans(
            canonical_graph,
            automatic_plan.resources,
            automatic_plan.attachments);

        for (const auto &fragment :
             *package.attachments) {
            const AttachmentIdentity identity{
                fragment.node,
                fragment.logical_resource};
            const auto &original =
                automatic_attachments.at(identity);
            const auto &linked =
                *attachments.at(identity);
            if (original.store_op !=
                    VulkanPhysicalAttachmentStoreOp::
                        store ||
                linked.store_op !=
                    VulkanPhysicalAttachmentStoreOp::
                        discard) {
                continue;
            }
            const auto &resource =
                *physical_resources
                     .at(fragment.logical_resource);
            if (!resource.resolve_required) {
                throw std::runtime_error(
                    "Vulkan physical attachment Store may only be "
                    "discarded when a separate multisample resolve "
                    "preserves the logical value: " +
                    fragment.node + " -> " +
                    fragment.logical_resource);
            }
            if (hasDownstreamAttachmentLoad(
                    canonical_graph,
                    automatic_plan.attachments,
                    fragment.node,
                    fragment.logical_resource)) {
                throw std::runtime_error(
                    "Vulkan physical attachment Store cannot be "
                    "discarded because a downstream attachment "
                    "loads the multisample surface: " +
                    fragment.node + " -> " +
                    fragment.logical_resource);
            }
        }
        if (changed) {
            automatic_plan.decisions.push_back(
                PlanningDecision{
                    "pelican.plan.physical_attachment_fragment@1",
                    automatic_plan.graph,
                    std::to_string(
                        package.attachments->size()),
                    "same-layer fragment supplied verified "
                    "per-attachment load/store operations",
                });
        }
    }

    validateMaterializedRenderingScopeOperations(
        automatic_plan);

    automatic_plan.required_physical_features.clear();
    for (const auto &resource :
         automatic_plan.resources) {
        automatic_plan.required_physical_features.insert(
            automatic_plan.required_physical_features.end(),
            resource.required_physical_features.begin(),
            resource.required_physical_features.end());
    }
    for (const auto &node :
         automatic_plan.lowering_graph.nodes) {
        automatic_plan.required_physical_features.insert(
            automatic_plan.required_physical_features.end(),
            node.required_physical_features.begin(),
            node.required_physical_features.end());
    }
    sortAndUniqueCapabilities(
        automatic_plan.required_physical_features,
        "linked Vulkan physical plan feature");
    const auto selected_probe = std::find_if(
        automatic_plan.backend_selection
            .candidates.begin(),
        automatic_plan.backend_selection
            .candidates.end(),
        [&](const BackendProbeResult &candidate) {
            return candidate.candidate ==
                   automatic_plan.backend_selection
                       .selected_candidate;
        });
    if (selected_probe ==
        automatic_plan.backend_selection
            .candidates.end()) {
        throw std::runtime_error(
            "Vulkan physical fragment cannot close the selected "
            "backend probe");
    }
    // The automatic probe declared the requirements of the automatic
    // lowering. Direct physical edits may select another declared format
    // or conservatively materialize a resource. Every resulting feature was
    // checked against the same endpoint above, so close the immutable
    // selected candidate over that verified set without selecting a
    // different backend.
    selected_probe->required_physical_features =
        automatic_plan.required_physical_features;
    automatic_plan.backend_selection.decisions.push_back(
        PlanningDecision{
            "pelican.plan.physical_fragment_feature_closure@1",
            automatic_plan.graph,
            automatic_plan.backend_selection
                .selected_candidate,
            "same backend candidate was closed over the "
            "fragment's endpoint-verified physical features",
        });
    validateVulkanPhysicalFeatureClosure(
        *selected_probe,
        automatic_plan.required_physical_features);

    automatic_plan.applied_fragment_package =
        std::move(package);
    return automatic_plan;
}

} // namespace Pelican
