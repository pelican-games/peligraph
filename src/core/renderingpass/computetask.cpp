#include "computetask.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/render_target_layout_tracker.hpp"
#include "../vkcore/util.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

constexpr uint32_t max_compute_descriptor_sets = 128;
constexpr uint32_t max_compute_descriptors = 512;

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

std::vector<std::string> taskResources(const ComputeTaskDefinition &definition) {
    std::vector<std::string> resources;
    for (const auto &resource : definition.reads) {
        appendUnique(resources, resource);
    }
    for (const auto &resource : definition.writes) {
        appendUnique(resources, resource);
    }
    return resources;
}

bool resourceExists(const std::string &resource, const ComputeTaskRuntimeDependencies &dependencies) {
    if (GET_MODULE(FrameGraphResourceContainer).hasBuffer(resource)) {
        return true;
    }
    return isConcreteRenderTarget(dependencies.render_target_container.getRenderTargetIdByName(resource));
}

std::string resourceForBinding(const ReflectedBinding &binding,
                               size_t binding_index,
                               const std::vector<std::string> &resources,
                               const ComputeTaskRuntimeDependencies &dependencies) {
    if (!binding.name.empty() && std::find(resources.begin(), resources.end(), binding.name) != resources.end() &&
        resourceExists(binding.name, dependencies)) {
        return binding.name;
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

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
    std::array<vk::DescriptorPoolSize, 2> pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, max_compute_descriptors},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, max_compute_descriptors},
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
        if (buffers.find(definition.name) != buffers.end()) {
            continue;
        }
        auto buffer = vkcore.allocBuf(definition.size,
                                      vk::BufferUsageFlagBits::eStorageBuffer |
                                          vk::BufferUsageFlagBits::eTransferSrc |
                                          vk::BufferUsageFlagBits::eTransferDst,
                                      vma::MemoryUsage::eAutoPreferDevice,
                                      {});
        buffers.emplace(definition.name, BufferRecord{definition, std::move(buffer)});
    }
}

bool FrameGraphResourceContainer::hasBuffer(std::string_view name) const {
    return buffers.find(std::string{name}) != buffers.end();
}

const BufferWrapper &FrameGraphResourceContainer::buffer(std::string_view name) const {
    const auto found = buffers.find(std::string{name});
    if (found == buffers.end()) {
        throw std::runtime_error("Frame graph buffer not found: " + std::string{name});
    }
    return found->second.buffer;
}

vk::DeviceSize FrameGraphResourceContainer::bufferSize(std::string_view name) const {
    const auto found = buffers.find(std::string{name});
    if (found == buffers.end()) {
        throw std::runtime_error("Frame graph buffer not found: " + std::string{name});
    }
    return found->second.definition.size;
}

vk::DescriptorBufferInfo FrameGraphResourceContainer::descriptorInfo(std::string_view name) const {
    const auto &record = buffers.at(std::string{name});
    return vk::DescriptorBufferInfo{record.buffer.buffer.get(), 0, record.definition.size};
}

ComputeTaskContainer::ComputeTaskContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      descriptor_pool{createDescriptorPool(device)} {}

ComputeTaskContainer::~ComputeTaskContainer() = default;

vk::UniqueDescriptorSet ComputeTaskContainer::createDescriptorSet(
    PipelineHandle pipeline,
    const ComputeTaskDefinition &definition,
    const ComputeTaskRuntimeDependencies &dependencies) const {
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto bindings = passInputBindings(pipeline_factory.reflection(pipeline));
    if (bindings.empty()) {
        return {};
    }

    const auto layout = pipeline_factory.descriptorSetLayout(pipeline, PELICAN_SET_PASS_INPUT);
    vk::DescriptorSetAllocateInfo alloc_info;
    alloc_info.descriptorPool = descriptor_pool.get();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;
    auto descriptor_sets = device.allocateDescriptorSetsUnique(alloc_info);
    auto descriptor_set = std::move(descriptor_sets.front());

    const auto resources = taskResources(definition);
    std::vector<vk::DescriptorBufferInfo> buffer_infos;
    std::vector<vk::DescriptorImageInfo> image_infos;
    std::vector<vk::WriteDescriptorSet> writes;
    buffer_infos.reserve(bindings.size());
    image_infos.reserve(bindings.size());
    writes.reserve(bindings.size());

    auto &resource_container = GET_MODULE(FrameGraphResourceContainer);
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        const auto resource = resourceForBinding(binding, i, resources, dependencies);

        vk::WriteDescriptorSet write;
        write.dstSet = descriptor_set.get();
        write.dstBinding = binding.binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = binding.type;

        if (resource_container.hasBuffer(resource)) {
            if (binding.type != vk::DescriptorType::eStorageBuffer) {
                throw std::runtime_error("Compute task buffer binding must be a storage buffer: " + definition.name);
            }
            buffer_infos.push_back(resource_container.descriptorInfo(resource));
            write.pBufferInfo = &buffer_infos.back();
        } else {
            const auto rt_id = dependencies.render_target_container.getRenderTargetIdByName(resource);
            if (!isConcreteRenderTarget(rt_id)) {
                throw std::runtime_error("Compute task resource not found: " + resource);
            }
            if (binding.type != vk::DescriptorType::eStorageImage) {
                throw std::runtime_error("Compute task render target binding must be a storage image: " +
                                         definition.name);
            }
            image_infos.push_back(vk::DescriptorImageInfo{
                {},
                dependencies.render_target_container.getImageView(rt_id),
                vk::ImageLayout::eGeneral,
            });
            write.pImageInfo = &image_infos.back();
        }
        writes.push_back(write);
    }

    device.updateDescriptorSets(writes, {});
    return descriptor_set;
}

ComputeTaskId ComputeTaskContainer::registerComputeTask(
    const ComputeTaskDefinition &definition,
    const ComputeTaskRuntimeDependencies &dependencies) {
    if (auto found = name_to_id.find(definition.name); found != name_to_id.end()) {
        return found->second;
    }

    auto &shader_library = dependencies.shader_library;
    const auto shader = shader_library.loadFromReference(definition.shader, dependencies.path_resolver, true);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline = pipeline_factory.createCompute(ComputePipelineDesc{shader});
    auto descriptor_set = createDescriptorSet(pipeline, definition, dependencies);

    const auto id = ComputeTaskId{static_cast<int>(tasks.size())};
    tasks.emplace(id.value,
                  TaskRecord{
                      definition,
                      pipeline,
                      std::move(descriptor_set),
                      definition.dispatch.groups_x,
                      definition.dispatch.groups_y,
                      definition.dispatch.groups_z,
                  });
    name_to_id.emplace(definition.name, id);
    return id;
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
    const auto &task = definition(task_id);
    std::vector<std::string> resources = taskResources(task);
    for (const auto &resource : resources) {
        const auto rt_id = render_target_container.getRenderTargetIdByName(resource);
        if (isConcreteRenderTarget(rt_id)) {
            layout_tracker.transition(cmd_buf, render_target_container, vk_utils, rt_id,
                                      vk::ImageLayout::eGeneral);
        }
    }
}

void ComputeTaskContainer::dispatch(vk::CommandBuffer cmd_buf, ComputeTaskId task_id) const {
    const auto found = tasks.find(task_id.value);
    if (found == tasks.end()) {
        throw std::runtime_error("Compute task not found");
    }
    const auto &record = found->second;
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_factory.pipeline(record.pipeline));
    if (record.descriptor_set) {
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_factory.layout(record.pipeline),
                                   PELICAN_SET_PASS_INPUT, record.descriptor_set.get(), {});
    }
    cmd_buf.dispatch(record.dispatch_x, record.dispatch_y, record.dispatch_z);
}

void ComputeTaskContainer::bufferReadAfterWriteBarrier(vk::CommandBuffer cmd_buf,
                                                       const std::string &resource,
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

} // namespace Pelican
