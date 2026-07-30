#include "vulkancompletephysicalplan.hpp"

#include "imagesubresourcejson.hpp"
#include "stablefingerprint.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Pelican {

std::string_view vulkanNativeScopeSynchronizationModeName(
    VulkanNativeScopeSynchronizationMode mode) {
    switch (mode) {
    case VulkanNativeScopeSynchronizationMode::automatic:
        return "automatic";
    case VulkanNativeScopeSynchronizationMode::manual:
        return "manual";
    case VulkanNativeScopeSynchronizationMode::unchecked:
        return "unchecked";
    }
    throw std::runtime_error(
        "unknown Vulkan NativeScope synchronization mode");
}

std::string_view vulkanNativeScopeResourceOwnershipName(
    VulkanNativeScopeResourceOwnership ownership) {
    switch (ownership) {
    case VulkanNativeScopeResourceOwnership::engine:
        return "engine";
    case VulkanNativeScopeResourceOwnership::native_scope:
        return "native_scope";
    }
    throw std::runtime_error(
        "unknown Vulkan NativeScope resource ownership");
}

namespace {

constexpr std::string_view kSchema =
    "pelican.vulkan_complete_physical_plan";
constexpr std::uint32_t kVersion = 1;

void require(
    bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

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
    require(
        object.is_object(),
        std::string{context} + " must be an object");
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

std::string requireString(
    const nlohmann::json &object,
    std::string_view key, std::string_view context,
    bool allow_empty = false) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() ||
        (!allow_empty &&
         found->get_ref<const std::string &>().empty())) {
        throw std::runtime_error(
            std::string{context} + " requires " +
            (allow_empty ? std::string{} : "non-empty ") +
            "string " + std::string{key});
    }
    return found->get<std::string>();
}

bool requireBool(
    const nlohmann::json &object,
    std::string_view key, std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_boolean()) {
        throw std::runtime_error(
            std::string{context} +
            " requires boolean " + std::string{key});
    }
    return found->get<bool>();
}

template <typename Integer>
Integer requireUnsigned(
    const nlohmann::json &object,
    std::string_view key, std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() ||
        (!found->is_number_unsigned() &&
         !found->is_number_integer()) ||
        (!found->is_number_unsigned() &&
         found->get<std::int64_t>() < 0)) {
        throw std::runtime_error(
            std::string{context} +
            " requires non-negative integer " +
            std::string{key});
    }
    const auto value = found->get<std::uint64_t>();
    if (value >
        static_cast<std::uint64_t>(
            std::numeric_limits<Integer>::max())) {
        throw std::runtime_error(
            std::string{context} + " " +
            std::string{key} + " is out of range");
    }
    return static_cast<Integer>(value);
}

float requireFloat(
    const nlohmann::json &object,
    std::string_view key, std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) {
        throw std::runtime_error(
            std::string{context} +
            " requires number " + std::string{key});
    }
    const auto value = found->get<float>();
    if (!std::isfinite(value)) {
        throw std::runtime_error(
            std::string{context} + " " +
            std::string{key} + " must be finite");
    }
    return value;
}

std::vector<std::string> requireStrings(
    const nlohmann::json &object,
    std::string_view key, std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array()) {
        throw std::runtime_error(
            std::string{context} +
            " requires array " + std::string{key});
    }
    std::vector<std::string> result;
    result.reserve(found->size());
    for (const auto &entry : *found) {
        if (!entry.is_string() ||
            entry.get_ref<const std::string &>().empty()) {
            throw std::runtime_error(
                std::string{context} + " " +
                std::string{key} +
                " must contain non-empty strings");
        }
        result.push_back(entry.get<std::string>());
    }
    return result;
}

template <typename Range>
void requireUnique(
    const Range &values, std::string_view subject) {
    std::set<std::string, std::less<>> seen;
    for (const auto &value : values) {
        requireNonEmpty(value, subject);
        if (!seen.insert(value).second) {
            throw std::runtime_error(
                std::string{subject} +
                " contains duplicate '" + value + "'");
        }
    }
}

void canonicalizeNames(
    std::vector<std::string> &values,
    std::string_view subject,
    bool versioned = false) {
    requireUnique(values, subject);
    if (versioned) {
        for (const auto &value : values) {
            requireVersionedName(value, subject);
        }
    }
    std::sort(values.begin(), values.end());
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
        "complete physical resource has unknown representation '" +
        std::string{value} + "'");
}

LogicalReadFootprintKind parseReadFootprint(
    std::string_view value) {
    if (value == "none") return LogicalReadFootprintKind::none;
    if (value == "same_pixel") {
        return LogicalReadFootprintKind::same_pixel;
    }
    if (value == "neighborhood") {
        return LogicalReadFootprintKind::neighborhood;
    }
    if (value == "arbitrary") {
        return LogicalReadFootprintKind::arbitrary;
    }
    if (value == "temporal") {
        return LogicalReadFootprintKind::temporal;
    }
    throw std::runtime_error(
        "complete physical resource has unknown widest_read '" +
        std::string{value} + "'");
}

VulkanResourceViewLayout parseViewLayout(
    std::string_view value) {
    if (value == "shared_2d") {
        return VulkanResourceViewLayout::shared_2d;
    }
    if (value == "sequential_2d") {
        return VulkanResourceViewLayout::sequential_2d;
    }
    if (value == "layered_2d_array") {
        return VulkanResourceViewLayout::layered_2d_array;
    }
    if (value == "family_2d_array") {
        return VulkanResourceViewLayout::family_2d_array;
    }
    throw std::runtime_error(
        "complete physical resource has unknown view_layout '" +
        std::string{value} + "'");
}

ImageResourceDimension parseDimension(
    std::string_view value) {
    if (value == "2d") return ImageResourceDimension::two_d;
    if (value == "cube") return ImageResourceDimension::cube;
    throw std::runtime_error(
        "complete physical resource has unknown dimension '" +
        std::string{value} + "'");
}

ResourceExtentKind parseExtentKind(
    std::string_view value) {
    if (value == "output_relative") {
        return ResourceExtentKind::output_relative;
    }
    if (value == "fixed") return ResourceExtentKind::fixed;
    throw std::runtime_error(
        "complete physical resource has unknown extent kind '" +
        std::string{value} + "'");
}

VulkanPhysicalScopeKind parseScopeKind(
    std::string_view value) {
    if (value == "rendering") {
        return VulkanPhysicalScopeKind::rendering;
    }
    if (value == "compute") {
        return VulkanPhysicalScopeKind::compute;
    }
    if (value == "transfer") {
        return VulkanPhysicalScopeKind::transfer;
    }
    if (value == "output") {
        return VulkanPhysicalScopeKind::output;
    }
    if (value == "marker") {
        return VulkanPhysicalScopeKind::marker;
    }
    throw std::runtime_error(
        "complete physical scope has unknown kind '" +
        std::string{value} + "'");
}

VulkanScopeViewExecution parseViewExecution(
    std::string_view value) {
    if (value == "single_view") {
        return VulkanScopeViewExecution::single_view;
    }
    if (value == "sequential") {
        return VulkanScopeViewExecution::sequential;
    }
    if (value == "multiview") {
        return VulkanScopeViewExecution::multiview;
    }
    throw std::runtime_error(
        "complete physical scope has unknown view_execution '" +
        std::string{value} + "'");
}

VulkanPhysicalAttachmentAspect parseAttachmentAspect(
    std::string_view value) {
    if (value == "color") {
        return VulkanPhysicalAttachmentAspect::color;
    }
    if (value == "depth") {
        return VulkanPhysicalAttachmentAspect::depth;
    }
    throw std::runtime_error(
        "complete physical attachment has unknown aspect '" +
        std::string{value} + "'");
}

VulkanPhysicalAttachmentLoadOp parseLoadOp(
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
        "complete physical attachment has unknown load_op '" +
        std::string{value} + "'");
}

VulkanPhysicalAttachmentStoreOp parseStoreOp(
    std::string_view value) {
    if (value == "store") {
        return VulkanPhysicalAttachmentStoreOp::store;
    }
    if (value == "discard") {
        return VulkanPhysicalAttachmentStoreOp::discard;
    }
    throw std::runtime_error(
        "complete physical attachment has unknown store_op '" +
        std::string{value} + "'");
}

LogicalAccessMode parseAccessMode(
    std::string_view value) {
    if (value == "read") return LogicalAccessMode::read;
    if (value == "write") return LogicalAccessMode::write;
    if (value == "read_write") {
        return LogicalAccessMode::read_write;
    }
    throw std::runtime_error(
        "NativeScope resource has unknown access '" +
        std::string{value} + "'");
}

VulkanNativeScopeSynchronizationMode parseSynchronization(
    std::string_view value) {
    if (value == "automatic") {
        return VulkanNativeScopeSynchronizationMode::automatic;
    }
    if (value == "manual") {
        return VulkanNativeScopeSynchronizationMode::manual;
    }
    if (value == "unchecked") {
        return VulkanNativeScopeSynchronizationMode::unchecked;
    }
    throw std::runtime_error(
        "NativeScope has unknown synchronization mode '" +
        std::string{value} + "'");
}

VulkanNativeScopeResourceOwnership parseOwnership(
    std::string_view value) {
    if (value == "engine") {
        return VulkanNativeScopeResourceOwnership::engine;
    }
    if (value == "native_scope") {
        return VulkanNativeScopeResourceOwnership::native_scope;
    }
    throw std::runtime_error(
        "NativeScope resource has unknown ownership '" +
        std::string{value} + "'");
}

nlohmann::ordered_json resourceToJson(
    const VulkanPhysicalResourcePlan &resource) {
    nlohmann::ordered_json lifetime{
        {"used", resource.lifetime.used},
    };
    if (resource.lifetime.used) {
        lifetime["first_use"] = resource.lifetime.first_use;
        lifetime["last_use"] = resource.lifetime.last_use;
    }
    nlohmann::ordered_json result{
        {"logical_resource", resource.logical_resource},
        {"pattern", resource.pattern},
        {"format", resource.format},
        {"representation",
         vulkanResourceRepresentationName(
             resource.representation)},
        {"widest_read",
         logicalReadFootprintKindName(resource.widest_read)},
        {"lifetime", std::move(lifetime)},
        {"stored", resource.stored},
        {"aliasable", resource.aliasable},
        {"required_physical_features",
         resource.required_physical_features},
        {"reason", resource.reason},
        {"rasterization_samples",
         resource.rasterization_samples},
        {"resolve_required", resource.resolve_required},
        {"view_layout",
         vulkanResourceViewLayoutName(resource.view_layout)},
        {"mip_levels",
         nlohmann::ordered_json{
             {"mode",
              imageMipLevelModeName(
                  resource.mip_levels.mode)},
             {"count", resource.mip_levels.count},
         }},
        {"array_layers", resource.array_layers},
        {"dimension",
         imageResourceDimensionName(resource.dimension)},
        {"extent", nullptr},
    };
    if (resource.extent) {
        result["extent"] = nlohmann::ordered_json{
            {"kind",
             resourceExtentKindName(resource.extent->kind)},
            {"scale_x", resource.extent->scale_x},
            {"scale_y", resource.extent->scale_y},
            {"width", resource.extent->width},
            {"height", resource.extent->height},
        };
    }
    return result;
}

VulkanPhysicalResourcePlan resourceFromJson(
    const nlohmann::json &entry,
    std::size_t index) {
    const auto context =
        "complete physical resource[" +
        std::to_string(index) + "]";
    requireOnlyKeys(
        entry,
        {"logical_resource", "pattern", "format",
         "representation", "widest_read", "lifetime",
         "stored", "aliasable",
         "required_physical_features", "reason",
         "rasterization_samples", "resolve_required",
         "view_layout", "mip_levels", "array_layers",
         "dimension", "extent"},
        context);
    const auto lifetime = entry.find("lifetime");
    require(
        lifetime != entry.end() && lifetime->is_object(),
        context + " requires object lifetime");
    requireOnlyKeys(
        *lifetime, {"used", "first_use", "last_use"},
        context + " lifetime");
    TargetResourceLifetime parsed_lifetime{
        .used = requireBool(
            *lifetime, "used", context + " lifetime"),
    };
    if (parsed_lifetime.used) {
        parsed_lifetime.first_use =
            requireUnsigned<std::size_t>(
                *lifetime, "first_use",
                context + " lifetime");
        parsed_lifetime.last_use =
            requireUnsigned<std::size_t>(
                *lifetime, "last_use",
                context + " lifetime");
    } else if (lifetime->contains("first_use") ||
               lifetime->contains("last_use")) {
        throw std::runtime_error(
            context +
            " unused lifetime must omit first_use/last_use");
    }

    const auto mip_levels = entry.find("mip_levels");
    require(
        mip_levels != entry.end() &&
            mip_levels->is_object(),
        context + " requires object mip_levels");
    requireOnlyKeys(
        *mip_levels, {"mode", "count"},
        context + " mip_levels");
    const auto mip_mode =
        requireString(
            *mip_levels, "mode",
            context + " mip_levels");
    ImageMipLevelCount parsed_mips;
    if (mip_mode == "fixed") {
        parsed_mips.mode = ImageMipLevelMode::fixed;
    } else if (mip_mode == "full") {
        parsed_mips.mode = ImageMipLevelMode::full_chain;
    } else {
        throw std::runtime_error(
            context + " has unknown mip-level mode '" +
            mip_mode + "'");
    }
    parsed_mips.count =
        requireUnsigned<std::uint32_t>(
            *mip_levels, "count",
            context + " mip_levels");

    std::optional<ResourceExtentPlan> parsed_extent;
    const auto extent = entry.find("extent");
    require(
        extent != entry.end(),
        context + " requires extent");
    if (!extent->is_null()) {
        requireOnlyKeys(
            *extent,
            {"kind", "scale_x", "scale_y",
             "width", "height"},
            context + " extent");
        parsed_extent = ResourceExtentPlan{
            .kind = parseExtentKind(
                requireString(
                    *extent, "kind",
                    context + " extent")),
            .scale_x = requireFloat(
                *extent, "scale_x",
                context + " extent"),
            .scale_y = requireFloat(
                *extent, "scale_y",
                context + " extent"),
            .width = requireUnsigned<std::uint32_t>(
                *extent, "width",
                context + " extent"),
            .height = requireUnsigned<std::uint32_t>(
                *extent, "height",
                context + " extent"),
        };
    }

    return VulkanPhysicalResourcePlan{
        .logical_resource =
            requireString(
                entry, "logical_resource", context),
        .pattern =
            requireString(entry, "pattern", context),
        .format =
            requireString(
                entry, "format", context, true),
        .representation =
            parseRepresentation(
                requireString(
                    entry, "representation", context)),
        .widest_read =
            parseReadFootprint(
                requireString(
                    entry, "widest_read", context)),
        .lifetime = parsed_lifetime,
        .stored =
            requireBool(entry, "stored", context),
        .aliasable =
            requireBool(entry, "aliasable", context),
        .required_physical_features =
            requireStrings(
                entry, "required_physical_features",
                context),
        .reason =
            requireString(
                entry, "reason", context, true),
        .rasterization_samples =
            requireUnsigned<std::uint32_t>(
                entry, "rasterization_samples",
                context),
        .resolve_required =
            requireBool(
                entry, "resolve_required", context),
        .view_layout =
            parseViewLayout(
                requireString(
                    entry, "view_layout", context)),
        .mip_levels = parsed_mips,
        .array_layers =
            requireUnsigned<std::uint32_t>(
                entry, "array_layers", context),
        .dimension =
            parseDimension(
                requireString(
                    entry, "dimension", context)),
        .extent = parsed_extent,
    };
}

nlohmann::ordered_json scopeToJson(
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

VulkanPhysicalScopePlan scopeFromJson(
    const nlohmann::json &entry,
    std::size_t index) {
    const auto context =
        "complete physical scope[" +
        std::to_string(index) + "]";
    requireOnlyKeys(
        entry,
        {"id", "kind", "nodes",
         "single_rendering_instance", "local_reads",
         "region_tags", "rasterization_samples",
         "view_execution", "view_count",
         "execution_count", "view_mask"},
        context);
    return VulkanPhysicalScopePlan{
        .id = requireString(entry, "id", context),
        .kind =
            parseScopeKind(
                requireString(entry, "kind", context)),
        .nodes = requireStrings(entry, "nodes", context),
        .single_rendering_instance =
            requireBool(
                entry, "single_rendering_instance",
                context),
        .local_reads =
            requireStrings(entry, "local_reads", context),
        .region_tags =
            requireStrings(entry, "region_tags", context),
        .rasterization_samples =
            requireUnsigned<std::uint32_t>(
                entry, "rasterization_samples",
                context),
        .view_execution =
            parseViewExecution(
                requireString(
                    entry, "view_execution", context)),
        .view_count =
            requireUnsigned<std::uint32_t>(
                entry, "view_count", context),
        .execution_count =
            requireUnsigned<std::uint32_t>(
                entry, "execution_count", context),
        .view_mask =
            requireUnsigned<std::uint32_t>(
                entry, "view_mask", context),
    };
}

nlohmann::ordered_json attachmentToJson(
    const VulkanPhysicalAttachmentPlan &attachment) {
    nlohmann::ordered_json result{
        {"node", attachment.node},
        {"logical_resource", attachment.logical_resource},
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

VulkanPhysicalAttachmentPlan attachmentFromJson(
    const nlohmann::json &entry,
    std::size_t index) {
    const auto context =
        "complete physical attachment[" +
        std::to_string(index) + "]";
    requireOnlyKeys(
        entry,
        {"node", "logical_resource", "subresource",
         "aspect", "load_op", "store_op"},
        context);
    return VulkanPhysicalAttachmentPlan{
        .node = requireString(entry, "node", context),
        .logical_resource =
            requireString(
                entry, "logical_resource", context),
        .subresource =
            parseOptionalImageSubresource(entry, context),
        .aspect =
            parseAttachmentAspect(
                requireString(entry, "aspect", context)),
        .load_op =
            parseLoadOp(
                requireString(entry, "load_op", context)),
        .store_op =
            parseStoreOp(
                requireString(entry, "store_op", context)),
    };
}

nlohmann::ordered_json nativeScopeToJson(
    const VulkanNativeScopeDeclaration &scope) {
    nlohmann::ordered_json resources =
        nlohmann::ordered_json::array();
    for (const auto &resource : scope.resources) {
        resources.push_back(
            nlohmann::ordered_json{
                {"logical_resource",
                 resource.logical_resource},
                {"semantic_type",
                 resource.semantic_type},
                {"access",
                 logicalAccessModeName(
                     resource.access)},
                {"ownership",
                 vulkanNativeScopeResourceOwnershipName(
                     resource.ownership)},
                {"stages", resource.stages},
                {"accesses", resource.accesses},
            });
    }
    return {
        {"scope", scope.scope},
        {"implementation", scope.implementation},
        {"queue_capability", scope.queue_capability},
        {"synchronization",
         vulkanNativeScopeSynchronizationModeName(
             scope.synchronization)},
        {"resources", std::move(resources)},
        {"alias_groups", scope.alias_groups},
        {"required_features", scope.required_features},
        {"required_extensions",
         scope.required_extensions},
        {"capture_compatible",
         scope.capture_compatible},
        {"device_loss_recoverable",
         scope.device_loss_recoverable},
        {"hot_reloadable", scope.hot_reloadable},
        {"implementation_config",
         scope.implementation_config},
    };
}

VulkanNativeScopeDeclaration nativeScopeFromJson(
    const nlohmann::json &entry,
    std::size_t index) {
    const auto context =
        "NativeScope[" + std::to_string(index) + "]";
    requireOnlyKeys(
        entry,
        {"scope", "implementation", "queue_capability",
         "synchronization", "resources",
         "alias_groups", "required_features",
         "required_extensions", "capture_compatible",
         "device_loss_recoverable", "hot_reloadable",
         "implementation_config"},
        context);
    const auto resources = entry.find("resources");
    require(
        resources != entry.end() && resources->is_array(),
        context + " requires array resources");
    std::vector<VulkanNativeScopeResourceBoundary>
        parsed_resources;
    parsed_resources.reserve(resources->size());
    for (std::size_t resource_index = 0;
         resource_index < resources->size();
         ++resource_index) {
        const auto &resource = resources->at(resource_index);
        const auto resource_context =
            context + " resource[" +
            std::to_string(resource_index) + "]";
        requireOnlyKeys(
            resource,
            {"logical_resource", "semantic_type",
             "access", "ownership", "stages",
             "accesses"},
            resource_context);
        parsed_resources.push_back(
            VulkanNativeScopeResourceBoundary{
                .logical_resource =
                    requireString(
                        resource, "logical_resource",
                        resource_context),
                .semantic_type =
                    requireString(
                        resource, "semantic_type",
                        resource_context),
                .access =
                    parseAccessMode(
                        requireString(
                            resource, "access",
                            resource_context)),
                .ownership =
                    parseOwnership(
                        requireString(
                            resource, "ownership",
                            resource_context)),
                .stages =
                    requireStrings(
                        resource, "stages",
                        resource_context),
                .accesses =
                    requireStrings(
                        resource, "accesses",
                        resource_context),
            });
    }
    const auto implementation_config =
        entry.find("implementation_config");
    require(
        implementation_config != entry.end() &&
            implementation_config->is_object(),
        context +
            " requires object implementation_config");
    return VulkanNativeScopeDeclaration{
        .scope =
            requireString(entry, "scope", context),
        .implementation =
            requireString(
                entry, "implementation", context),
        .queue_capability =
            requireString(
                entry, "queue_capability", context),
        .synchronization =
            parseSynchronization(
                requireString(
                    entry, "synchronization", context)),
        .resources = std::move(parsed_resources),
        .alias_groups =
            requireStrings(
                entry, "alias_groups", context),
        .required_features =
            requireStrings(
                entry, "required_features", context),
        .required_extensions =
            requireStrings(
                entry, "required_extensions", context),
        .capture_compatible =
            requireBool(
                entry, "capture_compatible", context),
        .device_loss_recoverable =
            requireBool(
                entry, "device_loss_recoverable",
                context),
        .hot_reloadable =
            requireBool(
                entry, "hot_reloadable", context),
        .implementation_config =
            nlohmann::ordered_json::parse(
                implementation_config->dump()),
    };
}

VulkanCompletePhysicalPlanPackage canonicalize(
    VulkanCompletePhysicalPlanPackage package) {
    require(
        package.schema_version == kVersion,
        "Vulkan complete physical plan version must be exactly 1");
    requireNonEmpty(
        package.graph,
        "Vulkan complete physical plan graph");
    requireVersionedName(
        package.backend_candidate,
        "Vulkan complete physical plan backend candidate");

    std::sort(
        package.resources.begin(), package.resources.end(),
        [](const auto &left, const auto &right) {
            return left.logical_resource <
                   right.logical_resource;
        });
    std::set<std::string, std::less<>> resource_names;
    for (auto &resource : package.resources) {
        requireNonEmpty(
            resource.logical_resource,
            "complete physical resource name");
        if (!resource_names.insert(
                resource.logical_resource).second) {
            throw std::runtime_error(
                "Vulkan complete physical plan has duplicate "
                "resource '" +
                resource.logical_resource + "'");
        }
        canonicalizeNames(
            resource.required_physical_features,
            "complete physical resource feature", true);
    }

    std::set<std::string, std::less<>> scope_ids;
    for (auto &scope : package.scopes) {
        requireNonEmpty(
            scope.id, "complete physical scope id");
        if (!scope_ids.insert(scope.id).second) {
            throw std::runtime_error(
                "Vulkan complete physical plan has duplicate "
                "scope '" +
                scope.id + "'");
        }
        requireUnique(
            scope.nodes, "complete physical scope node");
        canonicalizeNames(
            scope.local_reads,
            "complete physical scope local read");
        canonicalizeNames(
            scope.region_tags,
            "complete physical scope region tag");
    }

    std::sort(
        package.attachments.begin(),
        package.attachments.end(),
        [](const auto &left, const auto &right) {
            return std::tie(
                       left.node,
                       left.logical_resource) <
                   std::tie(
                       right.node,
                       right.logical_resource);
        });
    for (std::size_t index = 1;
         index < package.attachments.size(); ++index) {
        if (std::tie(
                package.attachments[index - 1].node,
                package.attachments[index - 1]
                    .logical_resource) ==
            std::tie(
                package.attachments[index].node,
                package.attachments[index]
                    .logical_resource)) {
            throw std::runtime_error(
                "Vulkan complete physical plan has duplicate "
                "attachment identity");
        }
    }

    std::sort(
        package.alias_groups.begin(),
        package.alias_groups.end(),
        [](const auto &left, const auto &right) {
            return left.id < right.id;
        });
    std::set<std::string, std::less<>> alias_ids;
    for (auto &group : package.alias_groups) {
        requireNonEmpty(
            group.id, "complete physical alias group id");
        if (!alias_ids.insert(group.id).second) {
            throw std::runtime_error(
                "Vulkan complete physical plan has duplicate alias "
                "group '" +
                group.id + "'");
        }
        canonicalizeNames(
            group.resources,
            "complete physical alias group resource");
    }
    canonicalizeNames(
        package.required_physical_features,
        "complete physical plan feature", true);

    std::sort(
        package.native_scopes.begin(),
        package.native_scopes.end(),
        [](const auto &left, const auto &right) {
            return left.scope < right.scope;
        });
    std::set<std::string, std::less<>> native_scope_ids;
    for (auto &scope : package.native_scopes) {
        requireNonEmpty(scope.scope, "NativeScope scope");
        if (!native_scope_ids.insert(scope.scope).second) {
            throw std::runtime_error(
                "Vulkan complete physical plan has duplicate "
                "NativeScope for '" +
                scope.scope + "'");
        }
        requireVersionedName(
            scope.implementation,
            "NativeScope implementation");
        requireVersionedName(
            scope.queue_capability,
            "NativeScope queue capability");
        canonicalizeNames(
            scope.alias_groups,
            "NativeScope alias group");
        canonicalizeNames(
            scope.required_features,
            "NativeScope required feature", true);
        canonicalizeNames(
            scope.required_extensions,
            "NativeScope required extension");
        require(
            scope.implementation_config.is_object(),
            "NativeScope implementation_config must be an object");
        std::sort(
            scope.resources.begin(),
            scope.resources.end(),
            [](const auto &left, const auto &right) {
                return left.logical_resource <
                       right.logical_resource;
            });
        std::set<std::string, std::less<>> boundary_names;
        for (auto &resource : scope.resources) {
            requireNonEmpty(
                resource.logical_resource,
                "NativeScope boundary resource");
            if (!boundary_names.insert(
                    resource.logical_resource)
                     .second) {
                throw std::runtime_error(
                    "NativeScope has duplicate resource '" +
                    resource.logical_resource + "'");
            }
            requireVersionedName(
                resource.semantic_type,
                "NativeScope resource semantic type");
            canonicalizeNames(
                resource.stages,
                "NativeScope resource stage");
            canonicalizeNames(
                resource.accesses,
                "NativeScope resource access mask");
        }
    }
    return package;
}

const BackendProbeResult &selectedProbe(
    const VulkanTargetPlan &plan) {
    const auto found = std::find_if(
        plan.backend_selection.candidates.begin(),
        plan.backend_selection.candidates.end(),
        [&](const auto &candidate) {
            return candidate.candidate ==
                   plan.backend_selection
                       .selected_candidate;
        });
    if (found == plan.backend_selection.candidates.end()) {
        throw std::runtime_error(
            "automatic Vulkan target plan has no selected probe");
    }
    return *found;
}

LogicalAccessMode mergeAccess(
    LogicalAccessMode left, LogicalAccessMode right) {
    if (left == right) return left;
    return LogicalAccessMode::read_write;
}

struct GraphResourceUse {
    LogicalAccessMode access = LogicalAccessMode::read;
    bool initialized = false;
    bool transfer_access = false;
    LogicalReadFootprintKind widest_read =
        LogicalReadFootprintKind::none;
    TargetResourceLifetime lifetime;
};

std::map<std::string, GraphResourceUse, std::less<>>
deriveUses(
    const CompiledLogicalRenderGraph &graph,
    const std::map<std::string, std::size_t, std::less<>>
        &node_order) {
    std::map<std::string, GraphResourceUse, std::less<>>
        result;
    for (const auto &resource : graph.resources) {
        result.emplace(resource.name, GraphResourceUse{});
    }
    for (const auto &node : graph.nodes) {
        const auto order = node_order.find(node.name);
        require(
            order != node_order.end(),
            "complete physical plan omits logical node '" +
                node.name + "'");
        for (const auto &use : node.uses) {
            const auto *value =
                use.input_value
                    ? &*use.input_value
                    : use.output_value
                          ? &*use.output_value
                          : nullptr;
            require(
                value != nullptr,
                "logical resource use has no input or output");
            const auto found =
                result.find(value->resource);
            require(
                found != result.end(),
                "logical node references unknown resource '" +
                    value->resource + "'");
            auto &summary = found->second;
            summary.access =
                summary.initialized
                    ? mergeAccess(
                          summary.access, use.access)
                    : use.access;
            summary.initialized = true;
            const auto reads =
                use.access == LogicalAccessMode::read ||
                use.access ==
                    LogicalAccessMode::read_write;
            if (reads) {
                summary.widest_read = std::max(
                    summary.widest_read,
                    use.footprint.kind);
            }
            summary.transfer_access =
                summary.transfer_access ||
                use.intent ==
                    LogicalAccessIntent::transfer;
            if (!summary.lifetime.used) {
                summary.lifetime = {
                    .used = true,
                    .first_use = order->second,
                    .last_use = order->second,
                };
            } else {
                summary.lifetime.first_use =
                    std::min(
                        summary.lifetime.first_use,
                        order->second);
                summary.lifetime.last_use =
                    std::max(
                        summary.lifetime.last_use,
                        order->second);
            }
        }
    }
    return result;
}

bool lifetimesOverlap(
    const TargetResourceLifetime &left,
    const TargetResourceLifetime &right) {
    return left.used && right.used &&
           !(left.last_use < right.first_use ||
             right.last_use < left.first_use);
}

bool aliasShapesMatch(
    const VulkanPhysicalResourcePlan &left,
    const VulkanPhysicalResourcePlan &right) {
    return left.representation == right.representation &&
           left.format == right.format &&
           left.rasterization_samples ==
               right.rasterization_samples &&
           left.resolve_required ==
               right.resolve_required &&
           left.view_layout == right.view_layout &&
           left.mip_levels == right.mip_levels &&
           left.array_layers == right.array_layers &&
           left.dimension == right.dimension &&
           left.extent == right.extent;
}

const VulkanPhysicalResourceFormatCapability *
findFormatCapability(
    std::span<
        const VulkanPhysicalResourceFormatCapability>
        capabilities,
    std::string_view resource, std::string_view format) {
    const auto found = std::find_if(
        capabilities.begin(), capabilities.end(),
        [&](const auto &candidate) {
            return candidate.logical_resource == resource &&
                   candidate.format == format;
        });
    return found == capabilities.end() ? nullptr : &*found;
}

} // namespace

VulkanCompletePhysicalPlanPackage
ejectVulkanCompletePhysicalPlanPackage(
    const VulkanTargetPlan &plan) {
    requireNonEmpty(
        plan.graph, "Vulkan target plan graph");
    requireVersionedName(
        plan.backend_selection.selected_candidate,
        "Vulkan target plan backend candidate");
    return canonicalize(
        VulkanCompletePhysicalPlanPackage{
            .schema_version = kVersion,
            .graph = plan.graph,
            .logical_graph_fingerprint =
                plan.logical_graph_fingerprint,
            .automatic_plan_fingerprint =
                plan.automatic_plan_fingerprint,
            .backend_candidate =
                plan.backend_selection
                    .selected_candidate,
            .resources = plan.resources,
            .scopes = plan.scopes,
            .attachments = plan.attachments,
            .alias_groups = plan.alias_groups,
            .required_physical_features =
                plan.required_physical_features,
            .external_depth_export =
                plan.external_depth_export,
        });
}

nlohmann::ordered_json
vulkanCompletePhysicalPlanPackageToJson(
    const VulkanCompletePhysicalPlanPackage &source) {
    const auto package = canonicalize(source);
    nlohmann::ordered_json result{
        {"schema", kSchema},
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
        {"scopes", nlohmann::ordered_json::array()},
        {"attachments", nlohmann::ordered_json::array()},
        {"alias_groups", nlohmann::ordered_json::array()},
        {"required_physical_features",
         package.required_physical_features},
        {"external_depth_export", nullptr},
        {"native_scopes", nlohmann::ordered_json::array()},
    };
    if (package.external_depth_export) {
        result["external_depth_export"] =
            nlohmann::ordered_json{
                {"source_resource",
                 package.external_depth_export
                     ->source_resource},
                {"format",
                 package.external_depth_export->format},
                {"view_layout",
                 vulkanResourceViewLayoutName(
                     package.external_depth_export
                         ->view_layout)},
                {"array_layers",
                 package.external_depth_export
                     ->array_layers},
                {"reason",
                 package.external_depth_export->reason},
            };
    }
    for (const auto &resource : package.resources) {
        result["resources"].push_back(
            resourceToJson(resource));
    }
    for (const auto &scope : package.scopes) {
        result["scopes"].push_back(scopeToJson(scope));
    }
    for (const auto &attachment :
         package.attachments) {
        result["attachments"].push_back(
            attachmentToJson(attachment));
    }
    for (const auto &group : package.alias_groups) {
        result["alias_groups"].push_back(
            nlohmann::ordered_json{
                {"id", group.id},
                {"resources", group.resources},
            });
    }
    for (const auto &scope : package.native_scopes) {
        result["native_scopes"].push_back(
            nativeScopeToJson(scope));
    }
    return result;
}

VulkanCompletePhysicalPlanPackage
vulkanCompletePhysicalPlanPackageFromJson(
    const nlohmann::json &document) {
    constexpr std::string_view context =
        "Vulkan complete physical plan package";
    requireOnlyKeys(
        document,
        {"schema", "version", "graph",
         "logical_graph_fingerprint",
         "automatic_plan_fingerprint",
         "backend_candidate", "resources", "scopes",
         "attachments", "alias_groups",
         "required_physical_features",
         "external_depth_export", "native_scopes"},
        context);
    if (requireString(
            document, "schema", context) != kSchema) {
        throw std::runtime_error(
            std::string{context} + " schema must be '" +
            std::string{kSchema} + "'");
    }
    const auto version =
        requireUnsigned<std::uint32_t>(
            document, "version", context);
    if (version != kVersion) {
        throw std::runtime_error(
            std::string{context} +
            " version must be exactly 1");
    }
    const auto parseFingerprint =
        [&](std::string_view field) {
            return parseStableFingerprint64(
                requireString(
                    document, field, context),
                std::string{context} + " " +
                    std::string{field});
        };
    const auto requireArray =
        [&](std::string_view field)
        -> const nlohmann::json & {
            const auto found = document.find(field);
            require(
                found != document.end() &&
                    found->is_array(),
                std::string{context} +
                    " requires array " +
                    std::string{field});
            return *found;
        };

    VulkanCompletePhysicalPlanPackage result{
        .schema_version = version,
        .graph =
            requireString(document, "graph", context),
        .logical_graph_fingerprint =
            parseFingerprint(
                "logical_graph_fingerprint"),
        .automatic_plan_fingerprint =
            parseFingerprint(
                "automatic_plan_fingerprint"),
        .backend_candidate =
            requireString(
                document, "backend_candidate", context),
        .required_physical_features =
            requireStrings(
                document,
                "required_physical_features", context),
    };
    const auto external_depth =
        document.find("external_depth_export");
    require(
        external_depth != document.end(),
        std::string{context} +
            " requires external_depth_export");
    if (!external_depth->is_null()) {
        const auto external_context =
            std::string{context} +
            " external_depth_export";
        requireOnlyKeys(
            *external_depth,
            {"source_resource", "format", "view_layout",
             "array_layers", "reason"},
            external_context);
        result.external_depth_export =
            VulkanExternalDepthExportPlan{
                .source_resource =
                    requireString(
                        *external_depth,
                        "source_resource",
                        external_context),
                .format =
                    requireString(
                        *external_depth, "format",
                        external_context),
                .view_layout =
                    parseViewLayout(
                        requireString(
                            *external_depth,
                            "view_layout",
                            external_context)),
                .array_layers =
                    requireUnsigned<std::uint32_t>(
                        *external_depth,
                        "array_layers",
                        external_context),
                .reason =
                    requireString(
                        *external_depth, "reason",
                        external_context, true),
            };
    }
    const auto &resources = requireArray("resources");
    result.resources.reserve(resources.size());
    for (std::size_t index = 0;
         index < resources.size(); ++index) {
        result.resources.push_back(
            resourceFromJson(resources.at(index), index));
    }
    const auto &scopes = requireArray("scopes");
    result.scopes.reserve(scopes.size());
    for (std::size_t index = 0;
         index < scopes.size(); ++index) {
        result.scopes.push_back(
            scopeFromJson(scopes.at(index), index));
    }
    const auto &attachments =
        requireArray("attachments");
    result.attachments.reserve(attachments.size());
    for (std::size_t index = 0;
         index < attachments.size(); ++index) {
        result.attachments.push_back(
            attachmentFromJson(
                attachments.at(index), index));
    }
    const auto &alias_groups =
        requireArray("alias_groups");
    result.alias_groups.reserve(alias_groups.size());
    for (std::size_t index = 0;
         index < alias_groups.size(); ++index) {
        const auto &entry = alias_groups.at(index);
        const auto alias_context =
            "complete physical alias group[" +
            std::to_string(index) + "]";
        requireOnlyKeys(
            entry, {"id", "resources"},
            alias_context);
        result.alias_groups.push_back(
            VulkanAliasGroupPlan{
                .id =
                    requireString(
                        entry, "id", alias_context),
                .resources =
                    requireStrings(
                        entry, "resources",
                        alias_context),
            });
    }
    const auto &native_scopes =
        requireArray("native_scopes");
    result.native_scopes.reserve(native_scopes.size());
    for (std::size_t index = 0;
         index < native_scopes.size(); ++index) {
        result.native_scopes.push_back(
            nativeScopeFromJson(
                native_scopes.at(index), index));
    }
    return canonicalize(std::move(result));
}

std::uint64_t vulkanCompletePhysicalPlanPackageFingerprint(
    const VulkanCompletePhysicalPlanPackage &package) {
    StableFingerprint64 fingerprint;
    fingerprint.appendString(
        "pelican.vulkan_complete_physical_plan.fingerprint@1");
    fingerprint.appendString(
        vulkanCompletePhysicalPlanPackageToJson(
            package)
            .dump());
    return fingerprint.value();
}

VerifiedVulkanCompletePhysicalPlanPackage
verifyVulkanCompletePhysicalPlanPackage(
    const CompiledLogicalRenderGraph &canonical_graph,
    const TargetTopologySnapshot &source_topology,
    const VulkanTargetPlan &automatic_plan,
    VulkanCompletePhysicalPlanPackage source_package,
    std::span<
        const VulkanPhysicalResourceFormatCapability>
        format_capabilities,
    std::span<const std::string> enabled_extensions) {
    auto package = canonicalize(
        std::move(source_package));
    require(
        package.graph == canonical_graph.name &&
            automatic_plan.graph == canonical_graph.name,
        "Vulkan complete physical plan graph mismatch");
    const auto logical_fingerprint =
        vulkanTargetPlanLogicalGraphFingerprint(
            canonical_graph);
    require(
        package.logical_graph_fingerprint ==
                logical_fingerprint &&
            automatic_plan.logical_graph_fingerprint ==
                logical_fingerprint,
        "Vulkan complete physical plan is stale for the logical "
        "graph");
    require(
        package.backend_candidate ==
            automatic_plan.backend_selection
                .selected_candidate,
        "Vulkan complete physical plan backend candidate "
        "mismatch");

    const auto topology =
        canonicalizeTargetTopology(source_topology);
    const auto automatic_fingerprint =
        vulkanAutomaticTargetPlanFingerprint(
            topology, automatic_plan);
    require(
        automatic_plan.automatic_plan_fingerprint ==
            automatic_fingerprint,
        "automatic Vulkan target plan fingerprint is "
        "inconsistent");
    require(
        package.automatic_plan_fingerprint ==
            automatic_fingerprint,
        "Vulkan complete physical plan is stale for the target "
        "environment");

    const auto &probe = selectedProbe(automatic_plan);
    const auto *endpoint =
        findTargetEndpoint(topology, probe.endpoint);
    require(
        endpoint != nullptr,
        "Vulkan complete physical plan selected endpoint is "
        "absent");
    const std::set<std::string, std::less<>>
        endpoint_capabilities(
            endpoint->capabilities.begin(),
            endpoint->capabilities.end());
    const std::set<std::string, std::less<>>
        plan_features(
            package.required_physical_features.begin(),
            package.required_physical_features.end());
    const auto requirePlanFeature =
        [&](std::string_view feature,
            std::string_view subject) {
            require(
                plan_features.contains(
                    std::string{feature}),
                "Vulkan complete physical plan omits required "
                "feature '" +
                    std::string{feature} + "' for " +
                    std::string{subject});
        };
    for (const auto &feature :
         package.required_physical_features) {
        require(
            endpoint_capabilities.contains(feature),
            "Vulkan complete physical plan requires unavailable "
            "endpoint capability '" +
                feature + "'");
    }
    for (const auto &node : canonical_graph.nodes) {
        switch (node.kind) {
        case LogicalGraphNodeKind::render:
        case LogicalGraphNodeKind::anchor:
        case LogicalGraphNodeKind::output_transform:
            requirePlanFeature(
                "pelican.vulkan.graphics@1",
                node.name);
            break;
        case LogicalGraphNodeKind::compute:
            requirePlanFeature(
                "pelican.vulkan.storage_buffer@1",
                node.name);
            break;
        case LogicalGraphNodeKind::snapshot_copy:
            requirePlanFeature(
                "pelican.vulkan.transfer_copy@1",
                node.name);
            break;
        }
    }

    std::map<std::string, std::size_t, std::less<>>
        node_order;
    std::map<std::string,
             const LogicalGraphNode *, std::less<>>
        logical_nodes;
    for (const auto &node : canonical_graph.nodes) {
        logical_nodes.emplace(node.name, &node);
    }
    std::map<std::string,
             const VulkanPhysicalScopePlan *, std::less<>>
        scopes;
    std::size_t order = 0;
    for (const auto &scope : package.scopes) {
        require(
            !scope.nodes.empty(),
            "Vulkan complete physical scope '" + scope.id +
                "' has no nodes");
        require(
            scope.rasterization_samples != 0 &&
                std::has_single_bit(
                    scope.rasterization_samples),
            "Vulkan complete physical scope '" + scope.id +
                "' has an invalid sample count");
        require(
            scope.view_count != 0 &&
                scope.execution_count != 0,
            "Vulkan complete physical scope '" + scope.id +
                "' has an empty view execution");
        require(
            !scope.single_rendering_instance ||
                scope.kind ==
                    VulkanPhysicalScopeKind::rendering,
            "only a rendering scope may select one rendering "
            "instance");
        switch (scope.view_execution) {
        case VulkanScopeViewExecution::single_view:
            require(
                scope.view_count == 1 &&
                    scope.execution_count == 1 &&
                    scope.view_mask == 0,
                "single-view scope '" + scope.id +
                    "' has an invalid execution shape");
            break;
        case VulkanScopeViewExecution::sequential:
            require(
                scope.execution_count ==
                        scope.view_count &&
                    scope.view_mask == 0,
                "sequential scope '" + scope.id +
                    "' must execute once per view");
            break;
        case VulkanScopeViewExecution::multiview:
            require(
                scope.view_mask != 0 &&
                    scope.execution_count == 1 &&
                    static_cast<std::uint32_t>(
                        std::popcount(
                            scope.view_mask)) ==
                        scope.view_count,
                "multiview scope '" + scope.id +
                    "' requires one mask bit per view and one "
                    "execution");
            require(
                plan_features.contains(
                    std::string{
                        vulkanMultiviewCapability}),
                "multiview scope is missing the multiview "
                "physical feature");
            break;
        }
        scopes.emplace(scope.id, &scope);
        for (const auto &node : scope.nodes) {
            const auto logical =
                logical_nodes.find(node);
            require(
                logical != logical_nodes.end(),
                "Vulkan complete physical scope references "
                "unknown node '" +
                    node + "'");
            VulkanPhysicalScopeKind expected_kind =
                VulkanPhysicalScopeKind::marker;
            switch (logical->second->kind) {
            case LogicalGraphNodeKind::render:
                expected_kind =
                    VulkanPhysicalScopeKind::rendering;
                break;
            case LogicalGraphNodeKind::compute:
                expected_kind =
                    VulkanPhysicalScopeKind::compute;
                break;
            case LogicalGraphNodeKind::snapshot_copy:
                expected_kind =
                    VulkanPhysicalScopeKind::transfer;
                break;
            case LogicalGraphNodeKind::output_transform:
                expected_kind =
                    VulkanPhysicalScopeKind::output;
                break;
            case LogicalGraphNodeKind::anchor:
                expected_kind =
                    VulkanPhysicalScopeKind::marker;
                break;
            }
            require(
                scope.kind == expected_kind,
                "Vulkan complete physical scope kind does not "
                "match logical node '" +
                    node + "'");
            if (scope.kind !=
                VulkanPhysicalScopeKind::rendering) {
                require(
                    scope.rasterization_samples == 1,
                    "non-rendering physical scope must use one "
                    "sample: " +
                        scope.id);
            }
            if (!node_order.emplace(node, order++).second) {
                throw std::runtime_error(
                    "Vulkan complete physical plan executes node '" +
                    node + "' more than once");
            }
        }
    }
    require(
        node_order.size() == canonical_graph.nodes.size(),
        "Vulkan complete physical plan node coverage is "
        "incomplete");
    for (const auto &node : canonical_graph.nodes) {
        require(
            node_order.contains(node.name),
            "Vulkan complete physical plan omits node '" +
                node.name + "'");
    }
    for (const auto &edge :
         deriveLogicalDataEdges(canonical_graph)) {
        require(
            node_order.at(edge.producer_node) <
                node_order.at(edge.consumer_node),
            "Vulkan complete physical plan reverses logical data "
            "dependency '" +
                edge.producer_node + " -> " +
                edge.consumer_node + "'");
    }
    for (const auto &node : canonical_graph.nodes) {
        for (const auto &after : node.after) {
            require(
                node_order.contains(after) &&
                    node_order.at(after) <
                        node_order.at(node.name),
                "Vulkan complete physical plan violates after "
                "dependency '" +
                    after + " -> " + node.name + "'");
        }
        for (const auto &before : node.before) {
            require(
                node_order.contains(before) &&
                    node_order.at(node.name) <
                        node_order.at(before),
                "Vulkan complete physical plan violates before "
                "dependency '" +
                    node.name + " -> " + before + "'");
        }
    }

    auto graph_uses =
        deriveUses(canonical_graph, node_order);
    if (!node_order.empty()) {
        const auto terminal = node_order.size() - 1;
        for (const auto &lowering :
             automatic_plan.lowering_graph.resources) {
            if (lowering.logical.materialization !=
                    LogicalMaterializationRequirement::
                        required &&
                lowering.logical.materialization !=
                    LogicalMaterializationRequirement::
                        external &&
                !lowering.pattern.require_store) {
                continue;
            }
            const auto use =
                graph_uses.find(
                    lowering.logical.name);
            if (use != graph_uses.end() &&
                use->second.lifetime.used) {
                use->second.lifetime.last_use =
                    terminal;
            }
        }
    }
    std::map<std::string,
             const LogicalResourceDesc *, std::less<>>
        logical_resources;
    for (const auto &resource :
         canonical_graph.resources) {
        logical_resources.emplace(resource.name, &resource);
    }
    std::map<std::string,
             const VulkanPhysicalResourcePlan *,
             std::less<>>
        automatic_resources;
    for (const auto &resource :
         automatic_plan.resources) {
        automatic_resources.emplace(
            resource.logical_resource, &resource);
    }
    require(
        package.resources.size() ==
            automatic_resources.size(),
        "Vulkan complete physical plan must replace every "
        "engine-visible physical resource exactly once");
    std::map<std::string,
             const VulkanPhysicalResourcePlan *,
             std::less<>>
        physical_resources;
    for (const auto &resource : package.resources) {
        const auto automatic =
            automatic_resources.find(
                resource.logical_resource);
        require(
            automatic != automatic_resources.end(),
            "Vulkan complete physical plan references unknown "
            "engine-visible resource '" +
                resource.logical_resource + "'");
        const auto logical =
            logical_resources.find(
                resource.logical_resource);
        require(
            logical != logical_resources.end(),
            "Vulkan complete physical resource has no logical "
            "provenance: " +
                resource.logical_resource);
        const auto use =
            graph_uses.find(resource.logical_resource);
        require(
            use != graph_uses.end(),
            "Vulkan complete physical resource has no logical use "
            "summary: " +
                resource.logical_resource);
        if (resource.lifetime != use->second.lifetime ||
            resource.widest_read !=
                use->second.widest_read) {
            throw std::runtime_error(
                "Vulkan complete physical resource has stale "
                "lifetime or read-footprint metadata: " +
                resource.logical_resource + " (authored " +
                std::to_string(
                    resource.lifetime.first_use) +
                ".." +
                std::to_string(
                    resource.lifetime.last_use) +
                ", recomputed " +
                std::to_string(
                    use->second.lifetime.first_use) +
                ".." +
                std::to_string(
                    use->second.lifetime.last_use) +
                ", authored footprint " +
                std::string{
                    logicalReadFootprintKindName(
                        resource.widest_read)} +
                ", recomputed footprint " +
                std::string{
                    logicalReadFootprintKindName(
                        use->second.widest_read)} +
                ")");
        }
        require(
            resource.pattern ==
                automatic->second->pattern,
            "Vulkan complete physical resource cannot rewrite its "
            "logical ResourcePattern identity: " +
                resource.logical_resource);
        require(
            resource.rasterization_samples != 0 &&
                std::has_single_bit(
                    resource.rasterization_samples),
            "Vulkan complete physical resource has an invalid "
            "sample count: " +
                resource.logical_resource);
        require(
            resource.array_layers != 0,
            "Vulkan complete physical resource has zero array "
            "layers: " +
                resource.logical_resource);
        require(
            resource.mip_levels.mode ==
                    ImageMipLevelMode::full_chain ||
                resource.mip_levels.count != 0,
            "Vulkan complete physical resource has zero mip "
            "levels: " +
                resource.logical_resource);
        if (resource.dimension ==
            ImageResourceDimension::cube) {
            require(
                resource.array_layers >= 6 &&
                    resource.array_layers % 6 == 0,
                "cube physical resource requires array layers in "
                "multiples of six: " +
                    resource.logical_resource);
        }
        if (resource.extent) {
            if (resource.extent->kind ==
                ResourceExtentKind::fixed) {
                require(
                    resource.extent->width != 0 &&
                        resource.extent->height != 0,
                    "fixed physical extent must be non-zero: " +
                        resource.logical_resource);
            } else {
                require(
                    resource.extent->scale_x > 0.0f &&
                        resource.extent->scale_y > 0.0f,
                    "relative physical extent scale must be "
                    "positive: " +
                        resource.logical_resource);
            }
        }
        const auto image =
            logical->second->type.constructor ==
            LogicalTypeConstructor::image;
        const auto buffer =
            logical->second->type.constructor ==
            LogicalTypeConstructor::buffer;
        require(
            !image ||
                resource.representation !=
                    VulkanResourceRepresentation::
                        materialized_buffer,
            "logical image cannot use a materialized_buffer "
            "representation: " +
                resource.logical_resource);
        require(
            !buffer ||
                resource.representation ==
                    VulkanResourceRepresentation::
                        materialized_buffer,
            "logical buffer requires a materialized_buffer "
            "representation: " +
                resource.logical_resource);
        if (logical->second->materialization ==
            LogicalMaterializationRequirement::external) {
            require(
                resource.representation ==
                    VulkanResourceRepresentation::external,
                "external logical resource requires external "
                "physical ownership: " +
                    resource.logical_resource);
        }
        if (resource.widest_read ==
                LogicalReadFootprintKind::arbitrary ||
            resource.widest_read ==
                LogicalReadFootprintKind::temporal) {
            require(
                resource.representation ==
                        VulkanResourceRepresentation::
                            materialized_image ||
                    resource.representation ==
                        VulkanResourceRepresentation::external ||
                    resource.representation ==
                        VulkanResourceRepresentation::
                            materialized_buffer,
                "arbitrary/temporal reads require a materialized "
                "resource: " +
                    resource.logical_resource);
        }
        const auto expected_stored =
            resource.representation ==
                VulkanResourceRepresentation::
                    materialized_image ||
            resource.representation ==
                VulkanResourceRepresentation::
                    materialized_buffer ||
            resource.representation ==
                VulkanResourceRepresentation::external;
        require(
            resource.stored == expected_stored,
            "Vulkan complete physical resource has inconsistent "
            "stored ownership: " +
                resource.logical_resource);
        require(
            !resource.aliasable ||
                (expected_stored &&
                 resource.representation !=
                     VulkanResourceRepresentation::external),
            "only engine-owned materialized resources may alias: " +
                resource.logical_resource);
        for (const auto &feature :
             resource.required_physical_features) {
            require(
                plan_features.contains(feature),
                "physical resource feature is missing from plan "
                "closure: " +
                    resource.logical_resource + " -> " +
                    feature);
        }
        std::set<std::string, std::less<>>
            minimum_resource_features;
        switch (resource.representation) {
        case VulkanResourceRepresentation::
            materialized_image:
        case VulkanResourceRepresentation::external:
            minimum_resource_features.insert(
                "pelican.vulkan.sampled_image@1");
            break;
        case VulkanResourceRepresentation::
            materialized_buffer:
            minimum_resource_features.insert(
                "pelican.vulkan.storage_buffer@1");
            break;
        case VulkanResourceRepresentation::
            transient_attachment:
            minimum_resource_features.insert(
                "pelican.vulkan.transient_attachment@1");
            break;
        case VulkanResourceRepresentation::
            tile_local_attachment:
            minimum_resource_features.insert(
                "pelican.vulkan.tile_based@1");
            minimum_resource_features.insert(
                "pelican.vulkan.dynamic_rendering_local_read@1");
            minimum_resource_features.insert(
                "pelican.vulkan.transient_attachment@1");
            break;
        }
        if (use->second.transfer_access) {
            minimum_resource_features.insert(
                "pelican.vulkan.transfer_copy@1");
        }
        if (resource.format ==
            automatic->second->format) {
            for (const auto &feature :
                 automatic->second
                     ->required_physical_features) {
                if (feature.starts_with(
                        "pelican.vulkan.format_")) {
                    minimum_resource_features.insert(
                        feature);
                }
            }
        }
        const std::set<std::string, std::less<>>
            resource_features(
                resource.required_physical_features.begin(),
                resource.required_physical_features.end());
        for (const auto &feature :
             minimum_resource_features) {
            require(
                resource_features.contains(feature),
                "Vulkan complete physical resource omits required "
                "feature '" +
                    feature + "': " +
                    resource.logical_resource);
        }
        if (resource.format != automatic->second->format) {
            const auto *capability =
                findFormatCapability(
                    format_capabilities,
                    resource.logical_resource,
                    resource.format);
            require(
                capability != nullptr &&
                    capability->image_usage_supported,
                "alternate complete-plan format lacks device usage "
                "evidence: " +
                    resource.logical_resource + " -> " +
                    resource.format);
            require(
                std::find(
                    capability->supported_samples.begin(),
                    capability->supported_samples.end(),
                    resource.rasterization_samples) !=
                    capability->supported_samples.end(),
                "alternate complete-plan format lacks sample-count "
                "evidence: " +
                    resource.logical_resource);
            require(
                capability->max_array_layers >=
                    resource.array_layers,
                "alternate complete-plan format lacks array-layer "
                "evidence: " +
                    resource.logical_resource);
            if (resource.mip_levels.mode ==
                ImageMipLevelMode::fixed) {
                require(
                    capability->max_mip_levels >=
                        resource.mip_levels.count,
                    "alternate complete-plan format lacks mip "
                    "evidence: " +
                        resource.logical_resource);
            }
        }
        physical_resources.emplace(
            resource.logical_resource, &resource);
    }

    for (const auto &node : canonical_graph.nodes) {
        std::map<std::string, std::string, std::less<>>
            resource_by_port;
        for (const auto &use : node.uses) {
            const auto *value =
                use.input_value
                    ? &*use.input_value
                    : use.output_value
                          ? &*use.output_value
                          : nullptr;
            require(
                value != nullptr,
                "logical port use has no physical resource");
            resource_by_port.emplace(
                use.port, value->resource);
        }
        for (const auto &port : node.ports) {
            const auto resource_name =
                resource_by_port.find(port.name);
            require(
                resource_name != resource_by_port.end(),
                "logical port has no physical resource: " +
                    node.name + "." + port.name);
            const auto &resource =
                *physical_resources.at(
                    resource_name->second);
            for (const auto &relation :
                 port.relations) {
                if (relation.kind ==
                    LogicalPortRelationKind::per_view) {
                    continue;
                }
                const auto other_name =
                    resource_by_port.find(
                        relation.other_port);
                require(
                    other_name != resource_by_port.end(),
                    "logical port relation has no physical peer: " +
                        node.name + "." + port.name);
                const auto &other =
                    *physical_resources.at(
                        other_name->second);
                switch (relation.kind) {
                case LogicalPortRelationKind::same_extent:
                    require(
                        resource.extent == other.extent &&
                            resource.dimension ==
                                other.dimension,
                        "complete physical plan violates same_extent "
                        "relation: " +
                            node.name + "." + port.name);
                    break;
                case LogicalPortRelationKind::extent_scale: {
                    require(
                        resource.extent.has_value() &&
                            other.extent.has_value() &&
                            resource.extent->kind ==
                                other.extent->kind &&
                            resource.dimension ==
                                other.dimension,
                        "complete physical plan cannot resolve "
                        "extent_scale relation: " +
                            node.name + "." + port.name);
                    const auto scale_x =
                        static_cast<long double>(
                            relation.scale_x.numerator) /
                        static_cast<long double>(
                            relation.scale_x.denominator);
                    const auto scale_y =
                        static_cast<long double>(
                            relation.scale_y.numerator) /
                        static_cast<long double>(
                            relation.scale_y.denominator);
                    const auto close =
                        [](long double left,
                           long double right) {
                            const auto magnitude =
                                std::max(
                                    {1.0L,
                                     std::abs(left),
                                     std::abs(right)});
                            return std::abs(
                                       left - right) <=
                                   magnitude * 0.000001L;
                        };
                    if (resource.extent->kind ==
                        ResourceExtentKind::
                            output_relative) {
                        require(
                            close(
                                resource.extent->scale_x,
                                other.extent->scale_x *
                                    scale_x) &&
                                close(
                                    resource.extent->scale_y,
                                    other.extent->scale_y *
                                        scale_y),
                            "complete physical plan violates "
                            "extent_scale relation: " +
                                node.name + "." + port.name);
                    } else {
                        require(
                            close(
                                resource.extent->width,
                                other.extent->width *
                                    scale_x) &&
                                close(
                                    resource.extent->height,
                                    other.extent->height *
                                        scale_y),
                            "complete physical plan violates "
                            "extent_scale relation: " +
                                node.name + "." + port.name);
                    }
                    break;
                }
                case LogicalPortRelationKind::same_view_set:
                    require(
                        resource.view_layout ==
                                other.view_layout &&
                            resource.array_layers ==
                                other.array_layers &&
                            resource.dimension ==
                                other.dimension,
                        "complete physical plan violates "
                        "same_view_set relation: " +
                            node.name + "." + port.name);
                    break;
                case LogicalPortRelationKind::same_samples:
                    require(
                        resource.rasterization_samples ==
                            other.rasterization_samples,
                        "complete physical plan violates "
                        "same_samples relation: " +
                            node.name + "." + port.name);
                    break;
                case LogicalPortRelationKind::per_view:
                    break;
                }
            }
        }
    }

    require(
        package.external_depth_export.has_value() ==
            automatic_plan.external_depth_export.has_value(),
        "Vulkan complete physical plan cannot add or remove the "
        "target-requested external depth boundary");
    if (package.external_depth_export) {
        const auto &external =
            *package.external_depth_export;
        require(
            external.source_resource ==
                automatic_plan.external_depth_export
                    ->source_resource,
            "Vulkan complete physical plan cannot redirect the "
            "target-requested external depth source");
        const auto physical =
            physical_resources.find(
                external.source_resource);
        const auto logical =
            logical_resources.find(
                external.source_resource);
        require(
            physical != physical_resources.end() &&
                logical != logical_resources.end() &&
                semanticTypeIdName(
                    logical->second->type.semantic) ==
                    "pelican.render.depth@1",
            "external depth export requires a typed physical "
            "depth resource");
        require(
            physical->second->representation ==
                    VulkanResourceRepresentation::
                        materialized_image &&
                physical->second->stored &&
                external.format ==
                    physical->second->format &&
                external.view_layout ==
                    physical->second->view_layout &&
                external.array_layers ==
                    physical->second->array_layers,
            "external depth export metadata does not match its "
            "materialized physical resource");
    }

    validateVulkanPhysicalAttachmentPlans(
        canonical_graph, package.resources,
        package.attachments);
    using AttachmentContractEntry =
        std::tuple<
            std::string,
            VulkanPhysicalAttachmentAspect,
            std::optional<ImageSubresourceRange>>;
    const auto attachmentContract =
        [&](std::string_view node) {
            std::vector<AttachmentContractEntry> result;
            for (const auto &attachment :
                 package.attachments) {
                if (attachment.node == node) {
                    result.emplace_back(
                        attachment.logical_resource,
                        attachment.aspect,
                        attachment.subresource);
                }
            }
            if (result.empty() &&
                package.attachments.empty()) {
                const auto logical =
                    logical_nodes.find(node);
                if (logical != logical_nodes.end()) {
                    for (const auto &use :
                         logical->second->uses) {
                        if (!use.output_value ||
                            (use.intent !=
                                 LogicalAccessIntent::
                                     automatic &&
                             use.intent !=
                                 LogicalAccessIntent::
                                     attachment)) {
                            continue;
                        }
                        const auto resource =
                            logical_resources.find(
                                use.output_value
                                    ->resource);
                        if (resource ==
                                logical_resources.end() ||
                            resource->second
                                    ->type.constructor !=
                                LogicalTypeConstructor::
                                    image) {
                            continue;
                        }
                        result.emplace_back(
                            use.output_value->resource,
                            semanticTypeIdName(
                                resource->second
                                    ->type.semantic) ==
                                    "pelican.render.depth@1"
                                ? VulkanPhysicalAttachmentAspect::
                                      depth
                                : VulkanPhysicalAttachmentAspect::
                                      color,
                            std::nullopt);
                    }
                }
            }
            std::sort(result.begin(), result.end());
            return result;
        };
    for (const auto &scope : package.scopes) {
        std::optional<
            std::vector<AttachmentContractEntry>>
            fused_contract;
        for (std::size_t node_index = 0;
             node_index < scope.nodes.size();
             ++node_index) {
            const auto &node =
                *logical_nodes.at(
                    scope.nodes[node_index]);
            if (scope.kind ==
                VulkanPhysicalScopeKind::rendering) {
                for (const auto &use : node.uses) {
                    if (!use.output_value ||
                        (use.intent !=
                             LogicalAccessIntent::
                                 automatic &&
                         use.intent !=
                             LogicalAccessIntent::
                                 attachment)) {
                        continue;
                    }
                    const auto resource =
                        physical_resources.find(
                            use.output_value
                                ->resource);
                    if (resource ==
                            physical_resources.end() ||
                        logical_resources
                                .at(
                                    use.output_value
                                        ->resource)
                                ->type.constructor !=
                            LogicalTypeConstructor::
                                image) {
                        continue;
                    }
                    require(
                        resource->second
                                ->rasterization_samples ==
                            scope.rasterization_samples,
                        "rendering scope sample count does not "
                        "match attachment resource: " +
                            scope.id + " -> " +
                            use.output_value->resource);
                }
            }
            if (!scope.single_rendering_instance) {
                continue;
            }
            const auto contract =
                attachmentContract(node.name);
            require(
                !contract.empty(),
                "single rendering instance has a node without "
                "attachments: " +
                    node.name);
            if (!scope.local_reads.empty()) {
                continue;
            }
            if (!fused_contract) {
                fused_contract = contract;
            } else {
                require(
                    *fused_contract == contract,
                    "single rendering instance requires identical "
                    "attachment contracts: " +
                        scope.id);
            }
            for (const auto &attachment :
                 package.attachments) {
                if (attachment.node != node.name) continue;
                if (node_index > 0) {
                    require(
                        attachment.load_op ==
                            VulkanPhysicalAttachmentLoadOp::
                                load,
                        "later node in a single rendering instance "
                        "must Load its attachments: " +
                            node.name);
                }
                if (node_index + 1 <
                    scope.nodes.size()) {
                    require(
                        attachment.store_op ==
                            VulkanPhysicalAttachmentStoreOp::
                                store,
                        "non-final node in a single rendering "
                        "instance must Store its attachments: " +
                            node.name);
                }
            }
        }
        for (const auto &local_read :
             scope.local_reads) {
            const auto resource =
                physical_resources.find(local_read);
            require(
                resource != physical_resources.end(),
                "physical scope local read references unknown "
                "resource '" +
                    local_read + "'");
            require(
                resource->second->representation ==
                    VulkanResourceRepresentation::
                        tile_local_attachment,
                "physical scope local read requires a tile-local "
                "resource: " +
                    local_read);
        }
    }

    std::map<std::string,
             const VulkanAliasGroupPlan *, std::less<>>
        aliases;
    std::set<std::string, std::less<>>
        aliased_resources;
    for (const auto &group : package.alias_groups) {
        require(
            group.resources.size() >= 2,
            "physical alias group '" + group.id +
                "' needs at least two resources");
        aliases.emplace(group.id, &group);
        for (std::size_t left = 0;
             left < group.resources.size(); ++left) {
            const auto left_resource =
                physical_resources.find(
                    group.resources[left]);
            require(
                left_resource != physical_resources.end() &&
                    left_resource->second->aliasable,
                "physical alias group references a missing or "
                "non-aliasable resource: " +
                    group.resources[left]);
            require(
                aliased_resources.insert(
                    group.resources[left])
                    .second,
                "physical resource belongs to more than one alias "
                "group: " +
                    group.resources[left]);
            for (std::size_t right = left + 1;
                 right < group.resources.size(); ++right) {
                const auto right_resource =
                    physical_resources.find(
                        group.resources[right]);
                require(
                    right_resource !=
                            physical_resources.end() &&
                        right_resource->second->aliasable,
                    "physical alias group references a missing or "
                    "non-aliasable resource: " +
                        group.resources[right]);
                require(
                    aliasShapesMatch(
                        *left_resource->second,
                        *right_resource->second) &&
                        !lifetimesOverlap(
                            left_resource->second->lifetime,
                            right_resource->second->lifetime),
                    "physical alias group has incompatible shapes "
                    "or overlapping lifetimes: " +
                        group.id);
            }
        }
    }

    std::set<std::string, std::less<>>
        enabled_extension_set(
            enabled_extensions.begin(),
            enabled_extensions.end());
    std::vector<PlanningDiagnostic> diagnostics;
    for (const auto &native : package.native_scopes) {
        const auto scope = scopes.find(native.scope);
        require(
            scope != scopes.end(),
            "NativeScope references unknown physical scope '" +
                native.scope + "'");
        require(
            endpoint_capabilities.contains(
                native.queue_capability),
            "NativeScope requires unavailable queue capability '" +
                native.queue_capability + "'");
        require(
            plan_features.contains(
                native.queue_capability),
            "NativeScope queue capability is missing from plan "
            "feature closure: " +
                native.queue_capability);
        for (const auto &feature :
             native.required_features) {
            require(
                endpoint_capabilities.contains(feature) &&
                    plan_features.contains(feature),
                "NativeScope requires an unavailable or unclosed "
                "feature '" +
                    feature + "'");
        }
        for (const auto &extension :
             native.required_extensions) {
            require(
                extension.starts_with("VK_"),
                "NativeScope extension must use a Vulkan VK_* "
                "name: " +
                    extension);
            require(
                enabled_extension_set.contains(extension),
                "NativeScope requires a Vulkan extension that is "
                "not enabled: '" +
                    extension + "'");
        }

        std::set<std::string, std::less<>>
            native_nodes(
                scope->second->nodes.begin(),
                scope->second->nodes.end());
        std::map<std::string, LogicalAccessMode, std::less<>>
            expected_accesses;
        for (const auto &node : canonical_graph.nodes) {
            if (!native_nodes.contains(node.name)) continue;
            for (const auto &use : node.uses) {
                const auto *value =
                    use.input_value
                        ? &*use.input_value
                        : use.output_value
                              ? &*use.output_value
                              : nullptr;
                require(
                    value != nullptr,
                    "NativeScope logical use has no resource");
                const auto [found, inserted] =
                    expected_accesses.emplace(
                        value->resource, use.access);
                if (!inserted) {
                    found->second =
                        mergeAccess(
                            found->second,
                            use.access);
                }
            }
        }
        require(
            native.resources.size() ==
                expected_accesses.size(),
            "NativeScope boundary must declare every accessed "
            "logical resource exactly once: " +
                native.scope);
        std::set<std::string, std::less<>>
            boundary_resources;
        for (const auto &boundary : native.resources) {
            const auto expected =
                expected_accesses.find(
                    boundary.logical_resource);
            require(
                expected != expected_accesses.end() &&
                    expected->second == boundary.access,
                "NativeScope resource access does not match the "
                "logical scope: " +
                    native.scope + " -> " +
                    boundary.logical_resource);
            const auto logical =
                logical_resources.find(
                    boundary.logical_resource);
            require(
                logical != logical_resources.end() &&
                    semanticTypeIdName(
                        logical->second->type.semantic) ==
                        boundary.semantic_type,
                "NativeScope semantic type does not match logical "
                "resource: " +
                    boundary.logical_resource);
            if (boundary.ownership ==
                VulkanNativeScopeResourceOwnership::
                    native_scope) {
                require(
                    logical->second->materialization ==
                        LogicalMaterializationRequirement::
                            external,
                    "NativeScope may own only an external logical "
                    "resource in schema v1: " +
                        boundary.logical_resource);
            }
            if (native.synchronization ==
                VulkanNativeScopeSynchronizationMode::
                    automatic) {
                require(
                    boundary.stages.empty() &&
                        boundary.accesses.empty(),
                    "automatic NativeScope synchronization derives "
                    "stage/access and must not author masks");
            } else if (
                native.synchronization ==
                VulkanNativeScopeSynchronizationMode::manual) {
                require(
                    !boundary.stages.empty() &&
                        !boundary.accesses.empty(),
                    "manual NativeScope synchronization requires "
                    "stage and access declarations for every "
                    "resource");
            }
            boundary_resources.insert(
                boundary.logical_resource);
        }
        for (const auto &alias_id :
             native.alias_groups) {
            const auto alias = aliases.find(alias_id);
            require(
                alias != aliases.end(),
                "NativeScope references unknown alias group '" +
                    alias_id + "'");
            require(
                std::all_of(
                    alias->second->resources.begin(),
                    alias->second->resources.end(),
                    [&](const auto &resource) {
                        return boundary_resources.contains(
                            resource);
                    }),
                "NativeScope alias group crosses its declared "
                "resource boundary: " +
                    alias_id);
        }
        if (native.synchronization ==
            VulkanNativeScopeSynchronizationMode::unchecked) {
            diagnostics.push_back(
                PlanningDiagnostic{
                    "pelican.plan.native_scope_unchecked_sync@1",
                    PlanningDiagnosticSeverity::warning,
                    native.scope,
                    "the NativeScope author owns synchronization "
                    "inside the declared boundary",
                });
        }
        if (!native.capture_compatible) {
            diagnostics.push_back(
                PlanningDiagnostic{
                    "pelican.plan.native_scope_capture_gap@1",
                    PlanningDiagnosticSeverity::info,
                    native.scope,
                    "capture tooling must treat the native "
                    "implementation as opaque",
                });
        }
        if (!native.device_loss_recoverable) {
            diagnostics.push_back(
                PlanningDiagnostic{
                    "pelican.plan.native_scope_device_loss_gap@1",
                    PlanningDiagnosticSeverity::warning,
                    native.scope,
                    "the native implementation cannot join "
                    "automatic device-loss reconstruction",
                });
        }
        if (!native.hot_reloadable) {
            diagnostics.push_back(
                PlanningDiagnostic{
                    "pelican.plan.native_scope_hot_reload_gap@1",
                    PlanningDiagnosticSeverity::info,
                    native.scope,
                    "changes require rebuilding the containing "
                    "physical package",
                });
        }
    }

    return {
        .package = package,
        .package_fingerprint =
            vulkanCompletePhysicalPlanPackageFingerprint(
                package),
        .diagnostics = std::move(diagnostics),
    };
}

VulkanTargetPlan applyVerifiedVulkanCompletePhysicalPlanPackage(
    const VulkanTargetPlan &automatic_plan,
    const VerifiedVulkanCompletePhysicalPlanPackage &verified) {
    const auto &package = verified.package;
    if (verified.package_fingerprint !=
        vulkanCompletePhysicalPlanPackageFingerprint(package)) {
        throw std::runtime_error(
            "Verified Vulkan complete physical plan fingerprint is stale");
    }
    if (package.graph != automatic_plan.graph ||
        package.logical_graph_fingerprint !=
            automatic_plan.logical_graph_fingerprint ||
        package.automatic_plan_fingerprint !=
            automatic_plan.automatic_plan_fingerprint ||
        package.backend_candidate !=
            automatic_plan.backend_selection.selected_candidate) {
        throw std::runtime_error(
            "Verified Vulkan complete physical plan does not match its "
            "automatic target plan");
    }

    auto result = automatic_plan;
    result.resources = package.resources;
    result.scopes = package.scopes;
    result.attachments = package.attachments;
    result.alias_groups = package.alias_groups;
    result.required_physical_features =
        package.required_physical_features;
    result.external_depth_export =
        package.external_depth_export;
    return result;
}

void validateVulkanTargetPlanMatchesCompletePhysicalPackage(
    const VulkanTargetPlan &target_plan,
    const VerifiedVulkanCompletePhysicalPlanPackage &verified) {
    const auto &package = verified.package;
    if (verified.package_fingerprint !=
        vulkanCompletePhysicalPlanPackageFingerprint(package)) {
        throw std::runtime_error(
            "Verified Vulkan complete physical plan fingerprint is stale");
    }
    if (target_plan.graph != package.graph ||
        target_plan.logical_graph_fingerprint !=
            package.logical_graph_fingerprint ||
        target_plan.automatic_plan_fingerprint !=
            package.automatic_plan_fingerprint ||
        target_plan.backend_selection.selected_candidate !=
            package.backend_candidate ||
        target_plan.resources != package.resources ||
        target_plan.scopes != package.scopes ||
        target_plan.attachments != package.attachments ||
        target_plan.alias_groups != package.alias_groups ||
        target_plan.required_physical_features !=
            package.required_physical_features ||
        target_plan.external_depth_export !=
            package.external_depth_export) {
        throw std::runtime_error(
            "Vulkan runtime target plan does not match its verified "
            "complete physical package");
    }
}

} // namespace Pelican
