#include "computetaskcontainer.hpp"
#include "deferredresourceretire.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../shader/raytracingvalidation.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../renderer/frameresources.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/render_target_layout_tracker.hpp"
#include "../vkcore/util.hpp"
#include "../../project/renderresourcename.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include "computetaskdefinitionjson.hpp"

namespace Pelican {

namespace {

thread_local bool ray_group_layout_virtual_include_hookup_enabled =
    true;

constexpr uint32_t max_compute_descriptor_sets = 256;
constexpr uint32_t max_compute_descriptors = 1024;
constexpr vk::DeviceSize compute_dispatch_command_size =
    sizeof(vk::DispatchIndirectCommand);

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
                GET_MODULE(RenderTargetContainer)
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


// The shader recipe a compute task compiles with: feature defines plus the
// includes generated for this task. A ray_group_layout task carries the
// generated ray-layout include (which is also the pipeline cache salt);
// a plain compute task carries the resource-port include and the defines.
struct ComputeTaskShaderRecipe {
    std::vector<std::string> defines;
    std::vector<std::pair<std::string, std::string>>
        virtual_includes;
};

ComputeTaskShaderRecipe makeComputeTaskShaderRecipe(
    const ComputeTaskDefinition &definition,
    const std::vector<ShaderResourceInterfaceBinding>
        &resource_interface,
    const ComputeTaskRuntimeDependencies &dependencies) {
    ComputeTaskShaderRecipe recipe;
    if (!definition.ray_tracing &&
        !resource_interface.empty()) {
        recipe.virtual_includes =
            makeShaderResourcePortVirtualIncludes(
                resource_interface);
    }
    if (ray_group_layout_virtual_include_hookup_enabled &&
        definition.ray_tracing &&
        definition.ray_tracing->uses_group_layout) {
        if (!dependencies.ray_tracing_project) {
            throw std::logic_error(
                "layout ray tracing task lost its typed project sidecar");
        }
        recipe.virtual_includes.emplace_back(
            std::string{rayGroupLayoutVirtualIncludeName},
            makeRayGroupLayoutVirtualInclude(
                dependencies.ray_tracing_project->layout));
    }
    recipe.defines =
        !definition.ray_tracing &&
                dependencies.shader_defines != nullptr
            ? *dependencies.shader_defines
            : std::vector<std::string>{};
    return recipe;
}

ShaderBundleId loadComputeTaskShader(
    const ShaderReference &reference,
    const ComputeTaskDefinition &definition,
    const ComputeTaskRuntimeDependencies &dependencies,
    const ComputeTaskShaderRecipe &recipe) {
    auto &shader_library = dependencies.shader_library;
    if (reference.kind ==
        ShaderReferenceKind::engine_generated) {
        if (!definition.ray_tracing ||
            !definition.ray_tracing->uses_group_layout ||
            !dependencies.ray_tracing_project ||
            reference.stage != ShaderStage::anyhit ||
            reference.ref !=
                "generated://material_alpha_v1" ||
            !dependencies.ray_tracing_project->layout
                 .usesMaterialAlphaProfile()) {
            throw std::logic_error(
                "invalid engine-generated ray shader reference");
        }
        return shader_library.loadGeneratedSource(
            makeMaterialAlphaAnyHitSource(
                dependencies.ray_tracing_project->layout),
            ShaderStage::anyhit,
            reference.ref,
            recipe.defines, recipe.virtual_includes);
    }
    if (definition.ray_tracing &&
        definition.ray_tracing->uses_group_layout) {
        auto lower = reference.ref;
        std::transform(
            lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        if (lower.ends_with(".spv")) {
            throw std::runtime_error(
                "pelican.ray_tracing.layout_source_only@1: ray_group_layout shaders must be GLSL source, not SPIR-V: " +
                reference.ref);
        }
    }
    return shader_library.loadFromReference(
        reference, dependencies.path_resolver, true,
        recipe.defines, recipe.virtual_includes);
}

// Every stage a ray tracing pipeline needs, loaded in the order the pipeline
// description used to load them so shader bundle registration order is
// unchanged. Unfilled hit-group slots fall back to the engine's no-op closest
// hit and, for procedural groups, to its non-reporting intersection.
struct ResolvedRayTracingShaders {
    std::vector<ShaderBundleId> misses;
    std::vector<ShaderBundleId> closest_hits;
    std::vector<std::optional<ShaderBundleId>> any_hits;
    std::vector<std::optional<ShaderBundleId>> intersections;
    std::vector<ShaderBundleId> callables;
    ShaderBundleId raygen{};
};

ResolvedRayTracingShaders resolveRayTracingShaders(
    const ComputeTaskDefinition &definition,
    const ComputeTaskRuntimeDependencies &dependencies,
    const ComputeTaskShaderRecipe &recipe) {
    const auto load_shader =
        [&](const ShaderReference &reference) {
            return loadComputeTaskShader(
                reference, definition, dependencies,
                recipe);
        };
    ResolvedRayTracingShaders shaders;
    auto &misses = shaders.misses;
    auto &closest_hits = shaders.closest_hits;
    auto &any_hits = shaders.any_hits;
    auto &intersections = shaders.intersections;
    auto &callables = shaders.callables;
    misses.reserve(
        definition.ray_tracing->misses.size());
    for (const auto &reference :
         definition.ray_tracing->misses) {
        misses.push_back(load_shader(reference));
    }
    if (definition.ray_tracing->uses_group_layout) {
        closest_hits.reserve(
            definition.ray_tracing->hit_shader_slots.size());
        std::optional<ShaderBundleId> no_op;
        for (const auto &slot :
             definition.ray_tracing->hit_shader_slots) {
            if (slot) {
                closest_hits.push_back(load_shader(*slot));
                continue;
            }
            if (!no_op) {
                // Stays an inline generated source rather than an
                // engine:// resource: every embedded .rchit joins
                // engineShaderUniverseSha256() and would re-key every
                // shader cache (shadercompiler.cpp:190-209, :313).
                static constexpr std::string_view source =
                    "#version 460\n"
                    "#extension GL_EXT_ray_tracing : require\n"
                    "hitAttributeEXT vec2 hit_attribute;\n"
                    "void main() {}\n";
                no_op = dependencies.shader_library
                            .loadGeneratedSource(
                                source,
                                ShaderStage::closesthit,
                                "pelican://generated/ray_group_noop.rchit",
                                recipe.defines,
                                recipe.virtual_includes);
            }
            closest_hits.push_back(*no_op);
        }
        any_hits.reserve(
            definition.ray_tracing->any_hit_shader_slots.size());
        for (const auto &slot :
             definition.ray_tracing->any_hit_shader_slots) {
            any_hits.push_back(slot
                                   ? std::optional<ShaderBundleId>{
                                         load_shader(*slot)}
                                   : std::nullopt);
        }
        intersections.reserve(
            definition.ray_tracing->intersection_shader_slots.size());
        std::optional<ShaderBundleId> no_report_intersection;
        const auto &layout = dependencies.ray_tracing_project->layout;
        const auto ray_count = layout.ray_types.size();
        for (std::size_t slot_index = 0;
             slot_index < definition.ray_tracing
                              ->intersection_shader_slots.size();
             ++slot_index) {
            const auto &slot = definition.ray_tracing
                                   ->intersection_shader_slots[slot_index];
            if (slot) {
                intersections.push_back(load_shader(*slot));
                continue;
            }
            const auto group_index = slot_index / ray_count;
            if (layout.group_types.at(group_index) !=
                RayGroupType::procedural) {
                intersections.push_back(std::nullopt);
                continue;
            }
            if (!no_report_intersection) {
                no_report_intersection = load_shader(ShaderReference{
                    "engine://procedural_no_report",
                    ShaderStage::intersection,
                    ShaderReferenceKind::stem, false});
            }
            intersections.push_back(*no_report_intersection);
        }
    } else {
        closest_hits.reserve(
            definition.ray_tracing->closest_hits.size());
        for (const auto &reference :
             definition.ray_tracing->closest_hits) {
            closest_hits.push_back(load_shader(reference));
        }
    }
    for (const auto &reference : definition.ray_tracing->callables) {
        callables.push_back(load_shader(reference));
    }
    shaders.raygen =
        load_shader(definition.ray_tracing->raygen);
    return shaders;
}
} // namespace

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
                                  layered
                                      ? ImageSubresourceViewDimension::
                                            two_d_array
                                      : ImageSubresourceViewDimension::
                                            two_d,
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
                            ImageSubresourceViewDimension::
                                two_d_array,
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

ComputeTaskContainer::TaskDescriptorSets
ComputeTaskContainer::createTaskDescriptorSets(
    vk::DescriptorPool pool, PipelineHandle pipeline,
    const std::vector<ResolvedComputeResourceBinding> &resources,
    const std::vector<ShaderResourceInterfaceBinding>
        &resource_interface,
    RenderTargetContainer &render_target_container,
    const FrameGraphResourceContainer &frame_graph_resources,
    std::uint32_t descriptor_variant_count) {
    TaskDescriptorSets result;
    result.descriptor_sets.resize(
        descriptor_variant_count);
    result.bound_image_views.resize(
        descriptor_variant_count);
    for (std::uint32_t view_index = 0;
         view_index < descriptor_variant_count;
         ++view_index) {
        for (std::uint32_t frame_index = 0;
             frame_index < 2; ++frame_index) {
            auto binding = createDescriptorSet(
                pool, pipeline, resources,
                resource_interface,
                render_target_container,
                frame_graph_resources,
                frame_index, view_index);
            result.descriptor_sets[view_index]
                                  [frame_index] =
                std::move(
                    binding.descriptor_set);
            result.bound_image_views[view_index]
                                    [frame_index] =
                std::move(
                    binding.bound_image_views);
        }
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

ComputeTaskId ComputeTaskContainer::registerComputeTask(
    const ComputeTaskDefinition &definition,
    const ComputeTaskRuntimeDependencies &dependencies) {
    if (auto found = name_to_id.find(definition.name); found != name_to_id.end()) {
        return found->second;
    }

    // Registration reads the same global modules that dispatch() and
    // rebindRenderTargets() read, so a FrameGraphBufferId resolved here can
    // never name a buffer in a different container instance.
    auto &render_target_container =
        GET_MODULE(RenderTargetContainer);
    auto &frame_graph_resources =
        GET_MODULE(FrameGraphResourceContainer);
    auto resolved_resources = resolveTaskResources(
        definition, render_target_container,
        frame_graph_resources);
    std::optional<IndirectDispatchRecord>
        indirect_dispatch;
    if (definition.dispatch.indirect) {
        if (definition.ray_tracing &&
            !GET_MODULE(VulkanManageCore).getRuntimeCapabilities().ray_tracing_indirect)
            throw std::runtime_error("pelican.ray_tracing.indirect_unavailable@1: device does not support indirect ray dispatch");
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
            frame_graph_resources
                .getBufferIdByName(authored.buffer);
        if (!isValidFrameGraphBufferId(buffer)) {
            throw std::runtime_error(
                "Compute task '" + definition.name +
                "' indirect dispatch buffer not found: " +
                authored.buffer);
        }
        const auto &buffer_definition =
            frame_graph_resources
                .definition(buffer);
        const auto expected_layout = definition.ray_tracing
            ? FrameGraphBufferCommandLayout::ray_dispatch
            : FrameGraphBufferCommandLayout::compute_dispatch;
        if (buffer_definition.command_layout != expected_layout) {
            throw std::runtime_error(
                "Compute task '" + definition.name +
                "' indirect dispatch buffer '" +
                authored.buffer +
                "' requires command_layout '" +
                std::string{frameGraphBufferCommandLayoutName(expected_layout)} + "'");
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

    const auto shader_recipe =
        makeComputeTaskShaderRecipe(
            definition, resource_interface,
            dependencies);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    PipelineHandle pipeline;
    if (definition.ray_tracing) {
        auto shaders = resolveRayTracingShaders(
            definition, dependencies, shader_recipe);
        pipeline = pipeline_factory.createRayTracing(
            RayTracingPipelineDesc{
                .max_recursion =
                    definition.ray_tracing->max_recursion,
                .raygen = shaders.raygen,
                .misses = std::move(shaders.misses),
                .closest_hits = std::move(shaders.closest_hits),
                .callables = std::move(shaders.callables),
                .callable_record_layout = definition.ray_tracing->callable_record_layout,
                .any_hits = std::move(shaders.any_hits),
                .intersections = std::move(shaders.intersections),
                .shader_defines = shader_recipe.defines,
                .resource_interface = resource_interface,
                .ray_tracing_project =
                    dependencies.ray_tracing_project,
                .traced_ray_type_indices =
                    definition.ray_tracing
                        ->traced_ray_type_indices,
            });
    } else {
        pipeline = pipeline_factory.createCompute(
            ComputePipelineDesc{
                .shader = loadComputeTaskShader(
                    definition.shader, definition,
                    dependencies, shader_recipe),
                .shader_defines = shader_recipe.defines,
                .resource_interface = resource_interface,
                .allow_ray_query = definition.ray_query_acceleration_structure.has_value(),
            });
    }
    std::array<std::uint32_t, 3>
        dispatch_groups{
            definition.dispatch.groups_x,
            definition.dispatch.groups_y,
            definition.dispatch.groups_z,
        };
    if (definition.dispatch.rays) dispatch_groups = *definition.dispatch.rays;
    std::optional<ImageExtentDispatchRecord>
        image_extent_dispatch;
    if (definition.dispatch.groups_from ||
        definition.dispatch.rays_from) {
        const auto resolved_dispatch =
            resolveImageExtentDispatch(
                definition, resolved_resources,
                render_target_container);
        dispatch_groups = definition.ray_tracing
                              ? imageExtentTraceDimensions(
                                    definition,
                                    resolved_dispatch,
                                    render_target_container)
                              : imageExtentDispatchGroups(
                                    definition, resolved_dispatch,
                                    render_target_container,
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
    if (definition.ray_tracing && !indirect_dispatch) {
        validateRayTracingInitialExtent(
            dispatch_groups,
            GET_MODULE(VulkanManageCore)
                .getRuntimeCapabilities()
                .ray_tracing_facts);
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
                render_target_container
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
    auto task_descriptors = createTaskDescriptorSets(
        descriptor_pool.get(), pipeline,
        resolved_resources, resource_interface,
        render_target_container,
        frame_graph_resources,
        descriptor_variant_count);

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
                std::move(
                    task_descriptors
                        .descriptor_sets),
            .bound_image_views =
                std::move(
                    task_descriptors
                        .bound_image_views),
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

ScopedRayGroupLayoutVirtualIncludeHookupOverrideForTesting::
    ScopedRayGroupLayoutVirtualIncludeHookupOverrideForTesting(
        bool enabled)
    : previous_{
          ray_group_layout_virtual_include_hookup_enabled} {
    ray_group_layout_virtual_include_hookup_enabled = enabled;
}

ScopedRayGroupLayoutVirtualIncludeHookupOverrideForTesting::
    ~ScopedRayGroupLayoutVirtualIncludeHookupOverrideForTesting() {
    ray_group_layout_virtual_include_hookup_enabled = previous_;
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
            if (task.ray_tracing) {
                validateRayTracingRebindExtent(
                    *next.dispatch_groups,
                    GET_MODULE(VulkanManageCore)
                        .getRuntimeCapabilities()
                        .ray_tracing_facts);
            }
        }
        auto rebound_descriptors =
            createTaskDescriptorSets(
                next_pool.get(), task.pipeline,
                task.resource_bindings,
                task.resource_interface,
                render_target_container,
                frame_graph_resources,
                static_cast<std::uint32_t>(
                    task.descriptor_sets.size()));
        next.descriptor_sets = std::move(
            rebound_descriptors.descriptor_sets);
        next.bound_image_views = std::move(
            rebound_descriptors.bound_image_views);
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

void ComputeTaskContainer::setRayTracingDispatchExtent(
    ComputeTaskId task_id, std::uint32_t width, std::uint32_t height,
    std::uint32_t depth) {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end()) throw std::runtime_error("Compute task not found");
    auto &task = found->second;
    if (!task.ray_tracing || !task.definition.dispatch.rays) {
        throw std::runtime_error(
            "pelican.ray_tracing.explicit_extent_required@1: runtime extent updates require dispatch.rays");
    }
    const std::array extent{width, height, depth};
    validateRayTracingInitialExtent(extent,
        GET_MODULE(VulkanManageCore).getRuntimeCapabilities().ray_tracing_facts);
    task.dispatch_x = width;
    task.dispatch_y = height;
    task.dispatch_z = depth;
    task.definition.dispatch.rays = extent;
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
            "use setRayTracingDispatchExtent for explicit ray dimensions");
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

void ComputeTaskContainer::setCallableRecord(
    ComputeTaskId task_id, std::uint32_t index, const nlohmann::json &values) {
    auto &record = tasks.at(task_id.value);
    auto &ray = record.definition.ray_tracing;
    if (!ray || !ray->callable_record_layout || index >= ray->callable_records.size()) {
        throw std::runtime_error("pelican.ray_tracing.callable_records_invalid@1: task '" +
            record.definition.name + "' has no callable record at index " + std::to_string(index));
    }
    auto bytes = encodeRayCallableRecord(values, *ray->callable_record_layout, record.definition.name);
    if (ray->callable_records[index] == bytes) return;
    ray->callable_records[index] = std::move(bytes);
    record.callable_records.reset();
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
            cmd_buf, pipeline_factory.layout(record.pipeline),
            record.definition.ray_tracing->acceleration_structure);
    } else {
        frame_resources.bindCompute(
            cmd_buf,
            pipeline_factory.layout(record.pipeline),
            record.definition.ray_query_acceleration_structure.value_or("scene"));
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
        if (record.definition.ray_tracing->callable_record_layout) {
            record.callable_records = pipeline_factory.prepareCallableRecords(
                record.pipeline, record.definition.ray_tracing->callable_records,
                record.callable_records);
            GET_MODULE(DeletionQueue).defer(record.callable_records);
        }
        if (record.indirect_dispatch) {
            const auto &indirect = *record.indirect_dispatch;
            const auto &buffer = GET_MODULE(FrameGraphResourceContainer).buffer(indirect.buffer);
            const auto address = GET_MODULE(VulkanManageCore).getDevice().getBufferAddress(
                vk::BufferDeviceAddressInfo{buffer.buffer.get()}) + indirect.offset;
            pipeline_factory.traceRaysIndirect(cmd_buf, record.pipeline, address,
                frame_resources.rayTracingSceneShaderBindingTables(), record.callable_records.get());
        } else {
            pipeline_factory.traceRays(
                cmd_buf, record.pipeline,
                record.dispatch_x, record.dispatch_y, record.dispatch_z,
                frame_resources.rayTracingSceneShaderBindingTables(),
                record.callable_records.get());
        }
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
    if (definition.index_buffer && to_kind == FramePlanNodeKind::render) {
        destination_stage |= vk::PipelineStageFlagBits::eVertexInput;
        barrier.dstAccessMask |= vk::AccessFlagBits::eIndexRead;
    }
    const auto indirect_compute =
        to_kind == FramePlanNodeKind::compute &&
        (definition.command_layout == FrameGraphBufferCommandLayout::compute_dispatch ||
         definition.command_layout == FrameGraphBufferCommandLayout::ray_dispatch);
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
