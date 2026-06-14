#include "pipelinefactory.hpp"
#include "../log.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <span>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Pelican {

namespace {

size_t hashCombine(size_t seed, size_t value) {
    return seed ^ (value + 0x9e3779b9u + (seed << 6) + (seed >> 2));
}

bool descriptorBindingEquals(const vk::DescriptorSetLayoutBinding &lhs,
                             const vk::DescriptorSetLayoutBinding &rhs) {
    return lhs.binding == rhs.binding && lhs.descriptorType == rhs.descriptorType &&
           lhs.descriptorCount == rhs.descriptorCount && lhs.stageFlags == rhs.stageFlags;
}

size_t descriptorBindingHash(const vk::DescriptorSetLayoutBinding &binding) {
    size_t seed = std::hash<uint32_t>{}(binding.binding);
    seed = hashCombine(seed, std::hash<uint32_t>{}(static_cast<uint32_t>(binding.descriptorType)));
    seed = hashCombine(seed, std::hash<uint32_t>{}(binding.descriptorCount));
    seed = hashCombine(seed, std::hash<VkShaderStageFlags>{}(static_cast<VkShaderStageFlags>(binding.stageFlags)));
    return seed;
}

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size > 0 && size < buffer.size()) {
        buffer.resize(size);
        return std::filesystem::path{buffer}.parent_path();
    }
#else
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return exe.parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::vector<std::byte> readPipelineCacheData(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file.is_open()) {
        return {};
    }

    const auto size = file.tellg();
    if (size <= 0) {
        return {};
    }

    std::vector<std::byte> data(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file.good()) {
        return {};
    }
    return data;
}

vk::UniquePipelineCache createPipelineCache(vk::Device device, const std::filesystem::path &path) {
    const auto cache_data = readPipelineCacheData(path);
    vk::PipelineCacheCreateInfo create_info;
    create_info.initialDataSize = cache_data.size();
    create_info.pInitialData = cache_data.empty() ? nullptr : cache_data.data();
    try {
        return device.createPipelineCacheUnique(create_info);
    } catch (const vk::SystemError &) {
        return device.createPipelineCacheUnique({});
    }
}

uint32_t maxDescriptorSet(const ShaderReflection &reflection) {
    uint32_t max_set = 0;
    for (const auto &binding : reflection.bindings) {
        max_set = std::max(max_set, binding.set);
    }
    return max_set;
}

void validateFullscreenReflection(const ShaderReflection &reflection) {
    if (!reflection.vertex_inputs.empty()) {
        throw std::runtime_error("Fullscreen PipelineFactory only supports shaders without vertex inputs");
    }
}

std::vector<vk::PipelineColorBlendAttachmentState> makeBlendAttachments(size_t count) {
    std::vector<vk::PipelineColorBlendAttachmentState> attachments(count);
    for (auto &attachment : attachments) {
        attachment.blendEnable = false;
        attachment.colorWriteMask = vk::ColorComponentFlagBits::eA | vk::ColorComponentFlagBits::eR |
                                    vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB;
    }
    return attachments;
}

std::vector<vk::PipelineShaderStageCreateInfo> makeShaderStages(vk::ShaderModule vert,
                                                                vk::ShaderModule frag) {
    std::vector<vk::PipelineShaderStageCreateInfo> stages(2);
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = frag;
    stages[1].pName = "main";
    return stages;
}

bool containsShader(std::span<const ShaderBundleId> shaders, ShaderBundleId shader) {
    return std::find(shaders.begin(), shaders.end(), shader) != shaders.end();
}

} // namespace

bool PipelineFactory::DescriptorSetLayoutKey::operator==(const DescriptorSetLayoutKey &other) const {
    return bindings.size() == other.bindings.size() &&
           std::equal(bindings.begin(), bindings.end(), other.bindings.begin(), descriptorBindingEquals);
}

size_t PipelineFactory::DescriptorSetLayoutKeyHash::operator()(const DescriptorSetLayoutKey &key) const {
    size_t seed = std::hash<size_t>{}(key.bindings.size());
    for (const auto &binding : key.bindings) {
        seed = hashCombine(seed, descriptorBindingHash(binding));
    }
    return seed;
}

PipelineFactory::PipelineFactory()
    : device{GET_MODULE(VulkanManageCore).getDevice()}, shader_library{GET_MODULE(ShaderLibrary)},
      pipeline_cache_path{executableDirectory() / "pipeline_cache.bin"},
      pipeline_cache{createPipelineCache(device, pipeline_cache_path)} {}

PipelineFactory::~PipelineFactory() { savePipelineCache(); }

std::vector<PipelineFactory::DescriptorSetLayoutKey>
PipelineFactory::descriptorSetLayoutKeysFor(const ShaderReflection &reflection) const {
    if (reflection.bindings.empty()) {
        return {};
    }

    std::vector<DescriptorSetLayoutKey> keys(maxDescriptorSet(reflection) + 1);
    for (uint32_t set = 0; set < keys.size(); ++set) {
        keys[set].bindings = makeDescriptorSetLayoutBindings(reflection, set);
    }
    return keys;
}

std::vector<vk::DescriptorSetLayout>
PipelineFactory::descriptorSetLayoutsFor(const ShaderReflection &reflection) {
    std::vector<vk::DescriptorSetLayout> layouts;
    for (const auto &key : descriptorSetLayoutKeysFor(reflection)) {
        auto found = descriptor_set_layout_cache.find(key);
        if (found == descriptor_set_layout_cache.end()) {
            const auto create_info = makeDescriptorSetLayoutCreateInfo(key.bindings);
            auto [inserted, _] = descriptor_set_layout_cache.emplace(key, device.createDescriptorSetLayoutUnique(create_info));
            found = inserted;
        }
        layouts.push_back(found->second.get());
    }
    return layouts;
}

vk::UniquePipelineLayout PipelineFactory::createPipelineLayout(
    const ShaderReflection &reflection, std::span<const vk::DescriptorSetLayout> layouts) const {
    const auto push_constants = makePushConstantRanges(reflection);
    const auto create_info = makePipelineLayoutCreateInfo(layouts, push_constants);
    return device.createPipelineLayoutUnique(create_info);
}

vk::UniquePipeline PipelineFactory::createGraphicsPipeline(const GraphicsPipelineDesc &desc,
                                                           vk::PipelineLayout layout) const {
    const auto &vert = shader_library.get(desc.vert);
    const auto &frag = shader_library.get(desc.frag);
    const auto stages = makeShaderStages(vert.module.get(), frag.module.get());

    vk::PipelineVertexInputStateCreateInfo vertex_input_info;

    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    input_assembly.topology = desc.topology;
    input_assembly.primitiveRestartEnable = false;

    vk::PipelineViewportStateCreateInfo viewport;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterization;
    rasterization.depthClampEnable = false;
    rasterization.rasterizerDiscardEnable = false;
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eNone;
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.depthBiasEnable = false;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample;
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    multisample.sampleShadingEnable = false;

    vk::PipelineDepthStencilStateCreateInfo depth;
    depth.depthTestEnable = false;
    depth.depthWriteEnable = false;
    depth.stencilTestEnable = false;

    auto blend_attachments = makeBlendAttachments(desc.color_formats.size());
    vk::PipelineColorBlendStateCreateInfo blend;
    blend.logicOpEnable = false;
    blend.setAttachments(blend_attachments);

    const auto dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state_info;
    dynamic_state_info.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info;
    rendering_info.setColorAttachmentFormats(desc.color_formats);

    vk::GraphicsPipelineCreateInfo create_info;
    create_info.setStages(stages);
    create_info.pVertexInputState = &vertex_input_info;
    create_info.pInputAssemblyState = &input_assembly;
    create_info.pViewportState = &viewport;
    create_info.pRasterizationState = &rasterization;
    create_info.pMultisampleState = &multisample;
    create_info.pDepthStencilState = &depth;
    create_info.pColorBlendState = &blend;
    create_info.pDynamicState = &dynamic_state_info;
    create_info.layout = layout;
    create_info.subpass = 0;

    vk::StructureChain create_info_chain{
        create_info,
        rendering_info,
    };

    auto result = device.createGraphicsPipelineUnique(pipeline_cache.get(), create_info_chain.get());
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed on vkCreateGraphicsPipeline : " + vk::to_string(result.result));
    }
    return std::move(result.value);
}

PipelineFactory::PipelineRecord PipelineFactory::buildGraphicsPipeline(const GraphicsPipelineDesc &desc) {
    if (desc.color_formats.empty()) {
        throw std::runtime_error("GraphicsPipelineDesc requires at least one color format");
    }

    const auto stage_reflections = std::array{
        shader_library.get(desc.vert).reflection,
        shader_library.get(desc.frag).reflection,
    };
    auto merged_reflection = merge(stage_reflections);
    validateFullscreenReflection(merged_reflection);

    auto set_layouts = descriptorSetLayoutsFor(merged_reflection);
    auto pipeline_layout = createPipelineLayout(merged_reflection, set_layouts);
    auto pipeline_object = createGraphicsPipeline(desc, pipeline_layout.get());

    return PipelineRecord{
        desc,
        std::move(merged_reflection),
        std::move(set_layouts),
        std::move(pipeline_layout),
        std::move(pipeline_object),
    };
}

void PipelineFactory::savePipelineCache() noexcept {
    if (!pipeline_cache) {
        return;
    }

    try {
        const auto cache_data = device.getPipelineCacheData(pipeline_cache.get());
        std::ofstream file{pipeline_cache_path, std::ios::binary | std::ios::trunc};
        if (!file.is_open()) {
            return;
        }
        file.write(reinterpret_cast<const char *>(cache_data.data()), static_cast<std::streamsize>(cache_data.size()));
    } catch (...) {
    }
}

PipelineHandle PipelineFactory::create(const GraphicsPipelineDesc &desc) {
    auto handle = pipelines.reg(buildGraphicsPipeline(desc));
    pipeline_handles.push_back(handle);
    return handle;
}

void PipelineFactory::replacePipeline(PipelineHandle handle, PipelineRecord replacement) {
    auto &current = pipelines.get(handle);
    auto old_pipeline = std::move(current.pipeline);
    auto old_layout = std::move(current.layout);

    current = std::move(replacement);

    auto &deletion_queue = GET_MODULE(DeletionQueue);
    if (old_pipeline) {
        deletion_queue.defer(std::move(old_pipeline));
    }
    if (old_layout) {
        deletion_queue.defer(std::move(old_layout));
    }
}

vk::Pipeline PipelineFactory::pipeline(PipelineHandle handle) const { return pipelines.get(handle).pipeline.get(); }

vk::PipelineLayout PipelineFactory::layout(PipelineHandle handle) const { return pipelines.get(handle).layout.get(); }

vk::DescriptorSetLayout PipelineFactory::descriptorSetLayout(PipelineHandle handle, uint32_t set) const {
    const auto &layouts = pipelines.get(handle).descriptor_set_layouts;
    if (set >= layouts.size()) {
        throw std::runtime_error("Pipeline descriptor set layout not found");
    }
    return layouts[set];
}

const ShaderReflection &PipelineFactory::reflection(PipelineHandle handle) const {
    return pipelines.get(handle).reflection;
}

void PipelineFactory::rebuildDirty() {
    const auto dirty_shaders = shader_library.takeDirtyBundles();
    if (dirty_shaders.empty()) {
        return;
    }

    for (const auto handle : pipeline_handles) {
        const auto &record = pipelines.get(handle);
        if (!containsShader(dirty_shaders, record.desc.vert) && !containsShader(dirty_shaders, record.desc.frag)) {
            continue;
        }

        try {
            auto replacement = buildGraphicsPipeline(record.desc);
            replacePipeline(handle, std::move(replacement));
        } catch (const std::exception &ex) {
            LOG_WARNING(logger, "Pipeline hot reload failed; keeping previous pipeline: {}", ex.what());
        }
    }
}

} // namespace Pelican
