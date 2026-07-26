#include "computetask.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../renderer/frameresources.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/render_target_layout_tracker.hpp"
#include "../vkcore/util.hpp"
#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

constexpr uint32_t max_compute_descriptor_sets = 256;
constexpr uint32_t max_compute_descriptors = 1024;

struct RetiredComputeDescriptorResources {
    vk::UniqueDescriptorPool pool;
    std::vector<std::array<vk::UniqueDescriptorSet, 2>> descriptor_sets;
};

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

const ShaderResourcePortDefinition *resourcePort(
    const ComputeTaskDefinition &definition,
    std::string_view authored_resource) {
    const auto found = std::find_if(
        definition.resource_ports.begin(),
        definition.resource_ports.end(),
        [&](const ShaderResourcePortDefinition &port) {
            return port.resource == authored_resource;
        });
    return found == definition.resource_ports.end()
               ? nullptr
               : &*found;
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
    for (std::size_t index = 0;
         index < resources.size(); ++index) {
        auto &resource = resources[index];
        const auto *port =
            resourcePort(
                definition, resource.authored_name);
        if (port == nullptr) continue;
        if (isValidFrameGraphBufferId(resource.buffer)) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') cannot type a frame-graph buffer yet; use the raw "
                "storage-buffer layout");
        }
        if (!isConcreteRenderTarget(
                resource.render_target)) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') does not resolve to an image");
        }
        if (dependencies.resource_views == nullptr) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') requires a physical target-plan view");
        }
        const auto physical =
            dependencies.resource_views->find(
                resource.name);
        if (physical ==
            dependencies.resource_views->end()) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') is absent from the physical target plan");
        }
        resource.physical_view = physical->second;

        const auto metadata =
            dependencies.render_target_container
                .getMetadata(resource.render_target);
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
                *port, true, written);
        const auto sampled =
            access ==
            ShaderResourcePortAccess::sampled;
        const auto required_usage =
            sampled
                ? vk::ImageUsageFlagBits::eSampled
                : vk::ImageUsageFlagBits::eStorage;
        if (!(metadata.usage & required_usage)) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') requires render-target usage " +
                std::string{
                    sampled ? "sampled" : "storage"});
        }
        const auto dimension =
            resolveShaderResourceImageViewDimension(
                *port, physical->second,
                ShaderResourceConsumerView::
                    compute_once);
        if (dimension ==
                ReflectedImageViewDimension::
                    two_d_array &&
            metadata.array_layers < 2) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') requires a 2D-array image with at least two layers");
        }
        result.push_back(
            ShaderResourceInterfaceBinding{
                .port = *port,
                .binding =
                    static_cast<std::uint32_t>(
                        index),
                .descriptor =
                    sampled
                        ? ShaderResourceDescriptorKind::
                              combined_image_sampler
                        : ShaderResourceDescriptorKind::
                              storage_image,
                .image_view_dimension = dimension,
                .storage_format =
                    sampled ? vk::Format::eUndefined
                            : metadata.format,
                .readable = readable,
                .writable = written,
            });
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
    create_info.maxLod = 0.0f;
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
    return kind == FramePlanNodeKind::compute ? vk::PipelineStageFlagBits::eComputeShader
                                              : vk::PipelineStageFlagBits::eFragmentShader;
}

ComputeDispatchDefinition parseDispatch(const nlohmann::json &task_json, const std::string &name) {
    ComputeDispatchDefinition dispatch;
    if (!task_json.contains("dispatch")) {
        return dispatch;
    }

    const auto &json = task_json.at("dispatch");
    if (!json.is_object()) {
        throw std::runtime_error("compute task dispatch must be an object: " + name);
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
        dispatch.groups_from = requireString(json, "groups_from", "compute task dispatch: " + name);
    }
    if (json.contains("local_size")) {
        dispatch.local_size = requireUint32(json, "local_size", "compute task dispatch: " + name);
    }
    if (dispatch.groups_x == 0 || dispatch.groups_y == 0 || dispatch.groups_z == 0) {
        throw std::runtime_error("compute task dispatch group counts must be positive: " + name);
    }
    return dispatch;
}

} // namespace

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
            definitions.push_back(FrameGraphBufferDefinition{entry.get<std::string>(), 0, true});
            continue;
        }
        if (!entry.is_object()) {
            throw std::runtime_error("buffers entries must be strings or objects");
        }
        auto definition = FrameGraphBufferDefinition{
            requireString(entry, "name", "buffer"),
            entry.contains("size") ? requireDeviceSize(entry, "size", "buffer") : vk::DeviceSize{0},
            true,
        };
        if (entry.contains("lifetime")) {
            const auto lifetime = requireString(entry, "lifetime", "buffer: " + definition.name);
            if (lifetime == "persistent") {
                definition.persistent = true;
            } else if (lifetime == "transient") {
                definition.persistent = false;
            } else {
                throw std::runtime_error("buffer lifetime must be persistent or transient: " + definition.name);
            }
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
        definition.shader = makeShaderReference(requireString(task_json, "shader", "compute task: " + definition.name),
                                                ShaderStage::compute);
        definition.reads = parseOptionalStringList(task_json, "reads", "compute task: " + definition.name);
        definition.writes = parseOptionalStringList(task_json, "writes", "compute task: " + definition.name);
        definition.after = parseOptionalStringList(task_json, "after", "compute task: " + definition.name);
        definition.before = parseOptionalStringList(task_json, "before", "compute task: " + definition.name);
        definition.resource_ports =
            parseShaderResourcePortDefinitions(
                task_json, definition.reads,
                definition.writes,
                "compute task '" + definition.name + "'");
        definition.dispatch = parseDispatch(task_json, definition.name);
        if (task_json.contains("schedule")) {
            definition.schedule = requireString(task_json, "schedule", "compute task: " + definition.name);
        }
        if (definition.schedule != "per_frame") {
            throw std::runtime_error("compute task schedule only supports per_frame in v1: " + definition.name);
        }
        definitions.push_back(std::move(definition));
    }
    return definitions;
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
        registration_order.reserve(
            registration_order.size() + 1);
        auto buffer = vkcore.allocBuf(definition.size,
                                      vk::BufferUsageFlagBits::eStorageBuffer |
                                          vk::BufferUsageFlagBits::eTransferSrc |
                                          vk::BufferUsageFlagBits::eTransferDst,
                                      vma::MemoryUsage::eAutoPreferDevice,
                                      {});
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
    const auto &record = buffers.get(id);
    return vk::DescriptorBufferInfo{record.buffer.buffer.get(), 0, record.definition.size};
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
    std::uint32_t frame_index) {
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
            image_infos.push_back(vk::DescriptorImageInfo{
                sampled
                    ? samplerFor(
                          typed->port.sampling)
                    : vk::Sampler{},
                layered
                    ? render_target_container
                          .getLayeredImageViewForFrame(
                              rt_id,
                              resource.history_read,
                              frame_index)
                    : render_target_container
                          .getImageViewForFrame(
                              rt_id,
                              resource.history_read,
                              frame_index),
                sampled
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
    auto resource_interface =
        makeComputeResourceInterface(
            definition, resolved_resources,
            dependencies);

    auto &shader_library = dependencies.shader_library;
    std::vector<std::pair<std::string, std::string>>
        virtual_includes;
    if (!resource_interface.empty()) {
        virtual_includes =
            makeShaderResourcePortVirtualIncludes(
                resource_interface);
    }
    const auto shader =
        shader_library.loadFromReference(
            definition.shader,
            dependencies.path_resolver, true, {},
            std::move(virtual_includes));
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline =
        pipeline_factory.createCompute(
            ComputePipelineDesc{
                .shader = shader,
                .resource_interface =
                    resource_interface,
            });
    std::array<vk::UniqueDescriptorSet, 2> descriptor_sets;
    std::array<std::vector<vk::ImageView>, 2> bound_image_views;
    for (std::uint32_t frame_index = 0; frame_index < 2; ++frame_index) {
        auto binding = createDescriptorSet(
            descriptor_pool.get(), pipeline,
            resolved_resources,
            resource_interface,
            dependencies.render_target_container,
            dependencies.frame_graph_resources, frame_index);
        descriptor_sets[frame_index] = std::move(binding.descriptor_set);
        bound_image_views[frame_index] = std::move(binding.bound_image_views);
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
            .dispatch_x =
                definition.dispatch.groups_x,
            .dispatch_y =
                definition.dispatch.groups_y,
            .dispatch_z =
                definition.dispatch.groups_z,
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
        std::array<vk::UniqueDescriptorSet, 2> descriptor_sets;
        std::array<std::vector<vk::ImageView>, 2> bound_image_views;
    };
    std::vector<ReboundTask> rebound;
    rebound.reserve(tasks.size());
    const auto &frame_graph_resources =
        GET_MODULE(FrameGraphResourceContainer);
    for (const auto &[id, task] : tasks) {
        ReboundTask next;
        next.id = id;
        for (std::uint32_t frame_index = 0; frame_index < 2; ++frame_index) {
            auto binding = createDescriptorSet(
                next_pool.get(), task.pipeline,
                task.resource_bindings,
                task.resource_interface,
                render_target_container,
                frame_graph_resources, frame_index);
            next.descriptor_sets[frame_index] =
                std::move(binding.descriptor_set);
            next.bound_image_views[frame_index] =
                std::move(binding.bound_image_views);
        }
        rebound.push_back(std::move(next));
    }

    RetiredComputeDescriptorResources retired;
    retired.pool = std::move(descriptor_pool);
    retired.descriptor_sets.reserve(tasks.size());
    for (auto &[id, task] : tasks) {
        (void)id;
        retired.descriptor_sets.push_back(std::move(task.descriptor_sets));
    }
    descriptor_pool = std::move(next_pool);
    for (auto &next : rebound) {
        auto &task = tasks.at(next.id);
        task.descriptor_sets = std::move(next.descriptor_sets);
        task.bound_image_views = std::move(next.bound_image_views);
        task.binding_revision = next_binding_revision++;
    }
    GET_MODULE(DeletionQueue).defer(std::move(retired));
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
    for (const auto &resource :
         found->second.resource_bindings) {
        const auto rt_id = resource.render_target;
        if (isConcreteRenderTarget(rt_id)) {
            const auto typed = std::find_if(
                found->second.resource_interface.begin(),
                found->second.resource_interface.end(),
                [&](const ShaderResourceInterfaceBinding
                        &candidate) {
                    return candidate.port.resource ==
                           resource.authored_name;
                });
            const auto desired_layout =
                typed !=
                            found->second
                                .resource_interface.end() &&
                        typed->descriptor ==
                            ShaderResourceDescriptorKind::
                                combined_image_sampler
                    ? vk::ImageLayout::
                          eShaderReadOnlyOptimal
                    : vk::ImageLayout::eGeneral;
            if (layout_tracker.currentLayout(rt_id, resource.history_read,
                                             &render_target_container) ==
                desired_layout) {
                layout_tracker.memoryDependency(cmd_buf, render_target_container,
                                                vk_utils, rt_id,
                                                resource.history_read);
            } else {
                layout_tracker.transition(cmd_buf, render_target_container, vk_utils,
                                          rt_id, desired_layout,
                                          resource.history_read);
            }
        }
    }
}

void ComputeTaskContainer::dispatch(
    vk::CommandBuffer cmd_buf, ComputeTaskId task_id,
    const FrameResources &frame_resources) const {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end()) {
        throw std::runtime_error("Compute task not found");
    }
    const auto &record = found->second;
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_factory.pipeline(record.pipeline));
    frame_resources.bindCompute(
        cmd_buf,
        pipeline_factory.layout(record.pipeline));
    const auto parity = GET_MODULE(RenderTargetContainer).historyFrameIndex();
    if (record.descriptor_sets[parity]) {
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_factory.layout(record.pipeline),
                                   PELICAN_SET_PASS_INPUT,
                                   record.descriptor_sets[parity].get(), {});
    }
    cmd_buf.dispatch(record.dispatch_x, record.dispatch_y, record.dispatch_z);
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
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = resource_container.buffer(resource).buffer.get();
    barrier.offset = 0;
    barrier.size = resource_container.bufferSize(resource);
    cmd_buf.pipelineBarrier(shaderStage(from_kind), shaderStage(to_kind), {}, {}, {barrier}, {});
}

std::vector<vk::ImageView> ComputeTaskContainer::boundImageViewsForTesting(
    ComputeTaskId task_id, std::uint32_t frame_index) const {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end() || frame_index >= 2) {
        return {};
    }
    return found->second.bound_image_views[frame_index];
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
