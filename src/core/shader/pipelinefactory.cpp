#include "pipelinefactory.hpp"
#include "pelican_sets.hpp"
#include "../log.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <variant>

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

std::vector<vk::DescriptorSetLayoutBinding> frameDescriptorSetLayoutBindings() {
    const auto all_stages = vk::ShaderStageFlagBits::eAll;
    return {
        vk::DescriptorSetLayoutBinding{PELICAN_FRAME_UBO_BINDING, vk::DescriptorType::eUniformBuffer, 1,
                                       all_stages},
        vk::DescriptorSetLayoutBinding{PELICAN_OBJECT_BUFFER_BINDING, vk::DescriptorType::eStorageBuffer, 1,
                                       all_stages},
        vk::DescriptorSetLayoutBinding{PELICAN_LIGHT_UBO_BINDING, vk::DescriptorType::eUniformBuffer, 1,
                                       all_stages},
        vk::DescriptorSetLayoutBinding{PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING,
                                       vk::DescriptorType::eStorageBuffer, 1, all_stages},
        vk::DescriptorSetLayoutBinding{
            PELICAN_FRAME_RESOLUTION_UBO_BINDING,
            vk::DescriptorType::eUniformBuffer, 1, all_stages},
        vk::DescriptorSetLayoutBinding{
            PELICAN_DIRECTIONAL_SHADOW_DATA_BINDING,
            vk::DescriptorType::eStorageBuffer, 1, all_stages},
    };
}

void validateFrameBindings(const ShaderReflection &reflection) {
    for (const auto &binding : reflection.bindings) {
        if (binding.set != PELICAN_SET_FRAME) {
            continue;
        }
        const auto valid =
            (binding.binding == PELICAN_FRAME_UBO_BINDING &&
             binding.type == vk::DescriptorType::eUniformBuffer) ||
            (binding.binding == PELICAN_OBJECT_BUFFER_BINDING &&
             binding.type == vk::DescriptorType::eStorageBuffer) ||
            (binding.binding == PELICAN_LIGHT_UBO_BINDING &&
             binding.type == vk::DescriptorType::eUniformBuffer) ||
            (binding.binding == PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING &&
             binding.type == vk::DescriptorType::eStorageBuffer) ||
            (binding.binding ==
                 PELICAN_FRAME_RESOLUTION_UBO_BINDING &&
             binding.type ==
                 vk::DescriptorType::eUniformBuffer) ||
            (binding.binding ==
                 PELICAN_DIRECTIONAL_SHADOW_DATA_BINDING &&
             binding.type ==
                 vk::DescriptorType::eStorageBuffer);
        if (!valid || binding.count != 1) {
            throw std::runtime_error(
                "Shader set 0 must use FrameUBO binding 0, ObjectBuffer binding 1, LightUBO binding 2, "
                "PreviousObjectBuffer binding 3, FrameResolutionUBO binding 4, or "
                "DirectionalShadowData binding 5");
        }
    }
}

void validateGraphicsReflection(const GraphicsPipelineDesc &desc, const ShaderReflection &reflection) {
    if (!desc.color_attachment_states.empty() &&
        desc.color_attachment_states.size() !=
            desc.color_formats.size()) {
        throw std::runtime_error(
            "GraphicsPipelineDesc color_attachment_states count "
            "must match color_formats");
    }
    if (desc.view.execution ==
        GraphicsPipelineViewExecution::single_view) {
        if (desc.view.view_count != 1 ||
            desc.view.view_mask != 0) {
            throw std::runtime_error(
                "single-view graphics pipeline requires view_count=1 and view_mask=0");
        }
    } else {
        if (desc.view.view_count < 2 ||
            desc.view.view_count > 32) {
            throw std::runtime_error(
                "multiview graphics pipeline view_count must be in [2, 32]");
        }
        const auto expected_mask =
            desc.view.view_count == 32
                ? std::numeric_limits<std::uint32_t>::max()
                : (std::uint32_t{1} << desc.view.view_count) - 1u;
        if (desc.view.view_mask != expected_mask) {
            throw std::runtime_error(
                "multiview graphics pipeline requires a contiguous view mask");
        }
        if (!reflection.uses_view_index) {
            throw std::runtime_error(
                "multiview graphics pipeline shader contract must consume gl_ViewIndex");
        }
    }
    const bool custom_vertex_layout = !desc.vertex_bindings.empty() || !desc.vertex_attributes.empty();
    if (!desc.use_engine_vertex_layout && !desc.use_skinned_vertex_layout && !custom_vertex_layout && !reflection.vertex_inputs.empty()) {
        throw std::runtime_error("GraphicsPipelineDesc requires use_engine_vertex_layout for vertex input shaders");
    }
    if (desc.use_engine_vertex_layout && desc.use_skinned_vertex_layout) {
        throw std::runtime_error("GraphicsPipelineDesc cannot select both engine vertex layouts");
    }
    if (custom_vertex_layout && (desc.use_engine_vertex_layout || desc.use_skinned_vertex_layout))
        throw std::runtime_error("GraphicsPipelineDesc custom and engine vertex layouts are mutually exclusive");
    if (custom_vertex_layout) {
        if (desc.vertex_bindings.empty() || desc.vertex_attributes.size() != reflection.vertex_inputs.size())
            throw std::runtime_error("GraphicsPipelineDesc custom vertex layout does not match shader reflection");
        for (const auto &input : reflection.vertex_inputs) {
            const auto found = std::find_if(desc.vertex_attributes.begin(), desc.vertex_attributes.end(),
                                            [&](const auto &attribute) { return attribute.location == input.location; });
            if (found == desc.vertex_attributes.end())
                throw std::runtime_error("GraphicsPipelineDesc custom vertex attribute location does not match shader reflection");
        }
    }
    validateFrameBindings(reflection);
    validatePushConstantContract(reflection);
}

std::vector<vk::PipelineColorBlendAttachmentState> makeBlendAttachments(const GraphicsPipelineDesc &desc,
                                                                        size_t count) {
    std::vector<vk::PipelineColorBlendAttachmentState> attachments(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto &attachment = attachments[index];
        if (desc.color_attachment_states.empty()) {
            attachment.blendEnable = desc.blend;
            attachment.srcColorBlendFactor =
                desc.src_color_blend_factor;
            attachment.dstColorBlendFactor =
                desc.dst_color_blend_factor;
            attachment.colorBlendOp =
                desc.color_blend_op;
            attachment.srcAlphaBlendFactor =
                desc.src_alpha_blend_factor;
            attachment.dstAlphaBlendFactor =
                desc.dst_alpha_blend_factor;
            attachment.alphaBlendOp =
                desc.alpha_blend_op;
            attachment.colorWriteMask =
                vk::ColorComponentFlagBits::eA |
                vk::ColorComponentFlagBits::eR |
                vk::ColorComponentFlagBits::eG |
                vk::ColorComponentFlagBits::eB;
            continue;
        }
        const auto &state =
            desc.color_attachment_states[index];
        attachment.blendEnable = state.blend_enabled;
        attachment.srcColorBlendFactor =
            state.source_color;
        attachment.dstColorBlendFactor =
            state.destination_color;
        attachment.colorBlendOp =
            state.color_operation;
        attachment.srcAlphaBlendFactor =
            state.source_alpha;
        attachment.dstAlphaBlendFactor =
            state.destination_alpha;
        attachment.alphaBlendOp =
            state.alpha_operation;
        attachment.colorWriteMask =
            state.write_mask;
    }
    return attachments;
}

std::vector<vk::PipelineShaderStageCreateInfo> makeShaderStages(vk::ShaderModule vert,
                                                                std::optional<vk::ShaderModule> frag) {
    std::vector<vk::PipelineShaderStageCreateInfo> stages;
    stages.reserve(frag ? 2 : 1);
    stages.emplace_back();
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = vert;
    stages[0].pName = "main";
    if (frag) {
        stages.emplace_back();
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = *frag;
        stages[1].pName = "main";
    }
    return stages;
}

bool containsShader(std::span<const ShaderBundleId> shaders, ShaderBundleId shader) {
    return std::find(shaders.begin(), shaders.end(), shader) != shaders.end();
}

bool pipelineUsesShader(const std::variant<GraphicsPipelineDesc, ComputePipelineDesc> &desc,
                        std::span<const ShaderBundleId> dirty_shaders) {
    return std::visit(
        [dirty_shaders](const auto &pipeline_desc) {
            using Desc = std::decay_t<decltype(pipeline_desc)>;
            if constexpr (std::is_same_v<Desc, GraphicsPipelineDesc>) {
                return containsShader(dirty_shaders, pipeline_desc.vert) ||
                       (pipeline_desc.frag && containsShader(dirty_shaders, *pipeline_desc.frag));
            } else {
                return containsShader(dirty_shaders, pipeline_desc.shader);
            }
        },
        desc);
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
    std::vector<DescriptorSetLayoutKey> keys(maxDescriptorSet(reflection) + 1);
    keys[PELICAN_SET_FRAME].bindings = frameDescriptorSetLayoutBindings();
    for (uint32_t set = 1; set < keys.size(); ++set) {
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
    std::optional<vk::ShaderModule> frag_module;
    if (desc.frag) {
        frag_module = shader_library.get(*desc.frag).module.get();
    }
    const auto stages = makeShaderStages(vert.module.get(), frag_module);

    vk::PipelineVertexInputStateCreateInfo vertex_input_info;
    VertBufContainer::CommonVertDataDescription engine_vertex_input;
    if (desc.use_engine_vertex_layout || desc.use_skinned_vertex_layout) {
        engine_vertex_input = desc.use_skinned_vertex_layout ? VertBufContainer::getSkinnedDescription()
                                                             : VertBufContainer::getDescription();
        vertex_input_info.setVertexAttributeDescriptions(engine_vertex_input.attr_descs);
        vertex_input_info.setVertexBindingDescriptions(engine_vertex_input.binding_descs);
    } else if (!desc.vertex_bindings.empty() || !desc.vertex_attributes.empty()) {
        vertex_input_info.setVertexAttributeDescriptions(desc.vertex_attributes);
        vertex_input_info.setVertexBindingDescriptions(desc.vertex_bindings);
    }

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
    rasterization.cullMode = desc.cull_mode;
    rasterization.frontFace = desc.front_face;
    rasterization.depthBiasEnable = false;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample;
    multisample.rasterizationSamples = desc.rasterization_samples;
    multisample.sampleShadingEnable = false;

    vk::PipelineDepthStencilStateCreateInfo depth;
    depth.depthTestEnable = desc.depth_test;
    depth.depthWriteEnable = desc.depth_write;
    depth.depthCompareOp = desc.depth_compare;
    depth.stencilTestEnable = false;

    auto blend_attachments = makeBlendAttachments(desc, desc.color_formats.size());
    vk::PipelineColorBlendStateCreateInfo blend;
    blend.logicOpEnable = false;
    blend.setAttachments(blend_attachments);

    const auto dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state_info;
    dynamic_state_info.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info;
    rendering_info.setColorAttachmentFormats(desc.color_formats);
    rendering_info.viewMask = desc.view.view_mask;
    if (desc.depth_format) {
        rendering_info.depthAttachmentFormat = *desc.depth_format;
    }

    vk::RenderingAttachmentLocationInfoKHR
        attachment_locations;
    attachment_locations
        .setColorAttachmentLocations(
            desc.local_read
                .color_attachment_locations);
    auto depth_input =
        desc.local_read
            .depth_attachment_input_index;
    auto stencil_input =
        desc.local_read
            .stencil_attachment_input_index;
    vk::RenderingInputAttachmentIndexInfoKHR
        input_attachment_indices;
    input_attachment_indices
        .setColorAttachmentInputIndices(
            desc.local_read
                .color_attachment_input_indices);
    if (depth_input !=
        unusedGraphicsAttachmentMapping) {
        input_attachment_indices
            .pDepthInputAttachmentIndex =
            &depth_input;
    }
    if (stencil_input !=
        unusedGraphicsAttachmentMapping) {
        input_attachment_indices
            .pStencilInputAttachmentIndex =
            &stencil_input;
    }

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
        attachment_locations,
        input_attachment_indices,
    };
    if (!desc.local_read.enabled) {
        create_info_chain
            .unlink<
                vk::RenderingAttachmentLocationInfoKHR>();
        create_info_chain
            .unlink<
                vk::RenderingInputAttachmentIndexInfoKHR>();
    }

    auto result = device.createGraphicsPipelineUnique(pipeline_cache.get(), create_info_chain.get());
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed on vkCreateGraphicsPipeline : " + vk::to_string(result.result));
    }
    return std::move(result.value);
}

vk::UniquePipeline PipelineFactory::createComputePipeline(const ComputePipelineDesc &desc,
                                                          vk::PipelineLayout layout) const {
    const auto &shader = shader_library.get(desc.shader);

    vk::PipelineShaderStageCreateInfo stage;
    stage.stage = vk::ShaderStageFlagBits::eCompute;
    stage.module = shader.module.get();
    stage.pName = "main";

    vk::ComputePipelineCreateInfo create_info;
    create_info.stage = stage;
    create_info.layout = layout;

    auto result = device.createComputePipelineUnique(pipeline_cache.get(), create_info);
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed on vkCreateComputePipeline : " + vk::to_string(result.result));
    }
    return std::move(result.value);
}

PipelineFactory::PipelineRecord PipelineFactory::buildGraphicsPipeline(const GraphicsPipelineDesc &desc) {
    if (desc.color_formats.empty() && !desc.depth_format) {
        throw std::runtime_error("GraphicsPipelineDesc requires at least one color or depth format");
    }
    if (desc.local_read.enabled) {
        if (!GET_MODULE(VulkanManageCore)
                 .getRuntimeCapabilities()
                 .dynamic_rendering_local_read) {
            throw std::runtime_error(
                "GraphicsPipelineDesc local read requires the enabled "
                "dynamic-rendering-local-read device feature");
        }
        if (desc.rasterization_samples !=
            vk::SampleCountFlagBits::e1) {
            throw std::runtime_error(
                "GraphicsPipelineDesc local read currently requires "
                "single-sample rasterization");
        }
        if (desc.local_read
                    .color_attachment_locations
                    .size() !=
                desc.color_formats.size() ||
            desc.local_read
                    .color_attachment_input_indices
                    .size() !=
                desc.color_formats.size()) {
            throw std::runtime_error(
                "GraphicsPipelineDesc local-read mappings must match "
                "the color attachment format count");
        }
        if (!desc.frag) {
            throw std::runtime_error(
                "GraphicsPipelineDesc local read requires a fragment "
                "shader");
        }
        if (!desc.depth_format &&
            desc.local_read
                    .depth_attachment_input_index !=
                unusedGraphicsAttachmentMapping) {
            throw std::runtime_error(
                "GraphicsPipelineDesc local read maps an absent depth "
                "attachment");
        }
    }

    std::vector<ShaderReflection> stage_reflections;
    stage_reflections.push_back(shader_library.get(desc.vert).reflection);
    if (desc.frag) {
        stage_reflections.push_back(shader_library.get(*desc.frag).reflection);
    }
    auto merged_reflection = merge(stage_reflections);
    validateGraphicsReflection(desc, merged_reflection);
    validateShaderResourceInterfaceReflection(
        desc.resource_interface, merged_reflection);

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

PipelineFactory::PipelineRecord PipelineFactory::buildComputePipeline(const ComputePipelineDesc &desc) {
    auto reflection = shader_library.get(desc.shader).reflection;
    validateFrameBindings(reflection);
    validatePushConstantContract(reflection);
    validateShaderResourceInterfaceReflection(
        desc.resource_interface, reflection);
    auto set_layouts = descriptorSetLayoutsFor(reflection);
    auto pipeline_layout = createPipelineLayout(reflection, set_layouts);
    auto pipeline_object = createComputePipeline(desc, pipeline_layout.get());

    return PipelineRecord{
        desc,
        std::move(reflection),
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
    pipeline_handles.reserve(pipeline_handles.size() + 1);
    auto handle = pipelines.reg(buildGraphicsPipeline(desc));
    pipeline_handles.push_back(handle);
    return handle;
}

PipelineHandle PipelineFactory::createCompute(const ComputePipelineDesc &desc) {
    pipeline_handles.reserve(pipeline_handles.size() + 1);
    auto handle = pipelines.reg(buildComputePipeline(desc));
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

vk::DescriptorSetLayout PipelineFactory::frameDescriptorSetLayout() {
    const DescriptorSetLayoutKey key{frameDescriptorSetLayoutBindings()};
    auto found = descriptor_set_layout_cache.find(key);
    if (found == descriptor_set_layout_cache.end()) {
        const auto create_info = makeDescriptorSetLayoutCreateInfo(key.bindings);
        found = descriptor_set_layout_cache.emplace(key, device.createDescriptorSetLayoutUnique(create_info)).first;
    }
    return found->second.get();
}

const ShaderReflection &PipelineFactory::reflection(PipelineHandle handle) const {
    return pipelines.get(handle).reflection;
}

GraphicsPipelineDesc PipelineFactory::graphicsDesc(
    PipelineHandle handle) const {
    const auto &record = pipelines.get(handle);
    const auto *desc =
        std::get_if<GraphicsPipelineDesc>(
            &record.desc);
    if (desc == nullptr) {
        throw std::runtime_error(
            "pipeline is not a graphics pipeline");
    }
    return *desc;
}

PipelineRebuildResult PipelineFactory::rebuildPrepared(
    PreparedShaderReload prepared,
    const std::function<void()> &before_publish,
    std::span<const GraphicsPipelineReloadOverride>
        graphics_overrides) {
    const auto affected_shaders = prepared.affectedBundleIds();
    PipelineRebuildResult result{.dirty_shaders = affected_shaders.size()};
    if (affected_shaders.empty() &&
        graphics_overrides.empty() &&
        !before_publish) {
        return result;
    }
    for (std::size_t left = 0;
         left < graphics_overrides.size(); ++left) {
        for (std::size_t right = left + 1;
             right < graphics_overrides.size();
             ++right) {
            if (graphics_overrides[left].handle ==
                graphics_overrides[right].handle) {
                throw std::runtime_error(
                    "graphics pipeline reload override is "
                    "duplicated");
            }
        }
    }

    struct PreparedPipeline {
        PipelineHandle handle;
        PipelineRecord replacement;
    };
    std::vector<DescriptorSetLayoutKey> original_layout_keys;
    original_layout_keys.reserve(descriptor_set_layout_cache.size());
    for (const auto &[key, layout] : descriptor_set_layout_cache) {
        (void)layout;
        original_layout_keys.push_back(key);
    }
    const auto discard_new_layouts = [&] {
        for (auto it = descriptor_set_layout_cache.begin();
             it != descriptor_set_layout_cache.end();) {
            if (std::find(original_layout_keys.begin(), original_layout_keys.end(), it->first) ==
                original_layout_keys.end()) {
                it = descriptor_set_layout_cache.erase(it);
            } else {
                ++it;
            }
        }
    };

    std::vector<PreparedPipeline> replacements;
    shader_library.activatePrepared(prepared);
    try {
        for (const auto handle : pipeline_handles) {
            const auto &record = pipelines.get(handle);
            const auto override = std::find_if(
                graphics_overrides.begin(),
                graphics_overrides.end(),
                [handle](const auto &candidate) {
                    return candidate.handle == handle;
                });
            if (override ==
                    graphics_overrides.end() &&
                !pipelineUsesShader(
                    record.desc,
                    affected_shaders)) {
                continue;
            }
            if (override !=
                    graphics_overrides.end() &&
                !std::holds_alternative<
                    GraphicsPipelineDesc>(
                    record.desc)) {
                throw std::runtime_error(
                    "graphics pipeline reload override "
                    "references a compute pipeline");
            }
            ++result.attempted_pipelines;
            auto replacement =
                override !=
                        graphics_overrides.end()
                    ? buildGraphicsPipeline(
                          override->desc)
                    : std::visit(
                          [this](
                              const auto
                                  &pipeline_desc) {
                              using Desc =
                                  std::decay_t<
                                      decltype(
                                          pipeline_desc)>;
                              if constexpr (
                                  std::is_same_v<
                                      Desc,
                                      GraphicsPipelineDesc>) {
                                  return buildGraphicsPipeline(
                                      pipeline_desc);
                              } else {
                                  return buildComputePipeline(
                                      pipeline_desc);
                              }
                          },
                          record.desc);
            replacements.push_back({handle, std::move(replacement)});
        }
        for (const auto &override :
             graphics_overrides) {
            if (std::none_of(
                    pipeline_handles.begin(),
                    pipeline_handles.end(),
                    [&](const auto handle) {
                        return handle ==
                               override.handle;
                    })) {
                throw std::runtime_error(
                    "graphics pipeline reload override "
                    "references an unknown pipeline");
            }
        }
    } catch (const std::exception &error) {
        replacements.clear();
        shader_library.activatePrepared(prepared);
        discard_new_layouts();
        result.failed_pipelines = std::max<std::size_t>(1, result.attempted_pipelines);
        result.last_error = error.what();
        LOG_WARNING(logger,
                    "Shader/pipeline transaction failed; keeping the previous generation: {}",
                    error.what());
        return result;
    } catch (...) {
        replacements.clear();
        shader_library.activatePrepared(prepared);
        discard_new_layouts();
        result.failed_pipelines = std::max<std::size_t>(1, result.attempted_pipelines);
        result.last_error = "unknown pipeline candidate failure";
        LOG_WARNING(logger,
                    "Shader/pipeline transaction failed; keeping the previous generation");
        return result;
    }
    // Candidate pipeline creation is complete. Restore the live shader table
    // while the cross-domain material candidate commits.
    shader_library.activatePrepared(prepared);

    try {
        if (before_publish) before_publish();
    } catch (const std::exception &error) {
        replacements.clear();
        discard_new_layouts();
        result.failed_pipelines = 1;
        result.last_error = error.what();
        return result;
    } catch (...) {
        replacements.clear();
        discard_new_layouts();
        result.failed_pipelines = 1;
        result.last_error = "unknown cross-domain shader reload failure";
        return result;
    }

    shader_library.activatePrepared(prepared);
    shader_library.finalizePrepared(prepared);
    for (auto &replacement : replacements) {
        replacePipeline(replacement.handle, std::move(replacement.replacement));
        ++result.rebuilt_pipelines;
    }
    result.committed = true;
    return result;
}

PipelineRebuildResult PipelineFactory::rebuildDirty() {
    const auto dirty_shaders = shader_library.takeDirtyBundles();
    PipelineRebuildResult result{.dirty_shaders = dirty_shaders.size()};
    if (dirty_shaders.empty()) {
        return result;
    }

    for (const auto handle : pipeline_handles) {
        const auto &record = pipelines.get(handle);
        if (!pipelineUsesShader(record.desc, dirty_shaders)) {
            continue;
        }

        ++result.attempted_pipelines;
        try {
            auto replacement = std::visit(
                [this](const auto &pipeline_desc) {
                    using Desc = std::decay_t<decltype(pipeline_desc)>;
                    if constexpr (std::is_same_v<Desc, GraphicsPipelineDesc>) {
                        return buildGraphicsPipeline(pipeline_desc);
                    } else {
                        return buildComputePipeline(pipeline_desc);
                    }
                },
                record.desc);
            replacePipeline(handle, std::move(replacement));
            ++result.rebuilt_pipelines;
        } catch (const std::exception &ex) {
            ++result.failed_pipelines;
            result.last_error = ex.what();
            LOG_WARNING(logger, "Pipeline hot reload failed; keeping previous pipeline: {}", ex.what());
        }
    }
    result.committed = result.failed_pipelines == 0;
    return result;
}

PipelineFactory::RegistrationCheckpoint
PipelineFactory::checkpointRegistrations() const {
    RegistrationCheckpoint checkpoint;
    checkpoint.pipeline_count = pipeline_handles.size();
    checkpoint.descriptor_set_layout_keys.reserve(
        descriptor_set_layout_cache.size());
    for (const auto &[key, layout] :
         descriptor_set_layout_cache) {
        (void)layout;
        checkpoint.descriptor_set_layout_keys.push_back(key);
    }
    return checkpoint;
}

void PipelineFactory::rollbackRegistrations(
    const RegistrationCheckpoint &checkpoint) {
    if (checkpoint.pipeline_count > pipeline_handles.size()) {
        throw std::runtime_error(
            "Pipeline registration checkpoint is invalid");
    }
    while (pipeline_handles.size() >
           checkpoint.pipeline_count) {
        const auto handle = pipeline_handles.back();
        (void)pipelines.extract(handle, false);
        pipeline_handles.pop_back();
    }
    for (auto found = descriptor_set_layout_cache.begin();
         found != descriptor_set_layout_cache.end();) {
        if (std::find(
                checkpoint.descriptor_set_layout_keys.begin(),
                checkpoint.descriptor_set_layout_keys.end(),
                found->first) ==
            checkpoint.descriptor_set_layout_keys.end()) {
            found = descriptor_set_layout_cache.erase(found);
        } else {
            ++found;
        }
    }
}

std::vector<PipelineHandle>
PipelineFactory::registrationsSince(
    const RegistrationCheckpoint &checkpoint) const {
    if (checkpoint.pipeline_count > pipeline_handles.size()) {
        throw std::runtime_error(
            "Pipeline registration checkpoint is invalid");
    }
    return {
        pipeline_handles.begin() +
            static_cast<std::ptrdiff_t>(
                checkpoint.pipeline_count),
        pipeline_handles.end()};
}

void PipelineFactory::retireRegistrations(
    const std::vector<PipelineHandle> &handles) noexcept {
    for (const auto handle : handles) {
        auto retired = pipelines.extract(handle, false);
        std::erase(pipeline_handles, handle);
        if (!retired) continue;
        try {
            auto *queue =
                FastModuleContainer::tryGet<DeletionQueue>();
            if (queue != nullptr &&
                queue->acceptingResources()) {
                queue->defer(std::move(*retired));
            }
        } catch (...) {
        }
    }
}

} // namespace Pelican
