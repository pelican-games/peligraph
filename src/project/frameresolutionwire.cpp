#include "frameresolutionwire.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(
        "frame runtime resolution wire " + std::move(message));
}

const Json &requireObjectField(const Json &object, std::string_view field,
                               std::string_view context) {
    const auto found = object.find(std::string{field});
    if (found == object.end() || !found->is_object()) {
        fail(std::string{context} + " requires object field '" +
             std::string{field} + "'");
    }
    return *found;
}

const Json &requireArrayField(const Json &object, std::string_view field,
                              std::string_view context) {
    const auto found = object.find(std::string{field});
    if (found == object.end() || !found->is_array()) {
        fail(std::string{context} + " requires array field '" +
             std::string{field} + "'");
    }
    return *found;
}

std::string requireStringField(const Json &object, std::string_view field,
                               std::string_view context) {
    const auto found = object.find(std::string{field});
    if (found == object.end() || !found->is_string()) {
        fail(std::string{context} + " requires string field '" +
             std::string{field} + "'");
    }
    auto result = found->get<std::string>();
    if (result.empty()) {
        fail(std::string{context} + " field '" + std::string{field} +
             "' must not be empty");
    }
    return result;
}

std::uint32_t requirePositiveUint32Field(
    const Json &object, std::string_view field,
    std::string_view context) {
    const auto found = object.find(std::string{field});
    if (found == object.end() ||
        (!found->is_number_unsigned() && !found->is_number_integer())) {
        fail(std::string{context} + " requires integer field '" +
             std::string{field} + "'");
    }
    if (found->is_number_integer() && !found->is_number_unsigned() &&
        found->get<std::int64_t>() < 0) {
        fail(std::string{context} + " field '" + std::string{field} +
             "' must be positive");
    }
    const auto value = found->get<std::uint64_t>();
    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        fail(std::string{context} + " field '" + std::string{field} +
             "' must be a positive uint32");
    }
    return static_cast<std::uint32_t>(value);
}

ResolvedResourceExtent parseExtent(const Json &value,
                                   std::string_view context) {
    if (!value.is_object()) {
        fail(std::string{context} + " must be an object");
    }
    return {
        .width = requirePositiveUint32Field(value, "width", context),
        .height = requirePositiveUint32Field(value, "height", context),
    };
}

nlohmann::ordered_json extentToJson(ResolvedResourceExtent extent) {
    return {
        {"width", extent.width},
        {"height", extent.height},
    };
}

} // namespace

FrameRuntimeResolutionWire makeFrameRuntimeResolutionWire(
    const VulkanTargetPlan &plan,
    const FrameRuntimeExtentResolver &resolve_extent) {
    if (!plan.resolution_plan) {
        throw std::runtime_error(
            "frame runtime resolution requires a compiled resolution plan");
    }
    if (!resolve_extent) {
        throw std::runtime_error(
            "frame runtime resolution requires an extent resolver");
    }

    const auto &compiled = *plan.resolution_plan;
    FrameRuntimeResolutionWire result{
        .render_source_resource = compiled.render_source_resource,
        .render_extent = resolve_extent(compiled.render_source_resource),
        .output_source_resource = compiled.output_source_resource,
        .output_extent = resolve_extent(compiled.output_source_resource),
    };
    result.resources.reserve(plan.resources.size());
    for (const auto &resource : plan.resources) {
        if (!resource.extent) {
            continue;
        }
        result.resources.push_back(FrameRuntimeResourceExtent{
            .resource = resource.logical_resource,
            .extent = resolve_extent(resource.logical_resource),
        });
    }
    return result;
}

nlohmann::ordered_json frameRuntimeResolutionWireToJson(
    const FrameRuntimeResolutionWire &resolution) {
    nlohmann::ordered_json resources =
        nlohmann::ordered_json::array();
    for (const auto &resource : resolution.resources) {
        resources.push_back({
            {"resource", resource.resource},
            {"extent", extentToJson(resource.extent)},
        });
    }
    return {
        {"schema", frameRuntimeResolutionWireSchema},
        {"version", frameRuntimeResolutionWireVersion},
        {"render_source_resource", resolution.render_source_resource},
        {"render_extent", extentToJson(resolution.render_extent)},
        {"output_source_resource", resolution.output_source_resource},
        {"output_extent", extentToJson(resolution.output_extent)},
        {"resources", std::move(resources)},
    };
}

FrameRuntimeResolutionWire frameRuntimeResolutionWireFromJson(
    const nlohmann::json &document) {
    if (!document.is_object()) {
        fail("must be an object");
    }
    if (requireStringField(document, "schema", "runtime_resolution") !=
        frameRuntimeResolutionWireSchema) {
        fail("requires schema 'pelican.frame_runtime_resolution'");
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_unsigned() ||
        version->get<std::uint64_t>() !=
            frameRuntimeResolutionWireVersion) {
        fail("requires pelican.frame_runtime_resolution version 1");
    }

    FrameRuntimeResolutionWire result{
        .render_source_resource = requireStringField(
            document, "render_source_resource", "runtime_resolution"),
        .render_extent = parseExtent(
            requireObjectField(document, "render_extent",
                               "runtime_resolution"),
            "runtime_resolution.render_extent"),
        .output_source_resource = requireStringField(
            document, "output_source_resource", "runtime_resolution"),
        .output_extent = parseExtent(
            requireObjectField(document, "output_extent",
                               "runtime_resolution"),
            "runtime_resolution.output_extent"),
    };
    const auto &resources =
        requireArrayField(document, "resources", "runtime_resolution");
    result.resources.reserve(resources.size());
    std::set<std::string, std::less<>> names;
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const auto context = "runtime_resolution.resources[" +
                             std::to_string(index) + "]";
        const auto &value = resources[index];
        if (!value.is_object()) {
            fail(context + " must be an object");
        }
        auto resource = requireStringField(value, "resource", context);
        if (!names.insert(resource).second) {
            fail("contains duplicate resource '" + resource + "'");
        }
        result.resources.push_back(FrameRuntimeResourceExtent{
            .resource = std::move(resource),
            .extent = parseExtent(
                requireObjectField(value, "extent", context),
                context + ".extent"),
        });
    }
    return result;
}

} // namespace Pelican
