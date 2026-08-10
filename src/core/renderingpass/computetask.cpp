#include "computetask.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../renderer/frameresources.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/render_target_layout_tracker.hpp"
#include "../vkcore/util.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

constexpr uint32_t max_compute_descriptor_sets = 256;
constexpr uint32_t max_compute_descriptors = 1024;
constexpr vk::DeviceSize compute_dispatch_command_size =
    sizeof(vk::DispatchIndirectCommand);
static_assert(
    sizeof(vk::DispatchIndirectCommand) ==
    sizeof(std::uint32_t) * 3);
static_assert(
    frameGraphIndexedDrawCommandBytes ==
    sizeof(std::uint32_t) * 5);
static_assert(frameGraphDrawCountBytes == 4);
static_assert(frameGraphSceneDrawBoundsV1Bytes == 32);
static_assert(frameGraphSceneDrawSegmentV1Bytes == 32);

std::string requireString(const nlohmann::json &json, std::string_view field, std::string_view context) {
    if (!json.contains(field) || !json.at(field).is_string()) {
        throw std::runtime_error(std::string{context} + " requires string field: " + std::string{field});
    }
    return json.at(field).get<std::string>();
}

uint32_t requireUint32(const nlohmann::json &json, std::string_view field, std::string_view context) {
    if (!json.contains(field) || !json.at(field).is_number_integer()) {
        throw std::runtime_error(std::string{context} + " requires non-negative integer field: " +
                                 std::string{field});
    }
    const auto value = json.at(field).get<uint64_t>();
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string{context} + " field is too large: " + std::string{field});
    }
    return static_cast<uint32_t>(value);
}

vk::DeviceSize requireDeviceSize(const nlohmann::json &json, std::string_view field, std::string_view context) {
    if (!json.contains(field) || !json.at(field).is_number_integer()) {
        throw std::runtime_error(std::string{context} + " requires non-negative integer field: " +
                                 std::string{field});
    }
    return static_cast<vk::DeviceSize>(json.at(field).get<uint64_t>());
}

FrameGraphHostBufferSource parseHostBufferSource(
    const nlohmann::json &entry,
    std::string_view context) {
    const auto value =
        requireString(entry, "host_source", context);
    if (value == "scene_lights_v2") {
        return FrameGraphHostBufferSource::scene_lights_v2;
    }
    if (value == "scene_draw_commands_v1") {
        return FrameGraphHostBufferSource::
            scene_draw_commands_v1;
    }
    if (value == "scene_draw_bounds_v1") {
        return FrameGraphHostBufferSource::
            scene_draw_bounds_v1;
    }
    if (value == "scene_draw_segments_v1") {
        return FrameGraphHostBufferSource::
            scene_draw_segments_v1;
    }
    throw std::runtime_error(
        std::string{context} +
        " has unknown host_source '" + value + "'");
}

FrameGraphBufferCommandLayout
parseBufferCommandLayout(
    const nlohmann::json &entry,
    std::string_view context) {
    const auto value =
        requireString(entry, "command_layout", context);
    if (value == "compute_dispatch") {
        return FrameGraphBufferCommandLayout::
            compute_dispatch;
    }
    if (value == "indexed_draw") {
        return FrameGraphBufferCommandLayout::
            indexed_draw;
    }
    if (value == "draw_count") {
        return FrameGraphBufferCommandLayout::
            draw_count;
    }
    throw std::runtime_error(
        std::string{context} +
        " has unknown command_layout '" + value + "'");
}

std::pair<std::uint32_t, std::uint32_t>
renderTargetExtent(
    const nlohmann::json &config,
    std::string_view resource,
    std::string_view context) {
    if (!config.contains("render_targets") ||
        !config.at("render_targets").is_array()) {
        throw std::runtime_error(
            std::string{context} +
            " requires render_targets to resolve resource '" +
            std::string{resource} + "'");
    }
    const auto found = std::find_if(
        config.at("render_targets").begin(),
        config.at("render_targets").end(),
        [resource](const nlohmann::json &target) {
            return target.is_object() &&
                   target.value("name", std::string{}) ==
                       resource;
        });
    if (found == config.at("render_targets").end()) {
        throw std::runtime_error(
            std::string{context} +
            " references unknown render target '" +
            std::string{resource} + "'");
    }
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    if (found->contains("width") &&
        found->contains("height")) {
        width = requireUint32(*found, "width", context);
        height = requireUint32(*found, "height", context);
    } else {
        const auto base = std::find_if(
            config.at("render_targets").begin(),
            config.at("render_targets").end(),
            [](const nlohmann::json &target) {
                return target.is_object() &&
                       target.contains("width") &&
                       target.contains("height");
            });
        if (base ==
            config.at("render_targets").end()) {
            throw std::runtime_error(
                std::string{context} +
                " cannot derive a base render-target extent");
        }
        const auto base_width =
            requireUint32(*base, "width", context);
        const auto base_height =
            requireUint32(*base, "height", context);
        const auto scale =
            found->value("extent_scale", 1.0);
        if (!std::isfinite(scale) || scale <= 0.0) {
            throw std::runtime_error(
                std::string{context} +
                " requires a positive finite extent_scale");
        }
        width = static_cast<std::uint64_t>(
            static_cast<double>(base_width) * scale);
        height = static_cast<std::uint64_t>(
            static_cast<double>(base_height) * scale);
    }
    if (width == 0 || height == 0) {
        throw std::runtime_error(
            std::string{context} +
            " requires a non-zero render-target extent");
    }
    if (width >
            std::numeric_limits<std::uint32_t>::max() ||
        height >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            std::string{context} +
            " render-target extent exceeds uint32");
    }
    return {
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height)};
}

vk::DeviceSize checkedMultiply(
    vk::DeviceSize lhs, vk::DeviceSize rhs,
    std::string_view context) {
    if (rhs != 0 &&
        lhs > std::numeric_limits<vk::DeviceSize>::max() /
                  rhs) {
        throw std::overflow_error(
            std::string{context} + " byte size overflow");
    }
    return lhs * rhs;
}

vk::DeviceSize checkedAdd(
    vk::DeviceSize lhs, vk::DeviceSize rhs,
    std::string_view context) {
    if (lhs >
        std::numeric_limits<vk::DeviceSize>::max() -
            rhs) {
        throw std::overflow_error(
            std::string{context} + " byte size overflow");
    }
    return lhs + rhs;
}

std::pair<FrameGraphBufferExtentSizeDefinition,
          vk::DeviceSize>
parseExtentDerivedBufferSize(
    const nlohmann::json &entry,
    const nlohmann::json &config,
    std::string_view context) {
    const auto &encoded =
        entry.at("size_from_extent");
    if (!encoded.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            ".size_from_extent must be an object");
    }
    for (auto field = encoded.begin();
         field != encoded.end(); ++field) {
        if (field.key() != "resource" &&
            field.key() != "tile_width" &&
            field.key() != "tile_height" &&
            field.key() != "header_bytes" &&
            field.key() != "bytes_per_tile" &&
            field.key() != "copies") {
            throw std::runtime_error(
                std::string{context} +
                ".size_from_extent has unknown field '" +
                field.key() + "'");
        }
    }
    FrameGraphBufferExtentSizeDefinition definition{
        .resource = requireString(
            encoded, "resource",
            std::string{context} +
                ".size_from_extent"),
        .tile_width = requireUint32(
            encoded, "tile_width",
            std::string{context} +
                ".size_from_extent"),
        .tile_height = requireUint32(
            encoded, "tile_height",
            std::string{context} +
                ".size_from_extent"),
        .header_bytes = requireDeviceSize(
            encoded, "header_bytes",
            std::string{context} +
                ".size_from_extent"),
        .bytes_per_tile = requireDeviceSize(
            encoded, "bytes_per_tile",
            std::string{context} +
                ".size_from_extent"),
        .copies =
            encoded.contains("copies")
                ? requireUint32(
                      encoded, "copies",
                      std::string{context} +
                          ".size_from_extent")
                : 1u,
    };
    if (definition.tile_width == 0 ||
        definition.tile_height == 0 ||
        definition.bytes_per_tile == 0 ||
        definition.copies == 0) {
        throw std::runtime_error(
            std::string{context} +
            ".size_from_extent requires positive tile dimensions "
            "bytes_per_tile, and copies");
    }
    const auto [width, height] =
        renderTargetExtent(
            config, definition.resource, context);
    const auto tiles_x =
        (static_cast<std::uint64_t>(width) +
         definition.tile_width - 1) /
        definition.tile_width;
    const auto tiles_y =
        (static_cast<std::uint64_t>(height) +
         definition.tile_height - 1) /
        definition.tile_height;
    const auto tile_count =
        checkedMultiply(
            tiles_x, tiles_y, context);
    const auto payload =
        checkedMultiply(
            tile_count,
            definition.bytes_per_tile,
            context);
    const auto region_byte_size =
        checkedAdd(
            definition.header_bytes,
            payload, context);
    const auto byte_size =
        checkedMultiply(
            region_byte_size,
            definition.copies,
            context);
    return {std::move(definition), byte_size};
}

std::vector<std::string> parseStringList(const nlohmann::json &json, std::string_view context) {
    if (json.is_null()) {
        return {};
    }
    if (json.is_string()) {
        return {json.get<std::string>()};
    }
    if (!json.is_array()) {
        throw std::runtime_error(std::string{context} + " must be a string or string array");
    }

    std::vector<std::string> values;
    values.reserve(json.size());
    for (const auto &entry : json) {
        if (!entry.is_string()) {
            throw std::runtime_error(std::string{context} + " entries must be strings");
        }
        values.push_back(entry.get<std::string>());
    }
    return values;
}

std::vector<std::string> parseOptionalStringList(const nlohmann::json &json, std::string_view field,
                                                 std::string_view context) {
    if (!json.contains(field)) {
        return {};
    }
    return parseStringList(json.at(field), std::string{context} + "." + std::string{field});
}

void appendUnique(std::vector<std::string> &values, const std::string &value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

struct ComputeResourceReference {
    std::string authored;
    std::string name;
    bool history_read = false;
};

ComputeResourceReference parseComputeResourceReference(
    const std::string &authored, bool allow_history) {
    constexpr std::string_view history_suffix = "@history";
    if (authored.ends_with(history_suffix)) {
        const auto name = authored.substr(0, authored.size() - history_suffix.size());
        if (!allow_history || name.empty() || name.find('@') != std::string::npos) {
            throw std::runtime_error("Invalid compute history resource: " + authored);
        }
        return {authored, name, true};
    }
    if (authored.empty() || authored.find('@') != std::string::npos) {
        throw std::runtime_error("Unknown compute resource qualifier: " + authored);
    }
    return {authored, authored, false};
}

std::vector<ComputeResourceReference> taskResources(
    const ComputeTaskDefinition &definition) {
    std::vector<ComputeResourceReference> resources;
    const auto append = [&](const std::string &authored, bool allow_history) {
        auto resource = parseComputeResourceReference(authored, allow_history);
        const auto duplicate = std::find_if(
            resources.begin(), resources.end(), [&](const auto &candidate) {
                return candidate.name == resource.name &&
                       candidate.history_read == resource.history_read;
            });
        if (duplicate == resources.end()) {
            resources.push_back(std::move(resource));
        }
    };
    for (const auto &resource : definition.reads) {
        append(resource, true);
    }
    for (const auto &resource : definition.writes) {
        append(resource, false);
    }
    return resources;
}

ResolvedComputeResourceBinding resolveResource(
    const ComputeResourceReference &resource,
    RenderTargetContainer &render_target_container,
    FrameGraphResourceContainer &frame_graph_resources) {
    if (!resource.history_read) {
        const auto buffer =
            frame_graph_resources.getBufferIdByName(resource.name);
        if (isValidFrameGraphBufferId(buffer)) {
            return ResolvedComputeResourceBinding{
                .authored_name = resource.authored,
                .name = resource.name,
                .history_read = false,
                .render_target = noRenderTargetId(),
                .buffer = buffer,
            };
        }
    }
    const auto target =
        render_target_container.getRenderTargetIdByName(resource.name);
    if (!isConcreteRenderTarget(target) ||
        (resource.history_read &&
         !render_target_container.getMetadata(target).history)) {
        throw std::runtime_error(
            "Compute task resource not found or invalid: " +
            resource.authored);
    }
    return ResolvedComputeResourceBinding{
        .authored_name = resource.authored,
        .name = resource.name,
        .history_read = resource.history_read,
        .render_target = target,
        .buffer = noFrameGraphBufferId(),
    };
}

std::vector<ResolvedComputeResourceBinding> resolveTaskResources(
    const ComputeTaskDefinition &definition,
    RenderTargetContainer &render_target_container,
    FrameGraphResourceContainer &frame_graph_resources) {
    std::vector<ResolvedComputeResourceBinding> result;
    for (const auto &resource : taskResources(definition)) {
        result.push_back(resolveResource(
            resource, render_target_container,
            frame_graph_resources));
    }
    return result;
}

bool containsResource(
    std::span<const std::string> resources,
    std::string_view resource) {
    return std::find(
               resources.begin(), resources.end(),
               resource) != resources.end();
}

std::vector<ShaderResourceInterfaceBinding>
makeComputeResourceInterface(
    const ComputeTaskDefinition &definition,
    std::vector<ResolvedComputeResourceBinding> &resources,
    const ComputeTaskRuntimeDependencies &dependencies) {
    std::vector<ShaderResourceInterfaceBinding> result;
    result.reserve(definition.resource_ports.size());
    auto next_extra_binding =
        static_cast<std::uint32_t>(
            resources.size());
    for (std::size_t index = 0;
         index < resources.size(); ++index) {
        auto &resource = resources[index];
        std::size_t port_ordinal = 0;
        for (const auto &port :
             definition.resource_ports) {
            if (port.resource !=
                resource.authored_name) {
                continue;
            }
            const auto binding =
                port_ordinal++ == 0
                    ? static_cast<std::uint32_t>(
                          index)
                    : next_extra_binding++;
            if (isValidFrameGraphBufferId(
                    resource.buffer)) {
                if (port.kind !=
                        ShaderResourcePortKind::buffer ||
                    !port.buffer_element ||
                    port.subresource) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') resolves to a frame-graph buffer and requires "
                        "kind 'buffer', an explicit element, and no "
                        "subresource");
                }
                const auto written =
                    containsResource(
                        definition.writes,
                        resource.authored_name);
                const auto readable =
                    containsResource(
                        definition.reads,
                        resource.authored_name);
                if (effectiveShaderResourcePortAccess(
                        port, false, written) !=
                    ShaderResourcePortAccess::storage) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') requires storage access for a buffer");
                }
                result.push_back(
                    ShaderResourceInterfaceBinding{
                        .port = port,
                        .binding = binding,
                        .descriptor =
                            ShaderResourceDescriptorKind::
                                storage_buffer,
                        .image_view_dimension =
                            ReflectedImageViewDimension::
                                none,
                        .buffer_element =
                            *port.buffer_element,
                        .readable = readable,
                        .writable = written,
                    });
                continue;
            }
            if (!isConcreteRenderTarget(
                    resource.render_target)) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port.name + "' (resource '" +
                    port.resource +
                    "') does not resolve to an image");
            }
            if (port.kind ==
                ShaderResourcePortKind::buffer) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port.name + "' (resource '" +
                    port.resource +
                    "') declares a buffer but resolves to an image");
            }
            if (dependencies.resource_views ==
                nullptr) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port.name + "' (resource '" +
                    port.resource +
                    "') requires a physical target-plan view");
            }
            const auto physical =
                dependencies.resource_views->find(
                    resource.name);
            if (physical ==
                dependencies.resource_views->end()) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port.name + "' (resource '" +
                    port.resource +
                    "') is absent from the physical target plan");
            }
            resource.physical_view =
                physical->second;
            if (physical->second ==
                VulkanResourceViewLayout::shared_2d) {
                resource.physical_view_count = 1;
            } else {
                if (dependencies.resource_view_counts ==
                    nullptr) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') requires a physical target-plan view count");
                }
                const auto count =
                    dependencies.resource_view_counts
                        ->find(resource.name);
                if (count ==
                        dependencies
                            .resource_view_counts->end() ||
                    count->second == 0) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') is missing its physical target-plan view "
                        "count");
                }
                resource.physical_view_count =
                    count->second;
            }

            const auto metadata =
                dependencies.render_target_container
                    .getMetadata(
                        resource.render_target);
            const auto written =
                containsResource(
                    definition.writes,
                    resource.authored_name);
            const auto readable =
                containsResource(
                    definition.reads,
                    resource.authored_name);
            const auto access =
                effectiveShaderResourcePortAccess(
                    port, true, written);
            const auto sampled =
                access ==
                ShaderResourcePortAccess::sampled;
            const auto required_usage =
                sampled
                    ? vk::ImageUsageFlagBits::eSampled
                    : vk::ImageUsageFlagBits::eStorage;
            if (!(metadata.usage &
                  required_usage)) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port.name + "' (resource '" +
                    port.resource +
                    "') requires render-target usage " +
                    std::string{
                        sampled ? "sampled"
                                : "storage"});
            }
            const auto dimension =
                resolveShaderResourceImageViewDimension(
                    port, physical->second,
                    definition.schedule ==
                            ComputeTaskSchedule::
                                per_view
                        ? ShaderResourceConsumerView::
                              compute_per_view
                        : ShaderResourceConsumerView::
                              compute_once,
                    metadata.dimension);
            if (dimension ==
                    ReflectedImageViewDimension::
                        two_d_array &&
                metadata.array_layers <
                    resource.physical_view_count) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port.name + "' (resource '" +
                    port.resource +
                    "') has too few layers for its logical view family "
                    "(physical_layers=" +
                    std::to_string(
                        metadata.array_layers) +
                    ", logical_views=" +
                    std::to_string(
                        resource
                            .physical_view_count) +
                    ")");
            }
            if (dimension ==
                    ReflectedImageViewDimension::cube &&
                !sampled) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port.name + "' (resource '" +
                    port.resource +
                    "') cube view currently requires sampled "
                    "access");
            }
            auto resolved_port = port;
            if (resolved_port.subresource &&
                dimension ==
                    ReflectedImageViewDimension::
                        two_d_array &&
                (resolved_port.view ==
                     ShaderResourcePortView::per_view ||
                 resolved_port.view ==
                     ShaderResourcePortView::
                         family_array)) {
                if (resolved_port.subresource
                        ->layer_count == 1) {
                    if (resolved_port.subresource
                            ->base_array_layer != 0) {
                        throw std::runtime_error(
                            "Shader resource port '" +
                            port.name + "' (resource '" +
                            port.resource +
                            "') expandable family subresource must "
                            "start at array layer zero");
                    }
                    resolved_port.subresource
                        ->layer_count =
                        resource
                            .physical_view_count;
                } else if (
                    resolved_port.subresource
                            ->layer_count !=
                        resource
                            .physical_view_count) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') family image subresource must select one "
                        "expandable layer or exactly the logical view count");
                }
            }
            if (resolved_port.subresource) {
                if (!validImageSubresourceRange(
                        *resolved_port.subresource,
                        metadata.mip_levels,
                        metadata.array_layers)) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') has an out-of-range image subresource");
                }
                if (!sampled &&
                    (resolved_port.subresource
                             ->mip_count_mode !=
                         ImageSubresourceMipCountMode::
                             fixed ||
                     resolved_port.subresource
                             ->level_count != 1)) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') storage view requires exactly one mip level");
                }
                if (dimension ==
                        ReflectedImageViewDimension::
                            two_d &&
                    resolved_port.subresource
                            ->layer_count != 1) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') 2D view requires exactly one array layer");
                }
                if (dimension ==
                        ReflectedImageViewDimension::
                            two_d_array &&
                    resolved_port.subresource
                            ->layer_count !=
                        resource.physical_view_count) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') family image subresource must select exactly "
                        "the logical view count");
                }
                if (dimension ==
                        ReflectedImageViewDimension::cube &&
                    (resolved_port.subresource
                             ->base_array_layer != 0 ||
                     resolved_port.subresource
                             ->layer_count != 6)) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') cube view must select all six faces");
                }
            }
            result.push_back(
                ShaderResourceInterfaceBinding{
                    .port =
                        std::move(
                            resolved_port),
                    .binding = binding,
                    .descriptor =
                        sampled
                            ? ShaderResourceDescriptorKind::
                                  combined_image_sampler
                            : ShaderResourceDescriptorKind::
                                  storage_image,
                    .image_view_dimension = dimension,
                    .storage_format =
                        sampled
                            ? vk::Format::eUndefined
                            : metadata.format,
                    .readable =
                        sampled ? true : readable,
                    .writable =
                        sampled ? false : written,
                });
        }
    }
    if (result.size() !=
        definition.resource_ports.size()) {
        throw std::runtime_error(
            "compute task resource port did not resolve to a declared resource");
    }
    return result;
}

const ResolvedComputeResourceBinding &resourceForBinding(
    const ReflectedBinding &binding, size_t binding_index,
    const std::vector<ResolvedComputeResourceBinding> &resources) {
    if (!binding.name.empty()) {
        const auto named = std::find_if(
            resources.begin(), resources.end(), [&](const auto &resource) {
                return resource.name == binding.name;
            });
        if (named != resources.end() &&
            std::none_of(std::next(named), resources.end(), [&](const auto &resource) {
                return resource.name == binding.name;
            })) {
            return *named;
        }
    }
    if (resources.size() == 1) {
        return resources.front();
    }
    if (binding_index < resources.size()) {
        return resources[binding_index];
    }
    throw std::runtime_error("Compute task descriptor binding does not map to a declared resource: " +
                             binding.name);
}

const ShaderResourceInterfaceBinding *interfaceForBinding(
    std::span<const ShaderResourceInterfaceBinding> resource_interface,
    std::uint32_t binding) {
    const auto found = std::find_if(
        resource_interface.begin(),
        resource_interface.end(),
        [&](const ShaderResourceInterfaceBinding &candidate) {
            return candidate.binding == binding;
        });
    return found == resource_interface.end()
               ? nullptr
               : &*found;
}

const ResolvedComputeResourceBinding &resourceForInterface(
    const ShaderResourceInterfaceBinding &interface_binding,
    std::span<const ResolvedComputeResourceBinding> resources) {
    const auto found = std::find_if(
        resources.begin(), resources.end(),
        [&](const ResolvedComputeResourceBinding &candidate) {
            return candidate.authored_name ==
                   interface_binding.port.resource;
        });
    if (found == resources.end()) {
        throw std::runtime_error(
            "Shader resource port '" +
            interface_binding.port.name +
            "' (resource '" +
            interface_binding.port.resource +
            "') has no resolved compute resource");
    }
    return *found;
}

struct ResolvedImageExtentDispatch {
    GlobalRenderTargetId render_target =
        noRenderTargetId();
    std::uint32_t mip_level = 0;
};

ResolvedImageExtentDispatch resolveImageExtentDispatch(
    const ComputeTaskDefinition &definition,
    std::span<const ResolvedComputeResourceBinding>
        resources,
    const RenderTargetContainer
        &render_target_container) {
    const auto ray_tracing =
        definition.dispatch.rays_from.has_value();
    const auto &authored = ray_tracing
                               ? *definition.dispatch.rays_from
                               : *definition.dispatch.groups_from;
    const auto dispatch_name = ray_tracing
                                   ? "dispatch.rays_from"
                                   : "dispatch.groups_from";
    const auto port = std::find_if(
        definition.resource_ports.begin(),
        definition.resource_ports.end(),
        [&](const ShaderResourcePortDefinition
                &candidate) {
            return candidate.name == authored.port;
        });
    if (port == definition.resource_ports.end()) {
        throw std::runtime_error(
            "Compute task '" + definition.name +
            "' " + std::string{dispatch_name} +
            " references unknown resource "
            "port '" +
            authored.port + "'");
    }
    if (port->kind ==
        ShaderResourcePortKind::buffer) {
        throw std::runtime_error(
            "Compute task '" + definition.name +
            "' " + std::string{dispatch_name} + " port '" +
            authored.port +
            "' must resolve to an image");
    }
    const auto resource = std::find_if(
        resources.begin(), resources.end(),
        [&](const ResolvedComputeResourceBinding
                &candidate) {
            return candidate.authored_name ==
                   port->resource;
        });
    if (resource == resources.end() ||
        !isConcreteRenderTarget(
            resource->render_target)) {
        throw std::runtime_error(
            "Compute task '" + definition.name +
            "' " + std::string{dispatch_name} + " port '" +
            authored.port +
            "' does not resolve to a render target");
    }
    const auto mip_level =
        port->subresource
            ? port->subresource
                  ->base_mip_level
            : 0u;
    const auto metadata =
        render_target_container.getMetadata(
            resource->render_target);
    if (mip_level >= metadata.mip_levels) {
        throw std::runtime_error(
            "Compute task '" + definition.name +
            "' dispatch.groups_from port '" +
            authored.port + "' selects mip " +
            std::to_string(mip_level) +
            " outside render target '" +
            metadata.name + "' (" +
            std::to_string(metadata.mip_levels) +
            " mip levels)");
    }
    return ResolvedImageExtentDispatch{
        .render_target =
            resource->render_target,
        .mip_level = mip_level,
    };
}

std::array<std::uint32_t, 3>
imageExtentDispatchGroups(
    const ComputeTaskDefinition &definition,
    const ResolvedImageExtentDispatch &dispatch,
    const RenderTargetContainer
        &render_target_container,
    const glm::uvec3 &local_size) {
    if (local_size.x == 0 ||
        local_size.y == 0 ||
        local_size.z == 0) {
        throw std::runtime_error(
            "Compute task '" + definition.name +
            "' uses dispatch.groups_from but its shader has no "
            "reflected workgroup size");
    }
    if (local_size.z != 1) {
        throw std::runtime_error(
            "Compute task '" + definition.name +
            "' image extent dispatch requires shader "
            "local_size_z = 1");
    }
    const auto metadata =
        render_target_container.getMetadata(
            dispatch.render_target);
    if (dispatch.mip_level >=
        metadata.mip_levels) {
        throw std::runtime_error(
            "Compute task '" + definition.name +
            "' image extent dispatch mip is outside render target '" +
            metadata.name + "'");
    }
    const auto mip_dimension =
        [mip = dispatch.mip_level](
            std::uint32_t dimension) {
            if (mip >=
                std::numeric_limits<
                    std::uint32_t>::digits) {
                return 1u;
            }
            return std::max(
                1u, dimension >> mip);
        };
    const auto ceil_divide =
        [](std::uint32_t value,
           std::uint32_t divisor) {
            return static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(
                     value) +
                 divisor - 1u) /
                divisor);
        };
    return {
        ceil_divide(
            mip_dimension(
                metadata.extent.width),
            local_size.x),
        ceil_divide(
            mip_dimension(
                metadata.extent.height),
            local_size.y),
        1u,
    };
}

vk::SamplerAddressMode samplerAddressMode(
    ShaderResourcePortAddressMode mode) {
    switch (mode) {
    case ShaderResourcePortAddressMode::repeat:
        return vk::SamplerAddressMode::eRepeat;
    case ShaderResourcePortAddressMode::mirrored_repeat:
        return vk::SamplerAddressMode::eMirroredRepeat;
    case ShaderResourcePortAddressMode::clamp_to_edge:
        return vk::SamplerAddressMode::eClampToEdge;
    }
    throw std::runtime_error(
        "unknown shader resource port sampler address mode");
}

std::size_t samplerIndex(
    ShaderResourcePortSampling sampling) {
    constexpr std::size_t address_mode_count = 3;
    return static_cast<std::size_t>(sampling.filter) *
               address_mode_count +
           static_cast<std::size_t>(
               sampling.address_mode);
}

vk::UniqueSampler createSampler(
    vk::Device device,
    ShaderResourcePortSampling sampling) {
    const auto filter =
        sampling.filter ==
                ShaderResourcePortFilter::nearest
            ? vk::Filter::eNearest
            : vk::Filter::eLinear;
    const auto address =
        samplerAddressMode(sampling.address_mode);
    vk::SamplerCreateInfo create_info;
    create_info.magFilter = filter;
    create_info.minFilter = filter;
    create_info.mipmapMode =
        vk::SamplerMipmapMode::eLinear;
    create_info.addressModeU = address;
    create_info.addressModeV = address;
    create_info.addressModeW = address;
    create_info.maxLod = VK_LOD_CLAMP_NONE;
    return device.createSamplerUnique(create_info);
}

template <typename T>
void deferOrDestroy(T &&resource) noexcept {
    try {
        auto *queue = FastModuleContainer::tryGet<DeletionQueue>();
        if (queue != nullptr &&
            queue->acceptingResources()) {
            queue->defer(std::forward<T>(resource));
        }
    } catch (...) {
        // The moved-from value (or the original value when defer failed)
        // is destroyed by the caller's stack during teardown.
    }
}

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
    std::array<vk::DescriptorPoolSize, 3> pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, max_compute_descriptors},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, max_compute_descriptors},
        vk::DescriptorPoolSize{
            vk::DescriptorType::eCombinedImageSampler,
            max_compute_descriptors},
    };

    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = max_compute_descriptor_sets;
    create_info.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

std::vector<ReflectedBinding> passInputBindings(const ShaderReflection &reflection) {
    std::vector<ReflectedBinding> bindings;
    for (const auto &binding : reflection.bindings) {
        if (binding.set == PELICAN_SET_PASS_INPUT) {
            bindings.push_back(binding);
        }
    }
    std::sort(bindings.begin(), bindings.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.binding < rhs.binding;
    });
    return bindings;
}

vk::PipelineStageFlags shaderStage(FramePlanNodeKind kind) {
    if (kind == FramePlanNodeKind::compute) {
        vk::PipelineStageFlags stages =
            vk::PipelineStageFlagBits::eComputeShader;
        if (GET_MODULE(VulkanManageCore)
                .getRuntimeCapabilities()
                .ray_tracing_pipeline) {
            stages |= vk::PipelineStageFlagBits::
                eRayTracingShaderKHR;
        }
        return stages;
    }
    return vk::PipelineStageFlagBits::eVertexShader |
           vk::PipelineStageFlagBits::eFragmentShader;
}

std::vector<ShaderReference> parseRayTracingShaderList(
    const nlohmann::json &pipeline_json,
    std::string_view field, ShaderStage stage,
    const std::string &name) {
    const auto found = pipeline_json.find(field);
    if (found == pipeline_json.end()) {
        throw std::runtime_error(
            "pelican.ray_tracing.shader_stage_required@1: compute task '" +
            name + "' requires ray_tracing." +
            std::string{field});
    }
    std::vector<ShaderReference> result;
    const auto append = [&](const nlohmann::json &value) {
        if (!value.is_string() ||
            value.get_ref<const std::string &>().empty()) {
            throw std::runtime_error(
                "pelican.ray_tracing.invalid_shader_reference@1: compute "
                "task '" +
                name + "' ray_tracing." + std::string{field} +
                " must contain non-empty strings");
        }
        result.push_back(makeShaderReference(
            value.get<std::string>(), stage));
    };
    if (found->is_string()) {
        append(*found);
    } else if (found->is_array()) {
        for (const auto &value : *found) append(value);
    } else {
        throw std::runtime_error(
            "pelican.ray_tracing.invalid_shader_reference@1: compute "
            "task '" +
            name + "' ray_tracing." + std::string{field} +
            " must be a string or string array");
    }
    if (result.empty()) {
        throw std::runtime_error(
            "pelican.ray_tracing.shader_stage_required@1: compute task '" +
            name + "' requires at least one ray_tracing." +
            std::string{field} + " shader");
    }
    return result;
}

std::array<std::uint32_t, 3>
imageExtentTraceDimensions(
    const ComputeTaskDefinition &definition,
    const ResolvedImageExtentDispatch &dispatch,
    const RenderTargetContainer &render_target_container) {
    const auto metadata = render_target_container.getMetadata(
        dispatch.render_target);
    if (dispatch.mip_level >= metadata.mip_levels) {
        throw std::runtime_error(
            "Ray tracing task '" + definition.name +
            "' image extent dispatch mip is outside render target '" +
            metadata.name + "'");
    }
    const auto mip_dimension =
        [mip = dispatch.mip_level](std::uint32_t dimension) {
            if (mip >=
                std::numeric_limits<std::uint32_t>::digits) {
                return 1u;
            }
            return std::max(1u, dimension >> mip);
        };
    return {mip_dimension(metadata.extent.width),
            mip_dimension(metadata.extent.height), 1u};
}

RayTracingTaskShaderDefinition parseRayTracingShaders(
    const nlohmann::json &task_json,
    const std::string &name) {
    const auto &pipeline_json = task_json.at("ray_tracing");
    if (!pipeline_json.is_object()) {
        throw std::runtime_error(
            "pelican.ray_tracing.invalid_pipeline_declaration@1: compute "
            "task '" +
            name + "' ray_tracing must be an object");
    }
    for (auto field = pipeline_json.begin();
         field != pipeline_json.end(); ++field) {
        if (field.key() == "raygen" || field.key() == "miss" ||
            field.key() == "closesthit") {
            continue;
        }
        if (field.key() == "any_hit" || field.key() == "anyhit" ||
            field.key() == "intersection" ||
            field.key() == "callable") {
            throw std::runtime_error(
                "pelican.ray_tracing.unsupported_shader_stage@1: compute "
                "task '" +
                name + "' declares unsupported ray tracing stage '" +
                field.key() + "'");
        }
        throw std::runtime_error(
            "pelican.ray_tracing.unknown_shader_stage@1: compute task '" +
            name + "' declares unknown ray tracing field '" +
            field.key() + "'");
    }
    return RayTracingTaskShaderDefinition{
        .raygen = makeShaderReference(
            requireString(
                pipeline_json, "raygen",
                "compute task ray_tracing: " + name),
            ShaderStage::raygen),
        .misses = parseRayTracingShaderList(
            pipeline_json, "miss", ShaderStage::miss, name),
        .closest_hits = parseRayTracingShaderList(
            pipeline_json, "closesthit",
            ShaderStage::closesthit, name),
    };
}

ComputeDispatchDefinition parseDispatch(
    const nlohmann::json &task_json, const std::string &name,
    bool ray_tracing) {
    ComputeDispatchDefinition dispatch;
    if (!task_json.contains("dispatch")) {
        if (ray_tracing) {
            throw std::runtime_error(
                "pelican.ray_tracing.trace_extent_required@1: compute "
                "task '" +
                name + "' requires dispatch.rays_from");
        }
        return dispatch;
    }

    const auto &json = task_json.at("dispatch");
    if (!json.is_object()) {
        throw std::runtime_error("compute task dispatch must be an object: " + name);
    }

    if (json.contains("rays_from")) {
        if (!ray_tracing) {
            throw std::runtime_error(
                "pelican.ray_tracing.rays_from_requires_pipeline@1: "
                "dispatch.rays_from requires a ray_tracing task: " +
                name);
        }
        if (json.size() != 1) {
            throw std::runtime_error(
                "pelican.ray_tracing.invalid_trace_dispatch@1: "
                "dispatch.rays_from cannot be combined with compute "
                "dispatch fields: " +
                name);
        }
        const auto &rays_from = json.at("rays_from");
        if (!rays_from.is_object() || rays_from.size() != 1 ||
            !rays_from.contains("port")) {
            throw std::runtime_error(
                "pelican.ray_tracing.invalid_trace_dispatch@1: "
                "dispatch.rays_from must contain exactly one image port: " +
                name);
        }
        auto port = requireString(
            rays_from, "port",
            "compute task dispatch.rays_from: " + name);
        if (port.empty()) {
            throw std::runtime_error(
                "pelican.ray_tracing.invalid_trace_dispatch@1: "
                "dispatch.rays_from.port must not be empty: " + name);
        }
        dispatch.rays_from =
            ComputeImageExtentDispatchDefinition{
                .port = std::move(port)};
        return dispatch;
    }
    if (ray_tracing) {
        throw std::runtime_error(
            "pelican.ray_tracing.trace_extent_required@1: compute task '" +
            name + "' requires dispatch.rays_from");
    }

    if (json.contains("indirect")) {
        if (json.contains("groups") ||
            json.contains("groups_from") ||
            json.contains("local_size")) {
            throw std::runtime_error(
                "compute task dispatch.indirect cannot be combined with "
                "groups, groups_from, or local_size: " +
                name);
        }
        const auto &indirect = json.at("indirect");
        if (!indirect.is_object()) {
            throw std::runtime_error(
                "compute task dispatch.indirect must be an object: " +
                name);
        }
        ComputeIndirectDispatchDefinition definition{
            .buffer = requireString(
                indirect, "buffer",
                "compute task dispatch.indirect: " +
                    name),
            .offset =
                indirect.contains("offset")
                    ? requireDeviceSize(
                          indirect, "offset",
                          "compute task dispatch.indirect: " +
                              name)
                    : vk::DeviceSize{0},
        };
        if (definition.buffer.empty()) {
            throw std::runtime_error(
                "compute task dispatch.indirect buffer must not be empty: " +
                name);
        }
        if (definition.offset %
                frameGraphIndirectCommandAlignment !=
            0) {
            throw std::runtime_error(
                "compute task dispatch.indirect offset must be "
                "4-byte aligned: " +
                name);
        }
        dispatch.indirect = std::move(definition);
        return dispatch;
    }

    if (json.contains("groups") &&
        json.contains("groups_from")) {
        throw std::runtime_error(
            "compute task dispatch.groups cannot be combined with "
            "groups_from: " +
            name);
    }
    if (json.contains("local_size")) {
        throw std::runtime_error(
            "compute task dispatch.local_size is not authored; "
            "declare the workgroup size in the compute shader: " +
            name);
    }
    if (json.contains("groups")) {
        const auto &groups = json.at("groups");
        if (groups.is_array()) {
            if (groups.size() != 3) {
                throw std::runtime_error("compute task dispatch.groups must have three entries: " + name);
            }
            for (const auto &entry : groups) {
                if (!entry.is_number_integer()) {
                    throw std::runtime_error("compute task dispatch.groups entries must be integers: " + name);
                }
            }
            dispatch.groups_x = groups.at(0).get<uint32_t>();
            dispatch.groups_y = groups.at(1).get<uint32_t>();
            dispatch.groups_z = groups.at(2).get<uint32_t>();
        } else if (groups.is_object()) {
            dispatch.groups_x = groups.value("x", 1u);
            dispatch.groups_y = groups.value("y", 1u);
            dispatch.groups_z = groups.value("z", 1u);
        } else {
            throw std::runtime_error("compute task dispatch.groups must be an array or object: " + name);
        }
    }
    if (json.contains("groups_from")) {
        const auto &groups_from =
            json.at("groups_from");
        if (!groups_from.is_object()) {
            throw std::runtime_error(
                "compute task dispatch.groups_from must be an "
                "object containing an image resource port: " +
                name);
        }
        for (auto field = groups_from.begin();
             field != groups_from.end(); ++field) {
            if (field.key() != "port") {
                throw std::runtime_error(
                    "compute task dispatch.groups_from has unknown "
                    "field '" +
                    field.key() + "': " + name);
            }
        }
        auto port = requireString(
            groups_from, "port",
            "compute task dispatch.groups_from: " +
                name);
        if (port.empty()) {
            throw std::runtime_error(
                "compute task dispatch.groups_from.port must not be "
                "empty: " +
                name);
        }
        dispatch.groups_from =
            ComputeImageExtentDispatchDefinition{
                .port = std::move(port),
            };
    }
    if (dispatch.groups_x == 0 || dispatch.groups_y == 0 || dispatch.groups_z == 0) {
        throw std::runtime_error("compute task dispatch group counts must be positive: " + name);
    }
    return dispatch;
}

} // namespace

std::string_view frameGraphHostBufferSourceName(
    FrameGraphHostBufferSource source) {
    switch (source) {
    case FrameGraphHostBufferSource::scene_lights_v2:
        return "scene_lights_v2";
    case FrameGraphHostBufferSource::
        scene_draw_commands_v1:
        return "scene_draw_commands_v1";
    case FrameGraphHostBufferSource::
        scene_draw_bounds_v1:
        return "scene_draw_bounds_v1";
    case FrameGraphHostBufferSource::
        scene_draw_segments_v1:
        return "scene_draw_segments_v1";
    }
    throw std::runtime_error(
        "unknown frame-graph host buffer source");
}

std::string_view frameGraphBufferCommandLayoutName(
    FrameGraphBufferCommandLayout layout) {
    switch (layout) {
    case FrameGraphBufferCommandLayout::
        compute_dispatch:
        return "compute_dispatch";
    case FrameGraphBufferCommandLayout::
        indexed_draw:
        return "indexed_draw";
    case FrameGraphBufferCommandLayout::
        draw_count:
        return "draw_count";
    }
    throw std::runtime_error(
        "unknown frame-graph buffer command layout");
}

std::string_view computeTaskScheduleName(
    ComputeTaskSchedule schedule) {
    switch (schedule) {
    case ComputeTaskSchedule::per_frame:
        return "per_frame";
    case ComputeTaskSchedule::per_view:
        return "per_view";
    }
    throw std::runtime_error(
        "unknown compute task schedule");
}

std::vector<FrameGraphBufferDefinition> parseFrameGraphBufferDefinitionsFromJson(const nlohmann::json &config_json) {
    std::vector<FrameGraphBufferDefinition> definitions;
    if (!config_json.contains("buffers")) {
        return definitions;
    }
    const auto &buffers = config_json.at("buffers");
    if (!buffers.is_array()) {
        throw std::runtime_error("buffers must be an array");
    }

    definitions.reserve(buffers.size());
    for (const auto &entry : buffers) {
        if (entry.is_string()) {
            definitions.push_back(
                FrameGraphBufferDefinition{
                    entry.get<std::string>(),
                    0, true});
            continue;
        }
        if (!entry.is_object()) {
            throw std::runtime_error("buffers entries must be strings or objects");
        }
        const auto name =
            requireString(entry, "name", "buffer");
        const auto context = "buffer '" + name + "'";
        if (entry.contains("size") &&
            entry.contains("size_from_extent")) {
            throw std::runtime_error(
                context +
                " cannot declare both size and size_from_extent");
        }
        FrameGraphBufferDefinition definition{
            .name = name,
            .size =
                entry.contains("size")
                    ? requireDeviceSize(
                          entry, "size", context)
                    : vk::DeviceSize{0},
            .persistent = true,
        };
        if (entry.contains("size_from_extent")) {
            auto [extent_size, byte_size] =
                parseExtentDerivedBufferSize(
                    entry, config_json, context);
            definition.extent_size =
                std::move(extent_size);
            definition.size = byte_size;
        }
        if (entry.contains("host_source")) {
            definition.host_source =
                parseHostBufferSource(
                    entry, context);
        }
        if (entry.contains("command_layout")) {
            definition.command_layout =
                parseBufferCommandLayout(
                    entry, context);
        }
        if (entry.contains("lifetime")) {
            const auto lifetime =
                requireString(
                    entry, "lifetime", context);
            if (lifetime == "persistent") {
                definition.persistent = true;
            } else if (lifetime == "transient") {
                definition.persistent = false;
            } else {
                throw std::runtime_error("buffer lifetime must be persistent or transient: " + definition.name);
            }
        }
        if (definition.host_source &&
            definition.size == 0) {
            throw std::runtime_error(
                context +
                " host_source requires a non-zero size");
        }
        if (definition.command_layout &&
            *definition.command_layout ==
                FrameGraphBufferCommandLayout::
                    compute_dispatch &&
            definition.size <
                compute_dispatch_command_size) {
            throw std::runtime_error(
                context +
                " command_layout 'compute_dispatch' requires at "
                "least " +
                std::to_string(
                    compute_dispatch_command_size) +
                " bytes");
        }
        if (definition.command_layout ==
                FrameGraphBufferCommandLayout::
                    indexed_draw &&
            definition.size <
                frameGraphIndexedDrawCommandBytes) {
            throw std::runtime_error(
                context +
                " command_layout 'indexed_draw' requires at least " +
                std::to_string(
                    frameGraphIndexedDrawCommandBytes) +
                " bytes");
        }
        if (definition.command_layout ==
                FrameGraphBufferCommandLayout::
                    draw_count &&
            definition.size <
                frameGraphDrawCountBytes) {
            throw std::runtime_error(
                context +
                " command_layout 'draw_count' requires at least " +
                std::to_string(
                    frameGraphDrawCountBytes) +
                " bytes");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_commands_v1 &&
            definition.command_layout !=
                FrameGraphBufferCommandLayout::
                    indexed_draw) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_commands_v1' requires "
                "command_layout 'indexed_draw'");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_commands_v1 &&
            definition.size %
                    frameGraphIndexedDrawCommandBytes !=
                0) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_commands_v1' size must "
                "be a multiple of the indexed draw command size");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_bounds_v1 &&
            definition.command_layout) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_bounds_v1' must not declare "
                "a command_layout");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_bounds_v1 &&
            definition.size %
                    frameGraphSceneDrawBoundsV1Bytes !=
                0) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_bounds_v1' size must be a "
                "multiple of 32 bytes");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_segments_v1 &&
            definition.command_layout) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_segments_v1' must not declare "
                "a command_layout");
        }
        if (definition.host_source ==
                FrameGraphHostBufferSource::
                    scene_draw_segments_v1 &&
            definition.size %
                    frameGraphSceneDrawSegmentV1Bytes !=
                0) {
            throw std::runtime_error(
                context +
                " host_source 'scene_draw_segments_v1' size must be a "
                "multiple of 32 bytes");
        }
        definitions.push_back(std::move(definition));
    }
    return definitions;
}

std::unordered_set<std::string> frameGraphBufferNameSet(const std::vector<FrameGraphBufferDefinition> &definitions) {
    std::unordered_set<std::string> names;
    for (const auto &definition : definitions) {
        if (!names.insert(definition.name).second) {
            throw std::runtime_error("Duplicate buffer name: " + definition.name);
        }
    }
    return names;
}

std::vector<ComputeTaskDefinition> parseComputeTaskDefinitionsFromConfigJson(const nlohmann::json &config_json) {
    std::vector<ComputeTaskDefinition> definitions;
    if (!config_json.contains("compute_tasks")) {
        return definitions;
    }
    const auto &tasks = config_json.at("compute_tasks");
    if (!tasks.is_array()) {
        throw std::runtime_error("compute_tasks must be an array");
    }

    definitions.reserve(tasks.size());
    for (const auto &task_json : tasks) {
        if (!task_json.is_object()) {
            throw std::runtime_error("compute_tasks entries must be objects");
        }
        ComputeTaskDefinition definition;
        definition.name = requireString(task_json, "name", "compute task");
        for (const auto *unsupported :
             {"any_hit", "anyhit", "intersection", "callable"}) {
            if (task_json.contains(unsupported)) {
                throw std::runtime_error(
                    "pelican.ray_tracing.unsupported_shader_stage@1: "
                    "compute task '" +
                    definition.name +
                    "' declares unsupported top-level ray tracing stage '" +
                    unsupported + "'");
            }
        }
        const auto ray_tracing =
            task_json.contains("ray_tracing");
        if (ray_tracing && task_json.contains("shader")) {
            throw std::runtime_error(
                "pelican.ray_tracing.ambiguous_pipeline_declaration@1: "
                "compute task '" +
                definition.name +
                "' cannot declare both shader and ray_tracing");
        }
        if (ray_tracing) {
            definition.ray_tracing =
                parseRayTracingShaders(
                    task_json, definition.name);
        } else {
            definition.shader = makeShaderReference(
                requireString(
                    task_json, "shader",
                    "compute task: " + definition.name),
                ShaderStage::compute);
        }
        definition.reads = parseOptionalStringList(task_json, "reads", "compute task: " + definition.name);
        definition.writes = parseOptionalStringList(task_json, "writes", "compute task: " + definition.name);
        definition.after = parseOptionalStringList(task_json, "after", "compute task: " + definition.name);
        definition.before = parseOptionalStringList(task_json, "before", "compute task: " + definition.name);
        definition.view_family =
            parseRenderViewFamilyId(
                task_json,
                "Compute task '" +
                    definition.name + "'");
        definition.resource_ports =
            parseShaderResourcePortDefinitions(
                task_json, definition.reads,
                definition.writes,
                "compute task '" + definition.name + "'");
        definition.dispatch = parseDispatch(
            task_json, definition.name, ray_tracing);
        const auto extent_from =
            definition.dispatch.groups_from
                ? definition.dispatch.groups_from
                : definition.dispatch.rays_from;
        if (extent_from) {
            const auto &port_name =
                extent_from->port;
            const auto port = std::find_if(
                definition.resource_ports.begin(),
                definition.resource_ports.end(),
                [&](const ShaderResourcePortDefinition
                        &candidate) {
                    return candidate.name ==
                           port_name;
                });
            if (port ==
                definition.resource_ports.end()) {
                throw std::runtime_error(
                    "compute task image extent dispatch references "
                    "unknown resource port '" +
                    port_name + "': " +
                    definition.name);
            }
            if (port->kind ==
                ShaderResourcePortKind::buffer) {
                throw std::runtime_error(
                    "compute task image extent dispatch port must be "
                    "an image: " +
                    definition.name);
            }
        }
        if (task_json.contains("schedule")) {
            const auto schedule =
                requireString(
                    task_json, "schedule",
                    "compute task: " +
                        definition.name);
            if (schedule == "per_frame") {
                definition.schedule =
                    ComputeTaskSchedule::per_frame;
            } else if (schedule == "per_view") {
                definition.schedule =
                    ComputeTaskSchedule::per_view;
            } else {
                throw std::runtime_error(
                    "compute task schedule supports per_frame or "
                    "per_view: " +
                    definition.name);
            }
        }
        definitions.push_back(std::move(definition));
    }
    return definitions;
}

void validateComputeTaskBufferContracts(
    std::span<const FrameGraphBufferDefinition>
        buffer_definitions,
    std::span<const ComputeTaskDefinition>
        task_definitions) {
    for (const auto &task : task_definitions) {
        if (!task.dispatch.indirect) continue;
        const auto &indirect =
            *task.dispatch.indirect;
        if (std::find(
                task.writes.begin(),
                task.writes.end(),
                indirect.buffer) !=
            task.writes.end()) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' cannot write its own indirect dispatch "
                "buffer; use a separate producer task");
        }
        const auto buffer = std::find_if(
            buffer_definitions.begin(),
            buffer_definitions.end(),
            [&](const FrameGraphBufferDefinition
                    &candidate) {
                return candidate.name ==
                       indirect.buffer;
            });
        if (buffer == buffer_definitions.end()) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' indirect dispatch references unknown buffer '" +
                indirect.buffer + "'");
        }
        if (!buffer->command_layout ||
            *buffer->command_layout !=
                FrameGraphBufferCommandLayout::
                    compute_dispatch) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' indirect dispatch buffer '" +
                indirect.buffer +
                "' requires command_layout "
                "'compute_dispatch'");
        }
        if (indirect.offset %
                frameGraphIndirectCommandAlignment !=
            0) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' indirect dispatch offset must be 4-byte "
                "aligned");
        }
        if (indirect.offset > buffer->size ||
            buffer->size - indirect.offset <
                compute_dispatch_command_size) {
            throw std::runtime_error(
                "compute task '" + task.name +
                "' indirect dispatch command at offset " +
                std::to_string(indirect.offset) +
                " exceeds buffer '" +
                indirect.buffer + "' size " +
                std::to_string(buffer->size));
        }
    }
}

FrameGraphResourceContainer::FrameGraphResourceContainer() = default;

FrameGraphResourceContainer::~FrameGraphResourceContainer() = default;

void FrameGraphResourceContainer::registerBuffers(const std::vector<FrameGraphBufferDefinition> &definitions) {
    auto &vkcore = GET_MODULE(VulkanManageCore);
    for (const auto &definition : definitions) {
        if (definition.name.empty()) {
            throw std::runtime_error("buffer name must not be empty");
        }
        if (definition.size == 0) {
            continue;
        }
        if (name_to_id.contains(definition.name)) {
            continue;
        }
        const auto maximum_range =
            static_cast<vk::DeviceSize>(
                vkcore.getPhysDevice()
                    .getProperties()
                    .limits.maxStorageBufferRange);
        if (definition.size > maximum_range) {
            throw std::runtime_error(
                "Frame graph buffer '" + definition.name +
                "' requires " +
                std::to_string(definition.size) +
                " bytes, exceeding device maxStorageBufferRange " +
                std::to_string(maximum_range));
        }
        registration_order.reserve(
            registration_order.size() + 1);
        auto usage =
            vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc |
            vk::BufferUsageFlagBits::eTransferDst;
        if (definition.command_layout) {
            usage |= vk::BufferUsageFlagBits::
                eIndirectBuffer;
        }
        auto buffer = vkcore.allocBuf(
            definition.size,
            usage,
            definition.host_source
                ? vma::MemoryUsage::eAuto
                : vma::MemoryUsage::eAutoPreferDevice,
            definition.host_source
                ? vma::AllocationCreateFlagBits::
                      eHostAccessSequentialWrite
                : vma::AllocationCreateFlags{});
        vkcore.getDebugUtils().nameBuffer(
            buffer.buffer.get(),
            ("frame_graph/buffer/" +
             definition.name)
                .c_str());
        const auto id = buffers.reg(
            BufferRecord{definition, std::move(buffer)});
        try {
            if (!name_to_id.emplace(definition.name, id).second) {
                throw std::runtime_error(
                    "Frame graph buffer name table changed during registration: " +
                    definition.name);
            }
        } catch (...) {
            (void)buffers.extract(id, false);
            throw;
        }
        registration_order.push_back(id);
    }
}

bool FrameGraphResourceContainer::hasBuffer(std::string_view name) const {
    return isValidFrameGraphBufferId(getBufferIdByName(name));
}

bool FrameGraphResourceContainer::hasBuffer(
    FrameGraphBufferId id) const {
    return isValidFrameGraphBufferId(id) &&
           buffers.contains(id);
}

FrameGraphBufferId
FrameGraphResourceContainer::getBufferIdByName(
    std::string_view name) const {
    const auto found = name_to_id.find(std::string{name});
    return found != name_to_id.end()
               ? found->second
               : noFrameGraphBufferId();
}

const BufferWrapper &FrameGraphResourceContainer::buffer(std::string_view name) const {
    const auto id = getBufferIdByName(name);
    if (!isValidFrameGraphBufferId(id)) {
        throw std::runtime_error("Frame graph buffer not found: " + std::string{name});
    }
    return buffer(id);
}

const BufferWrapper &FrameGraphResourceContainer::buffer(
    FrameGraphBufferId id) const {
    return buffers.get(id).buffer;
}

vk::DeviceSize FrameGraphResourceContainer::bufferSize(std::string_view name) const {
    const auto id = getBufferIdByName(name);
    if (!isValidFrameGraphBufferId(id)) {
        throw std::runtime_error("Frame graph buffer not found: " + std::string{name});
    }
    return bufferSize(id);
}

vk::DeviceSize FrameGraphResourceContainer::bufferSize(
    FrameGraphBufferId id) const {
    return buffers.get(id).definition.size;
}

const FrameGraphBufferDefinition &
FrameGraphResourceContainer::definition(
    FrameGraphBufferId id) const {
    return buffers.get(id).definition;
}

vk::DescriptorBufferInfo FrameGraphResourceContainer::descriptorInfo(std::string_view name) const {
    const auto id = getBufferIdByName(name);
    if (!isValidFrameGraphBufferId(id)) {
        throw std::runtime_error(
            "Frame graph buffer not found: " + std::string{name});
    }
    return descriptorInfo(id);
}

vk::DescriptorBufferInfo FrameGraphResourceContainer::descriptorInfo(
    FrameGraphBufferId id) const {
    auto &record = buffers.get(id);
    return vk::DescriptorBufferInfo{record.buffer.buffer.get(), 0, record.definition.size};
}

bool FrameGraphResourceContainer::hasHostBufferSource(
    FrameGraphHostBufferSource source) const {
    return std::any_of(
        registration_order.begin(),
        registration_order.end(),
        [&](FrameGraphBufferId id) {
            if (!buffers.contains(id)) return false;
            const auto &candidate =
                buffers.get(id).definition.host_source;
            return candidate && *candidate == source;
        });
}

std::vector<
    FrameGraphResourceContainer::HostBufferTarget>
FrameGraphResourceContainer::hostBufferTargets(
    FrameGraphHostBufferSource source) const {
    std::vector<HostBufferTarget> result;
    for (const auto id : registration_order) {
        if (!buffers.contains(id)) continue;
        const auto &definition =
            buffers.get(id).definition;
        if (!definition.host_source ||
            *definition.host_source != source) {
            continue;
        }
        result.push_back(HostBufferTarget{
            .id = id,
            .name = definition.name,
            .size = definition.size,
            .source = source,
        });
    }
    return result;
}

void FrameGraphResourceContainer::writeHostBuffer(
    FrameGraphBufferId id,
    std::span<const std::byte> bytes,
    std::optional<HostBufferPopulation>
        population) {
    if (!buffers.contains(id)) {
        throw std::runtime_error(
            "Frame graph host buffer handle is unavailable");
    }
    auto &record = buffers.get(id);
    if (!record.definition.host_source) {
        throw std::runtime_error(
            "Frame graph buffer '" +
            record.definition.name +
            "' is not host sourced");
    }
    if (bytes.empty() ||
        bytes.size() > record.definition.size) {
        throw std::runtime_error(
            "Frame graph host write for '" +
            record.definition.name +
            "' requires 1.." +
            std::to_string(record.definition.size) +
            " bytes, received " +
            std::to_string(bytes.size()));
    }
    GET_MODULE(VulkanManageCore)
        .writeBuf(
            record.buffer, bytes.data(), 0,
            static_cast<vk::DeviceSize>(
                bytes.size()));
    if (population &&
        record.host_population != population) {
        const auto dropped =
            population->source_records -
            std::min(
                population->source_records,
                population->written_records);
        if (logger != nullptr && dropped != 0) {
            LOG_WARNING(
                logger,
                "Host buffer '{}' source '{}' truncated {} of {} records "
                "(written {})",
                record.definition.name,
                frameGraphHostBufferSourceName(
                    *record.definition.host_source),
                dropped,
                population->source_records,
                population->written_records);
        }
        record.host_population = *population;
    }
}

std::optional<
    FrameGraphResourceContainer::HostBufferPopulation>
FrameGraphResourceContainer::hostBufferPopulation(
    FrameGraphBufferId id) const {
    if (!buffers.contains(id)) {
        return std::nullopt;
    }
    return buffers.get(id).host_population;
}

ComputeTaskContainer::ComputeTaskContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      descriptor_pool{createDescriptorPool(device)} {}

ComputeTaskContainer::~ComputeTaskContainer() = default;

ComputeTaskContainer::DescriptorSetRecord ComputeTaskContainer::createDescriptorSet(
    vk::DescriptorPool pool, PipelineHandle pipeline,
    const std::vector<ResolvedComputeResourceBinding> &resources,
    const std::vector<ShaderResourceInterfaceBinding>
        &resource_interface,
    RenderTargetContainer &render_target_container,
    const FrameGraphResourceContainer &frame_graph_resources,
    std::uint32_t frame_index,
    std::uint32_t view_index) {
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto bindings = passInputBindings(pipeline_factory.reflection(pipeline));
    if (bindings.empty()) {
        return {};
    }

    const auto layout = pipeline_factory.descriptorSetLayout(pipeline, PELICAN_SET_PASS_INPUT);
    vk::DescriptorSetAllocateInfo alloc_info;
    alloc_info.descriptorPool = pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;
    auto descriptor_sets = device.allocateDescriptorSetsUnique(alloc_info);
    auto descriptor_set = std::move(descriptor_sets.front());

    std::vector<vk::DescriptorBufferInfo> buffer_infos;
    std::vector<vk::DescriptorImageInfo> image_infos;
    std::vector<vk::WriteDescriptorSet> writes;
    buffer_infos.reserve(bindings.size());
    image_infos.reserve(bindings.size());
    writes.reserve(bindings.size());

    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        const auto *typed =
            interfaceForBinding(
                resource_interface, binding.binding);
        const auto &resource =
            typed != nullptr
                ? resourceForInterface(
                      *typed, resources)
                : resourceForBinding(
                      binding, i, resources);

        vk::WriteDescriptorSet write;
        write.dstSet = descriptor_set.get();
        write.dstBinding = binding.binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = binding.type;

        if (isValidFrameGraphBufferId(resource.buffer)) {
            if (resource.history_read) {
                throw std::runtime_error(
                    "Compute task history resource must be a render target: " +
                    resource.name);
            }
            if (binding.type != vk::DescriptorType::eStorageBuffer) {
                throw std::runtime_error("Compute task buffer binding must be a storage buffer");
            }
            buffer_infos.push_back(
                frame_graph_resources.descriptorInfo(
                    resource.buffer));
            write.pBufferInfo = &buffer_infos.back();
        } else {
            const auto rt_id = resource.render_target;
            if (!isConcreteRenderTarget(rt_id)) {
                throw std::runtime_error("Compute task resource not found: " +
                                         resource.name);
            }
            const auto sampled =
                typed != nullptr &&
                typed->descriptor ==
                    ShaderResourceDescriptorKind::
                        combined_image_sampler;
            const auto expected_type =
                sampled
                    ? vk::DescriptorType::
                          eCombinedImageSampler
                    : vk::DescriptorType::eStorageImage;
            if (binding.type != expected_type) {
                throw std::runtime_error(
                    "Compute task render target binding has an "
                    "unexpected descriptor type: " +
                    resource.authored_name);
            }
            const auto layered =
                typed != nullptr &&
                typed->image_view_dimension ==
                    ReflectedImageViewDimension::
                        two_d_array;
            const auto cube =
                typed != nullptr &&
                typed->image_view_dimension ==
                    ReflectedImageViewDimension::cube;
            const auto metadata =
                render_target_container
                    .getMetadata(rt_id);
            const auto sequential_layer =
                !cube && !layered &&
                resource.physical_view ==
                    VulkanResourceViewLayout::
                        sequential_2d &&
                resource.physical_view_count > 1 &&
                metadata.array_layers >=
                    resource.physical_view_count;
            if (sequential_layer &&
                view_index >=
                    resource.physical_view_count) {
                throw std::runtime_error(
                    "Compute task descriptor view index is outside the logical view family: " +
                    resource.authored_name);
            }
            const auto shares_storage_layout =
                sampled &&
                std::any_of(
                    resource_interface.begin(),
                    resource_interface.end(),
                    [&](const auto &candidate) {
                        if (candidate.descriptor !=
                            ShaderResourceDescriptorKind::
                                storage_image) {
                            return false;
                        }
                        const auto &other =
                            resourceForInterface(
                                candidate, resources);
                        return other.render_target ==
                                   rt_id &&
                               other.history_read ==
                                   resource.history_read;
                    });
            vk::ImageView image_view;
            if (typed != nullptr &&
                typed->port.subresource) {
                auto subresource =
                    *typed->port.subresource;
                if (sequential_layer) {
                    subresource
                        .base_array_layer +=
                        view_index;
                }
                image_view =
                    cube
                        ? render_target_container
                              .getImageSubresourceViewForFrame(
                                  rt_id,
                                  subresource,
                                  ImageSubresourceViewDimension::
                                      cube,
                                  resource.history_read,
                                  frame_index)
                        : render_target_container
                              .getImageSubresourceViewForFrame(
                                  rt_id,
                                  subresource,
                                  layered,
                                  resource.history_read,
                                  frame_index);
            } else if (cube) {
                image_view =
                    render_target_container
                        .getImageSubresourceViewForFrame(
                            rt_id,
                            ImageSubresourceRange{
                                .base_mip_level = 0,
                                .level_count =
                                    metadata.mip_levels,
                                .base_array_layer = 0,
                                .layer_count = 6,
                            },
                            ImageSubresourceViewDimension::
                                cube,
                            resource.history_read,
                            frame_index);
            } else if (layered) {
                image_view =
                    render_target_container
                        .getImageSubresourceViewForFrame(
                            rt_id,
                            ImageSubresourceRange{
                                .layer_count =
                                    resource
                                        .physical_view_count,
                            },
                            true,
                            resource.history_read,
                            frame_index);
            } else if (sequential_layer) {
                image_view =
                    render_target_container
                        .getImageLayerViewForFrame(
                            rt_id, view_index,
                            resource.history_read,
                            frame_index);
            } else {
                image_view =
                    render_target_container
                        .getImageViewForFrame(
                            rt_id,
                            resource.history_read,
                            frame_index);
            }
            image_infos.push_back(vk::DescriptorImageInfo{
                sampled
                    ? samplerFor(
                          typed->port.sampling)
                    : vk::Sampler{},
                image_view,
                sampled && !shares_storage_layout
                    ? vk::ImageLayout::
                          eShaderReadOnlyOptimal
                    : vk::ImageLayout::eGeneral,
            });
            write.pImageInfo = &image_infos.back();
        }
        writes.push_back(write);
    }

    device.updateDescriptorSets(writes, {});
    DescriptorSetRecord result;
    result.descriptor_set = std::move(descriptor_set);
    result.bound_image_views.reserve(image_infos.size());
    for (const auto &image_info : image_infos) {
        result.bound_image_views.push_back(image_info.imageView);
    }
    return result;
}

vk::Sampler ComputeTaskContainer::samplerFor(
    ShaderResourcePortSampling sampling) {
    auto &sampler =
        sampled_image_samplers.at(
            samplerIndex(sampling));
    if (!sampler) {
        sampler =
            createSampler(device, sampling);
    }
    return sampler.get();
}

FrameGraphResourceContainer::RegistrationCheckpoint
FrameGraphResourceContainer::checkpointRegistrations() const {
    return RegistrationCheckpoint{
        registration_order.size(), name_to_id};
}

void FrameGraphResourceContainer::rollbackRegistrations(
    RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Frame graph buffer registration checkpoint is invalid");
    }
    while (registration_order.size() >
           checkpoint.registration_count) {
        (void)buffers.extract(
            registration_order.back(), false);
        registration_order.pop_back();
    }
    name_to_id = std::move(checkpoint.name_to_id);
}

std::vector<std::tuple<std::string, FrameGraphBufferId,
                       vk::DeviceSize>>
FrameGraphResourceContainer::registrationsSince(
    RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Frame graph buffer registration checkpoint is invalid");
    }
    std::vector<std::tuple<std::string, FrameGraphBufferId,
                           vk::DeviceSize>>
        result;
    result.reserve(registration_order.size() -
                   checkpoint.registration_count);
    for (std::size_t index = checkpoint.registration_count;
         index < registration_order.size(); ++index) {
        const auto id = registration_order[index];
        const auto &record = buffers.get(id);
        result.emplace_back(
            record.definition.name, id,
            record.definition.size);
    }
    return result;
}

void FrameGraphResourceContainer::hideRegistrationName(
    const std::string &name, FrameGraphBufferId expected) {
    const auto found = name_to_id.find(name);
    if (found != name_to_id.end() &&
        found->second == expected) {
        name_to_id.erase(found);
    }
}

void FrameGraphResourceContainer::retireRegistrations(
    const std::vector<FrameGraphBufferId> &ids) noexcept {
    for (const auto id : ids) {
        if (!buffers.contains(id)) continue;
        const auto name = buffers.get(id).definition.name;
        hideRegistrationName(name, id);
        auto retired = buffers.extract(id, false);
        std::erase(registration_order, id);
        if (retired) deferOrDestroy(std::move(*retired));
    }
}

ComputeTaskId ComputeTaskContainer::registerComputeTask(
    const ComputeTaskDefinition &definition,
    const ComputeTaskRuntimeDependencies &dependencies) {
    if (auto found = name_to_id.find(definition.name); found != name_to_id.end()) {
        return found->second;
    }

    auto resolved_resources = resolveTaskResources(
        definition, dependencies.render_target_container,
        dependencies.frame_graph_resources);
    std::optional<IndirectDispatchRecord>
        indirect_dispatch;
    if (definition.dispatch.indirect) {
        if (definition.ray_tracing) {
            throw std::runtime_error(
                "pelican.ray_tracing.indirect_trace_unsupported@1: ray "
                "tracing task '" +
                definition.name +
                "' cannot declare compute indirect dispatch");
        }
        const auto &authored =
            *definition.dispatch.indirect;
        if (std::find(
                definition.writes.begin(),
                definition.writes.end(),
                authored.buffer) !=
            definition.writes.end()) {
            throw std::runtime_error(
                "Compute task '" + definition.name +
                "' cannot write its own indirect dispatch "
                "buffer; use a separate producer task");
        }
        const auto buffer =
            dependencies.frame_graph_resources
                .getBufferIdByName(authored.buffer);
        if (!isValidFrameGraphBufferId(buffer)) {
            throw std::runtime_error(
                "Compute task '" + definition.name +
                "' indirect dispatch buffer not found: " +
                authored.buffer);
        }
        const auto &buffer_definition =
            dependencies.frame_graph_resources
                .definition(buffer);
        if (buffer_definition.command_layout !=
            FrameGraphBufferCommandLayout::
                compute_dispatch) {
            throw std::runtime_error(
                "Compute task '" + definition.name +
                "' indirect dispatch buffer '" +
                authored.buffer +
                "' requires command_layout "
                "'compute_dispatch'");
        }
        if (authored.offset %
                frameGraphIndirectCommandAlignment !=
                0 ||
            authored.offset >
                buffer_definition.size ||
            buffer_definition.size -
                    authored.offset <
                compute_dispatch_command_size) {
            throw std::runtime_error(
                "Compute task '" + definition.name +
                "' indirect dispatch command is outside buffer '" +
                authored.buffer + "'");
        }
        indirect_dispatch =
            IndirectDispatchRecord{
                .buffer = buffer,
                .offset = authored.offset,
            };
    }
    auto resource_interface =
        makeComputeResourceInterface(
            definition, resolved_resources,
            dependencies);

    auto &shader_library = dependencies.shader_library;
    std::vector<std::pair<std::string, std::string>>
        virtual_includes;
    if (!definition.ray_tracing &&
        !resource_interface.empty()) {
        virtual_includes =
            makeShaderResourcePortVirtualIncludes(
                resource_interface);
    }
    const auto shader_defines =
        !definition.ray_tracing &&
                dependencies.shader_defines != nullptr
            ? *dependencies.shader_defines
            : std::vector<std::string>{};
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    PipelineHandle pipeline;
    const auto load_shader =
        [&](const ShaderReference &reference) {
            return shader_library.loadFromReference(
                reference, dependencies.path_resolver, true,
                shader_defines, virtual_includes);
        };
    if (definition.ray_tracing) {
        std::vector<ShaderBundleId> misses;
        misses.reserve(
            definition.ray_tracing->misses.size());
        for (const auto &reference :
             definition.ray_tracing->misses) {
            misses.push_back(load_shader(reference));
        }
        std::vector<ShaderBundleId> closest_hits;
        closest_hits.reserve(
            definition.ray_tracing->closest_hits.size());
        for (const auto &reference :
             definition.ray_tracing->closest_hits) {
            closest_hits.push_back(load_shader(reference));
        }
        pipeline = pipeline_factory.createRayTracing(
            RayTracingPipelineDesc{
                .raygen = load_shader(
                    definition.ray_tracing->raygen),
                .misses = std::move(misses),
                .closest_hits = std::move(closest_hits),
                .shader_defines = shader_defines,
                .resource_interface = resource_interface,
            });
    } else {
        pipeline = pipeline_factory.createCompute(
            ComputePipelineDesc{
                .shader = load_shader(definition.shader),
                .shader_defines = shader_defines,
                .resource_interface = resource_interface,
            });
    }
    std::array<std::uint32_t, 3>
        dispatch_groups{
            definition.dispatch.groups_x,
            definition.dispatch.groups_y,
            definition.dispatch.groups_z,
        };
    std::optional<ImageExtentDispatchRecord>
        image_extent_dispatch;
    if (definition.dispatch.groups_from ||
        definition.dispatch.rays_from) {
        const auto resolved_dispatch =
            resolveImageExtentDispatch(
                definition, resolved_resources,
                dependencies
                    .render_target_container);
        dispatch_groups = definition.ray_tracing
                              ? imageExtentTraceDimensions(
                                    definition,
                                    resolved_dispatch,
                                    dependencies
                                        .render_target_container)
                              : imageExtentDispatchGroups(
                                    definition, resolved_dispatch,
                                    dependencies
                                        .render_target_container,
                                    pipeline_factory
                                        .reflection(pipeline)
                                        .local_size);
        image_extent_dispatch =
            ImageExtentDispatchRecord{
                .render_target =
                    resolved_dispatch
                        .render_target,
                .mip_level =
                    resolved_dispatch.mip_level,
            };
    }
    std::uint32_t descriptor_variant_count = 1;
    if (definition.schedule ==
        ComputeTaskSchedule::per_view) {
        for (const auto &resource :
             resolved_resources) {
            if (!isConcreteRenderTarget(
                    resource.render_target) ||
                resource.physical_view !=
                    VulkanResourceViewLayout::
                        sequential_2d ||
                resource.physical_view_count <= 1) {
                continue;
            }
            const auto metadata =
                dependencies
                    .render_target_container
                    .getMetadata(
                        resource.render_target);
            if (metadata.array_layers >=
                resource.physical_view_count) {
                descriptor_variant_count =
                    std::max(
                        descriptor_variant_count,
                        resource
                            .physical_view_count);
            }
        }
    }
    std::vector<
        std::array<vk::UniqueDescriptorSet, 2>>
        descriptor_sets(
            descriptor_variant_count);
    std::vector<
        std::array<
            std::vector<vk::ImageView>, 2>>
        bound_image_views(
            descriptor_variant_count);
    for (std::uint32_t view_index = 0;
         view_index < descriptor_variant_count;
         ++view_index) {
        for (std::uint32_t frame_index = 0;
             frame_index < 2; ++frame_index) {
            auto binding = createDescriptorSet(
                descriptor_pool.get(), pipeline,
                resolved_resources,
                resource_interface,
                dependencies
                    .render_target_container,
                dependencies
                    .frame_graph_resources,
                frame_index, view_index);
            descriptor_sets[view_index]
                           [frame_index] =
                std::move(
                    binding.descriptor_set);
            bound_image_views[view_index]
                             [frame_index] =
                std::move(
                    binding.bound_image_views);
        }
    }

    registration_order.reserve(registration_order.size() + 1);
    if (next_task_id == std::numeric_limits<int>::max()) {
        throw std::runtime_error(
            "Compute task handle table is exhausted");
    }
    const auto id = ComputeTaskId{next_task_id++};
    const auto [task_it, task_inserted] = tasks.emplace(
        id.value,
        TaskRecord{
            .definition = definition,
            .resource_bindings =
                std::move(resolved_resources),
            .resource_interface =
                std::move(resource_interface),
            .pipeline = pipeline,
            .descriptor_sets =
                std::move(descriptor_sets),
            .bound_image_views =
                std::move(bound_image_views),
            .binding_revision =
                next_binding_revision++,
            .ray_tracing =
                definition.ray_tracing.has_value(),
            .dispatch_x =
                dispatch_groups[0],
            .dispatch_y =
                dispatch_groups[1],
            .dispatch_z =
                dispatch_groups[2],
            .indirect_dispatch =
                indirect_dispatch,
            .image_extent_dispatch =
                image_extent_dispatch,
        });
    if (!task_inserted) {
        throw std::runtime_error(
            "Compute task handle table changed during registration");
    }
    try {
        if (!name_to_id.emplace(definition.name, id).second) {
            throw std::runtime_error(
                "Compute task name table changed during registration: " +
                definition.name);
        }
    } catch (...) {
        tasks.erase(task_it);
        throw;
    }
    registration_order.push_back(id);
    return id;
}

void ComputeTaskContainer::rebindRenderTargets(
    RenderTargetContainer &render_target_container) {
    auto next_pool = createDescriptorPool(device);
    struct ReboundTask {
        int id = -1;
        std::vector<
            std::array<vk::UniqueDescriptorSet, 2>>
            descriptor_sets;
        std::vector<
            std::array<
                std::vector<vk::ImageView>, 2>>
            bound_image_views;
        std::optional<
            std::array<std::uint32_t, 3>>
            dispatch_groups;
    };
    std::vector<ReboundTask> rebound;
    rebound.reserve(tasks.size());
    const auto &frame_graph_resources =
        GET_MODULE(FrameGraphResourceContainer);
    for (const auto &[id, task] : tasks) {
        ReboundTask next;
        next.id = id;
        if (task.image_extent_dispatch) {
            const auto &dispatch =
                *task.image_extent_dispatch;
            const ResolvedImageExtentDispatch resolved{
                .render_target = dispatch.render_target,
                .mip_level = dispatch.mip_level,
            };
            next.dispatch_groups =
                task.ray_tracing
                    ? imageExtentTraceDimensions(
                          task.definition, resolved,
                          render_target_container)
                    : imageExtentDispatchGroups(
                          task.definition, resolved,
                          render_target_container,
                          GET_MODULE(PipelineFactory)
                              .reflection(task.pipeline)
                              .local_size);
        }
        next.descriptor_sets.resize(
            task.descriptor_sets.size());
        next.bound_image_views.resize(
            task.descriptor_sets.size());
        for (std::uint32_t view_index = 0;
             view_index <
             task.descriptor_sets.size();
             ++view_index) {
            for (std::uint32_t frame_index = 0;
                 frame_index < 2;
                 ++frame_index) {
                auto binding =
                    createDescriptorSet(
                        next_pool.get(),
                        task.pipeline,
                        task.resource_bindings,
                        task.resource_interface,
                        render_target_container,
                        frame_graph_resources,
                        frame_index,
                        view_index);
                next.descriptor_sets
                        [view_index]
                        [frame_index] =
                    std::move(
                        binding.descriptor_set);
                next.bound_image_views
                        [view_index]
                        [frame_index] =
                    std::move(
                        binding
                            .bound_image_views);
            }
        }
        rebound.push_back(std::move(next));
    }

    auto retired_pool = std::move(descriptor_pool);
    std::vector<
        std::vector<
            std::array<
                vk::UniqueDescriptorSet, 2>>>
        retired_descriptor_sets;
    retired_descriptor_sets.reserve(tasks.size());
    for (auto &[id, task] : tasks) {
        (void)id;
        retired_descriptor_sets.push_back(
            std::move(task.descriptor_sets));
    }
    descriptor_pool = std::move(next_pool);
    for (auto &next : rebound) {
        auto &task = tasks.at(next.id);
        task.descriptor_sets = std::move(next.descriptor_sets);
        task.bound_image_views = std::move(next.bound_image_views);
        if (next.dispatch_groups) {
            task.dispatch_x =
                next.dispatch_groups->at(0);
            task.dispatch_y =
                next.dispatch_groups->at(1);
            task.dispatch_z =
                next.dispatch_groups->at(2);
        }
        task.binding_revision = next_binding_revision++;
    }
    deferRetiredDescriptorResources(
        GET_MODULE(DeletionQueue),
        std::move(retired_pool),
        std::move(retired_descriptor_sets));
}

const ComputeTaskDefinition &ComputeTaskContainer::definition(ComputeTaskId task_id) const {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end()) {
        throw std::runtime_error("Compute task not found");
    }
    return found->second.definition;
}

ComputeTaskId ComputeTaskContainer::getComputeTaskIdByName(const std::string &name) const {
    if (auto found = name_to_id.find(name); found != name_to_id.end()) {
        return found->second;
    }
    return ComputeTaskId{-1};
}

void ComputeTaskContainer::setDispatchGroups(ComputeTaskId task_id, uint32_t x, uint32_t y, uint32_t z) {
    if (x == 0 || y == 0 || z == 0) {
        throw std::runtime_error("compute dispatch group counts must be positive");
    }
    auto found = tasks.find(task_id.value);
    if (found == tasks.end()) {
        throw std::runtime_error("Compute task not found");
    }
    if (found->second.ray_tracing) {
        throw std::runtime_error(
            "pelican.ray_tracing.fixed_trace_extent_unsupported@1: trace "
            "dimensions are derived from dispatch.rays_from");
    }
    if (found->second.indirect_dispatch) {
        throw std::runtime_error(
            "cannot set direct dispatch groups on an indirect "
            "compute task");
    }
    if (found->second.image_extent_dispatch) {
        throw std::runtime_error(
            "cannot override image extent-derived compute dispatch "
            "groups");
    }
    found->second.dispatch_x = x;
    found->second.dispatch_y = y;
    found->second.dispatch_z = z;
}

void ComputeTaskContainer::setDispatchGroups(const std::string &task_name, uint32_t x, uint32_t y, uint32_t z) {
    const auto task_id = getComputeTaskIdByName(task_name);
    if (task_id.value < 0) {
        throw std::runtime_error("Compute task not found: " + task_name);
    }
    setDispatchGroups(task_id, x, y, z);
}

void ComputeTaskContainer::transitionResourcesForDispatch(vk::CommandBuffer cmd_buf,
                                                          ComputeTaskId task_id,
                                                          RenderTargetContainer &render_target_container,
                                                          VulkanUtils &vk_utils,
                                                          RenderTargetLayoutTracker &layout_tracker) const {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end()) {
        throw std::runtime_error("Compute task not found");
    }
    const auto shader_stage =
        found->second.ray_tracing
            ? vk::PipelineStageFlags{
                  vk::PipelineStageFlagBits::eRayTracingShaderKHR}
            : vk::PipelineStageFlags{
                  vk::PipelineStageFlagBits::eComputeShader};
    for (const auto &resource :
         found->second.resource_bindings) {
        const auto rt_id = resource.render_target;
        if (isConcreteRenderTarget(rt_id)) {
            const auto sampled = std::any_of(
                found->second.resource_interface.begin(),
                found->second.resource_interface.end(),
                [&](const ShaderResourceInterfaceBinding
                        &candidate) {
                    return candidate.port.resource ==
                               resource.authored_name &&
                           candidate.descriptor ==
                               ShaderResourceDescriptorKind::
                                   combined_image_sampler;
                });
            const auto storage = std::any_of(
                found->second.resource_interface.begin(),
                found->second.resource_interface.end(),
                [&](const ShaderResourceInterfaceBinding
                        &candidate) {
                    return candidate.port.resource ==
                               resource.authored_name &&
                           candidate.descriptor ==
                               ShaderResourceDescriptorKind::
                                   storage_image;
                });
            const auto desired_layout =
                sampled && !storage
                    ? vk::ImageLayout::
                          eShaderReadOnlyOptimal
                    : vk::ImageLayout::eGeneral;
            if (layout_tracker.currentLayout(rt_id, resource.history_read,
                                             &render_target_container) ==
                desired_layout) {
                layout_tracker.memoryDependency(cmd_buf, render_target_container,
                                                 vk_utils, rt_id,
                                                 resource.history_read,
                                                 shader_stage);
            } else {
                layout_tracker.transition(cmd_buf, render_target_container, vk_utils,
                                           rt_id, desired_layout,
                                           resource.history_read,
                                           RenderTargetImageKind::resolved,
                                           shader_stage);
            }
        }
    }
}

void ComputeTaskContainer::dispatch(
    vk::CommandBuffer cmd_buf, ComputeTaskId task_id,
    const FrameResources &frame_resources,
    std::uint32_t view_index) const {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end()) {
        throw std::runtime_error("Compute task not found");
    }
    const auto &record = found->second;
    if (record.descriptor_sets.empty()) {
        throw std::runtime_error(
            "Compute task has no descriptor variants");
    }
    const auto descriptor_view =
        record.descriptor_sets.size() == 1
            ? 0u
            : view_index;
    if (descriptor_view >=
        record.descriptor_sets.size()) {
        throw std::runtime_error(
            "Compute task descriptor view index is out of range");
    }
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto bind_point =
        record.ray_tracing
            ? vk::PipelineBindPoint::eRayTracingKHR
            : vk::PipelineBindPoint::eCompute;
    cmd_buf.bindPipeline(
        bind_point, pipeline_factory.pipeline(record.pipeline));
    if (record.ray_tracing) {
        frame_resources.bindRayTracing(
            cmd_buf,
            pipeline_factory.layout(record.pipeline));
    } else {
        frame_resources.bindCompute(
            cmd_buf,
            pipeline_factory.layout(record.pipeline));
    }
    const auto parity = GET_MODULE(RenderTargetContainer).historyFrameIndex();
    if (record.descriptor_sets
            [descriptor_view][parity]) {
        cmd_buf.bindDescriptorSets(bind_point, pipeline_factory.layout(record.pipeline),
                                   PELICAN_SET_PASS_INPUT,
                                   record.descriptor_sets
                                       [descriptor_view]
                                       [parity]
                                           .get(),
                                   {});
    }
    if (record.ray_tracing) {
        pipeline_factory.traceRays(
            cmd_buf, record.pipeline,
            record.dispatch_x, record.dispatch_y,
            record.dispatch_z);
    } else if (record.indirect_dispatch) {
        const auto &indirect =
            *record.indirect_dispatch;
        const auto &resources =
            GET_MODULE(FrameGraphResourceContainer);
        cmd_buf.dispatchIndirect(
            resources.buffer(indirect.buffer)
                .buffer.get(),
            indirect.offset);
    } else {
        cmd_buf.dispatch(
            record.dispatch_x, record.dispatch_y,
            record.dispatch_z);
    }
}

void ComputeTaskContainer::bufferReadAfterWriteBarrier(vk::CommandBuffer cmd_buf,
                                                       FrameGraphBufferId resource,
                                                       FramePlanNodeKind from_kind,
                                                       FramePlanNodeKind to_kind) const {
    const auto &resource_container = GET_MODULE(FrameGraphResourceContainer);
    if (!resource_container.hasBuffer(resource)) {
        return;
    }

    vk::BufferMemoryBarrier barrier;
    barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    auto destination_stage = shaderStage(to_kind);
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    const auto &definition =
        resource_container.definition(resource);
    const auto indirect_compute =
        to_kind == FramePlanNodeKind::compute &&
        definition.command_layout ==
            FrameGraphBufferCommandLayout::
                compute_dispatch;
    const auto indirect_draw =
        to_kind == FramePlanNodeKind::render &&
        (definition.command_layout ==
             FrameGraphBufferCommandLayout::
                 indexed_draw ||
         definition.command_layout ==
             FrameGraphBufferCommandLayout::
                 draw_count);
    if (indirect_compute || indirect_draw) {
        destination_stage |=
            vk::PipelineStageFlagBits::eDrawIndirect;
        barrier.dstAccessMask |=
            vk::AccessFlagBits::
                eIndirectCommandRead;
    }
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = resource_container.buffer(resource).buffer.get();
    barrier.offset = 0;
    barrier.size = resource_container.bufferSize(resource);
    cmd_buf.pipelineBarrier(
        shaderStage(from_kind), destination_stage,
        {}, {}, {barrier}, {});
}

std::vector<vk::ImageView> ComputeTaskContainer::boundImageViewsForTesting(
    ComputeTaskId task_id, std::uint32_t frame_index,
    std::uint32_t view_index) const {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end() ||
        frame_index >= 2 ||
        view_index >=
            found->second
                .bound_image_views.size()) {
        return {};
    }
    return found->second
        .bound_image_views[view_index]
                          [frame_index];
}

std::array<std::uint32_t, 3>
ComputeTaskContainer::dispatchGroupsForTesting(
    ComputeTaskId task_id) const {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end()) {
        return {};
    }
    return {
        found->second.dispatch_x,
        found->second.dispatch_y,
        found->second.dispatch_z,
    };
}

std::uint64_t ComputeTaskContainer::bindingRevisionForTesting(
    ComputeTaskId task_id) const {
    const auto found = tasks.find(task_id.value);
    return found == tasks.end() ? 0 : found->second.binding_revision;
}

ComputeTaskContainer::RegistrationCheckpoint
ComputeTaskContainer::checkpointRegistrations() const {
    return RegistrationCheckpoint{
        registration_order.size(), next_binding_revision,
        next_task_id, name_to_id};
}

void ComputeTaskContainer::rollbackRegistrations(
    RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Compute task registration checkpoint is invalid");
    }
    while (registration_order.size() >
           checkpoint.registration_count) {
        const auto id = registration_order.back();
        const auto found = tasks.find(id.value);
        if (found == tasks.end()) {
            throw std::runtime_error(
                "Compute task registration log is inconsistent");
        }
        tasks.erase(found);
        registration_order.pop_back();
    }
    next_binding_revision =
        checkpoint.next_binding_revision;
    next_task_id = checkpoint.next_task_id;
    name_to_id = std::move(checkpoint.name_to_id);
}

std::vector<std::pair<std::string, ComputeTaskId>>
ComputeTaskContainer::registrationsSince(
    RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Compute task registration checkpoint is invalid");
    }
    std::vector<std::pair<std::string, ComputeTaskId>>
        result;
    result.reserve(registration_order.size() -
                   checkpoint.registration_count);
    for (std::size_t index = checkpoint.registration_count;
         index < registration_order.size(); ++index) {
        const auto id = registration_order[index];
        result.emplace_back(tasks.at(id.value).definition.name,
                            id);
    }
    return result;
}

void ComputeTaskContainer::hideRegistrationName(
    const std::string &name, ComputeTaskId expected) {
    const auto found = name_to_id.find(name);
    if (found != name_to_id.end() &&
        found->second == expected) {
        name_to_id.erase(found);
    }
}

void ComputeTaskContainer::retireRegistrations(
    const std::vector<ComputeTaskId> &ids) noexcept {
    for (const auto id : ids) {
        const auto found = tasks.find(id.value);
        if (found == tasks.end()) continue;
        hideRegistrationName(
            found->second.definition.name, id);
        auto retired = std::move(found->second);
        tasks.erase(found);
        std::erase(registration_order, id);
        deferOrDestroy(std::move(retired));
    }
}

} // namespace Pelican
