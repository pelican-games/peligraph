#include "fullscreenpasscontainer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Pelican {

namespace {

constexpr uint32_t fullscreenInputBindingCount = 8;

FullscreenPassContainer::PipelineId requireFullscreenPipelineId(PassId pass_id) {
    if (pass_id.value < 0) {
        throw std::runtime_error("Fullscreen pass id is invalid");
    }
    return FullscreenPassContainer::PipelineId{static_cast<uint32_t>(pass_id.value)};
}

PipelineHandle requirePipelineHandle(
    PassId pass_id,
    const std::unordered_map<FullscreenPassContainer::PipelineId, PipelineHandle,
                             FullscreenPassContainer::PipelineId::Hash> &pipelines) {
    const auto pipeline_id = requireFullscreenPipelineId(pass_id);
    auto found = pipelines.find(pipeline_id);
    if (found == pipelines.end()) {
        throw std::runtime_error("Fullscreen pipeline not found");
    }
    return found->second;
}

bool hasInputBinding(const ShaderReflection &reflection, uint32_t binding) {
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set == PELICAN_SET_PASS_INPUT && reflected.binding == binding &&
            reflected.type == vk::DescriptorType::eCombinedImageSampler) {
            return true;
        }
    }
    return false;
}

bool hasStorageBufferBinding(const ShaderReflection &reflection, uint32_t binding) {
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set == PELICAN_SET_PASS_INPUT && reflected.binding == binding &&
            reflected.type == vk::DescriptorType::eStorageBuffer) {
            return true;
        }
    }
    return false;
}

void requireInputBindings(const ShaderReflection &reflection, size_t texture_count, size_t buffer_count) {
    const auto input_count = texture_count + buffer_count;
    for (uint32_t binding = 0; binding < input_count; ++binding) {
        if (binding < texture_count) {
            if (!hasInputBinding(reflection, binding)) {
                throw std::runtime_error("Fullscreen pass input texture does not match shader reflection");
            }
        } else if (!hasStorageBufferBinding(reflection, binding)) {
            throw std::runtime_error("Fullscreen pass input buffer does not match shader reflection");
        }
    }
}

vk::UniqueDescriptorPool createDescPool(vk::Device device,
                                        uint32_t maxSets = 1024) {
    std::array<vk::DescriptorPoolSize, 2> pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               maxSets * fullscreenInputBindingCount},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer,
                               maxSets * fullscreenInputBindingCount},
    };

    vk::DescriptorPoolCreateInfo ci{};
    ci.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    ci.maxSets = maxSets;
    ci.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(ci);
}

vk::SamplerAddressMode samplerAddressMode(
    FullscreenInputAddressMode mode) {
    switch (mode) {
    case FullscreenInputAddressMode::repeat:
        return vk::SamplerAddressMode::eRepeat;
    case FullscreenInputAddressMode::mirrored_repeat:
        return vk::SamplerAddressMode::eMirroredRepeat;
    case FullscreenInputAddressMode::clamp_to_edge:
        return vk::SamplerAddressMode::eClampToEdge;
    }
    throw std::runtime_error(
        "Unknown fullscreen sampler address mode");
}

std::size_t samplerIndex(FullscreenInputSampling sampling) {
    constexpr std::size_t address_mode_count = 3;
    return static_cast<std::size_t>(sampling.filter) *
               address_mode_count +
           static_cast<std::size_t>(sampling.address_mode);
}

vk::UniqueSampler createSampler(
    vk::Device device, FullscreenInputSampling sampling) {
    const auto filter =
        sampling.filter == FullscreenInputFilter::nearest
            ? vk::Filter::eNearest
            : vk::Filter::eLinear;
    const auto address_mode =
        samplerAddressMode(sampling.address_mode);
    vk::SamplerCreateInfo create_info;
    create_info.magFilter = filter;
    create_info.minFilter = filter;
    create_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    create_info.addressModeU = address_mode;
    create_info.addressModeV = address_mode;
    create_info.addressModeW = address_mode;
    create_info.mipLodBias = 0.0f;
    create_info.anisotropyEnable = false;
    create_info.maxAnisotropy = 1.0f;
    create_info.compareEnable = false;
    create_info.compareOp = vk::CompareOp::eAlways;
    create_info.minLod = 0.0f;
    create_info.maxLod = 0.0f;
    create_info.borderColor = vk::BorderColor::eIntOpaqueBlack;
    create_info.unnormalizedCoordinates = false;
    return device.createSamplerUnique(create_info);
}

} // namespace

FullscreenPassContainer::FullscreenPassContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      desc_pool{createDescPool(device)} {
    for (const auto filter :
         {FullscreenInputFilter::linear,
          FullscreenInputFilter::nearest}) {
        for (const auto address_mode :
             {FullscreenInputAddressMode::repeat,
              FullscreenInputAddressMode::mirrored_repeat,
              FullscreenInputAddressMode::clamp_to_edge}) {
            const FullscreenInputSampling sampling{
                filter, address_mode};
            input_samplers.at(samplerIndex(sampling)) =
                createSampler(device, sampling);
        }
    }
}

FullscreenPassContainer::~FullscreenPassContainer() {}

FullscreenPassContainer::PipelineId
FullscreenPassContainer::registerFullscreenPass(vk::Format colorFormat, ShaderBundleId vertShader,
                                                ShaderBundleId fragShader,
                                                std::vector<std::string> shader_defines,
                                                vk::SampleCountFlagBits samples,
                                                GraphicsPipelineViewContract view) {
    registration_order.reserve(registration_order.size() + 1);
    if (next_pipeline_id >
        static_cast<uint32_t>(
            std::numeric_limits<int>::max())) {
        throw std::runtime_error(
            "Fullscreen pipeline handle table is exhausted");
    }
    PipelineId pipeline_id = {next_pipeline_id++};

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    auto desc = GraphicsPipelineDesc{
        vertShader,
        fragShader,
        {colorFormat},
        {},
        std::move(shader_defines),
    };
    desc.rasterization_samples = samples;
    desc.view = view;
    const auto pipeline_handle = pipeline_factory.create(desc);
    if (!pipelines.insert({pipeline_id, pipeline_handle}).second) {
        throw std::runtime_error(
            "Fullscreen pipeline handle table changed during registration");
    }
    registration_order.push_back(pipeline_id);

    return pipeline_id;
}

void FullscreenPassContainer::bindResource(
    vk::CommandBuffer cmd_buf, PassId pass_id,
    RenderPassViewInvocation invocation) {
    const auto pipeline_handle = requirePipelineHandle(pass_id, pipelines);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(pipeline_handle));

    auto it = input_textures.find(pass_id.value);
    if (it != input_textures.end()) {
        const auto parity = GET_MODULE(RenderTargetContainer).historyFrameIndex();
        const auto &info = it->second;
        std::size_t variant_index = 0;
        if (info.view.execution ==
            GraphicsPipelineViewExecution::multiview) {
            if (invocation.view_index != 0 ||
                invocation.logical_view_count !=
                    info.view.view_count) {
                throw std::runtime_error(
                    "fullscreen multiview descriptor invocation does not "
                    "match its pipeline");
            }
        } else if (info.variants.size() > 1) {
            if (invocation.logical_view_count !=
                    info.variants.size() ||
                invocation.view_index >=
                info.variants.size()) {
                throw std::runtime_error(
                    "fullscreen sequential descriptor invocation does not "
                    "match its layered inputs");
            }
            variant_index = invocation.view_index;
        }
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_factory.layout(pipeline_handle),
                                   PELICAN_SET_PASS_INPUT,
                                   info.variants.at(variant_index)
                                       .descsets[parity]
                                       .get(),
                                   {});
    }
}

void FullscreenPassContainer::setInputTextures(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                                               const RenderTargetImageViewResolver &rt_views) {
    setInputResources(pass_id, input_rts, std::vector<bool>(input_rts.size(), false), {}, rt_views,
                      GET_MODULE(FrameGraphResourceContainer));
}

void FullscreenPassContainer::setInputResources(PassId pass_id,
                                                const std::vector<GlobalRenderTargetId> &input_rts,
                                                const std::vector<bool> &input_rt_history,
                                                const std::vector<std::string> &input_buffers,
                                                const RenderTargetImageViewResolver &rt_views,
                                                const FrameGraphResourceContainer &frame_graph_resources,
                                                const std::vector<FullscreenInputSampling> &input_sampling,
                                                const std::vector<PassInputViewDimension> &input_views,
                                                GraphicsPipelineViewContract view) {
    std::vector<FrameGraphBufferId> buffer_ids;
    buffer_ids.reserve(input_buffers.size());
    for (const auto &name : input_buffers) {
        const auto id =
            frame_graph_resources.getBufferIdByName(name);
        if (!isValidFrameGraphBufferId(id)) {
            throw std::runtime_error(
                "Fullscreen pass input buffer not found: " +
                name);
        }
        buffer_ids.push_back(id);
    }
    setInputResourcesById(
        pass_id, input_rts, input_rt_history, buffer_ids,
        rt_views, frame_graph_resources, input_sampling,
        input_views, view);
}

void FullscreenPassContainer::setInputResourcesById(
    PassId pass_id,
    const std::vector<GlobalRenderTargetId> &input_rts,
    const std::vector<bool> &input_rt_history,
    const std::vector<FrameGraphBufferId> &input_buffers,
    const RenderTargetImageViewResolver &rt_views,
    const FrameGraphResourceContainer &frame_graph_resources,
    const std::vector<FullscreenInputSampling> &input_sampling,
    const std::vector<PassInputViewDimension> &input_views,
    GraphicsPipelineViewContract view) {
    const auto pipeline_handle = requirePipelineHandle(pass_id, pipelines);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    requireInputBindings(pipeline_factory.reflection(pipeline_handle), input_rts.size(), input_buffers.size());
    if (input_rt_history.size() != input_rts.size()) {
        throw std::runtime_error("Fullscreen pass input history metadata is inconsistent");
    }
    if (!input_sampling.empty() &&
        input_sampling.size() != input_rts.size()) {
        throw std::runtime_error(
            "Fullscreen pass input sampling metadata is inconsistent");
    }
    if (!input_views.empty() &&
        input_views.size() != input_rts.size()) {
        throw std::runtime_error(
            "Fullscreen pass input view metadata is inconsistent");
    }
    if (view.execution ==
            GraphicsPipelineViewExecution::multiview &&
        std::find(input_views.begin(), input_views.end(),
                  PassInputViewDimension::sequential_2d) !=
            input_views.end()) {
        throw std::runtime_error(
            "Fullscreen multiview pass cannot bind a sequential-only input");
    }

    const auto input_count = input_rts.size() + input_buffers.size();
    if (input_count > fullscreenInputBindingCount) {
        throw std::runtime_error("Fullscreen pass has too many inputs");
    }
    if (input_count == 0) {
        input_textures.erase(pass_id.value);
        return;
    }

    vk::DescriptorSetLayout layout = pipeline_factory.descriptorSetLayout(pipeline_handle, PELICAN_SET_PASS_INPUT);

    InputTextureInfo info;
    info.input_rt_ids = input_rts;
    info.input_rt_history = input_rt_history;
    info.input_rt_views =
        input_views.empty()
            ? std::vector<PassInputViewDimension>(
                  input_rts.size(),
                  PassInputViewDimension::shared_2d)
            : input_views;
    info.input_sampling =
        input_sampling.empty()
            ? std::vector<FullscreenInputSampling>(
                  input_rts.size())
            : input_sampling;
    info.input_buffer_ids = input_buffers;
    info.view = view;
    info.binding_revision = next_binding_revision++;
    if (std::find_if(
            input_rts.begin(), input_rts.end(),
            [](GlobalRenderTargetId target) {
                return !isConcreteRenderTarget(target);
            }) != input_rts.end()) {
        throw std::runtime_error(
            "Fullscreen pass input texture must be a render target");
    }
    std::uint32_t variant_count = 1;
    if (view.execution ==
        GraphicsPipelineViewExecution::single_view) {
        for (std::size_t input = 0;
             input < input_rts.size(); ++input) {
            if (info.input_rt_views[input] !=
                PassInputViewDimension::shared_2d) {
                variant_count = std::max(
                    variant_count,
                    rt_views.arrayLayers(
                        input_rts[input]));
            }
        }
    }
    for (std::size_t input = 0;
         input < input_rts.size(); ++input) {
        const auto layers =
            rt_views.arrayLayers(input_rts[input]);
        if (view.execution ==
                GraphicsPipelineViewExecution::multiview &&
            info.input_rt_views[input] ==
                PassInputViewDimension::layered_2d_array &&
            layers < view.view_count) {
            throw std::runtime_error(
                "Fullscreen multiview input has too few array layers");
        }
        if (view.execution ==
                GraphicsPipelineViewExecution::single_view &&
            info.input_rt_views[input] !=
                PassInputViewDimension::shared_2d &&
            layers != variant_count) {
            throw std::runtime_error(
                "Fullscreen sequential inputs have inconsistent "
                "array-layer counts");
        }
    }
    info.variants.resize(variant_count);
    for (std::uint32_t variant = 0;
         variant < variant_count; ++variant) {
        for (uint32_t parity = 0; parity < 2; ++parity) {
            auto &descriptor_variant =
                info.variants[variant];
            vk::DescriptorSetAllocateInfo alloc_info;
            alloc_info.descriptorPool = desc_pool.get();
            alloc_info.descriptorSetCount = 1;
            alloc_info.pSetLayouts = &layout;
            descriptor_variant.descsets[parity] =
                std::move(device.allocateDescriptorSetsUnique(alloc_info).front());

            std::vector<vk::WriteDescriptorSet> writes;
            std::vector<vk::DescriptorImageInfo> image_infos;
            std::vector<vk::DescriptorBufferInfo> buffer_infos;
            image_infos.reserve(input_rts.size());
            buffer_infos.reserve(input_buffers.size());
            descriptor_variant.bound_image_views[parity]
                .reserve(input_rts.size());
            for (uint32_t i = 0; i < input_rts.size(); ++i) {
                if (!isConcreteRenderTarget(input_rts[i])) {
                    throw std::runtime_error("Fullscreen pass input texture must be a render target");
                }
                const auto dimension =
                    info.input_rt_views[i];
                vk::ImageView image_view;
                if (view.execution ==
                        GraphicsPipelineViewExecution::multiview &&
                    dimension ==
                        PassInputViewDimension::layered_2d_array) {
                    image_view =
                        rt_views.getLayeredImageViewForFrame(
                            input_rts[i],
                            input_rt_history[i], parity);
                } else if (
                    view.execution ==
                        GraphicsPipelineViewExecution::single_view &&
                    dimension !=
                        PassInputViewDimension::shared_2d) {
                    image_view =
                        rt_views.getImageLayerViewForFrame(
                            input_rts[i], variant,
                            input_rt_history[i], parity);
                } else {
                    image_view =
                        rt_views.getImageViewForFrame(
                            input_rts[i],
                            input_rt_history[i], parity);
                }
                image_infos.push_back(vk::DescriptorImageInfo{
                    input_samplers
                        .at(samplerIndex(info.input_sampling[i]))
                        .get(),
                    image_view,
                    vk::ImageLayout::eShaderReadOnlyOptimal});
                descriptor_variant.bound_image_views[parity]
                    .push_back(image_infos.back().imageView);
                vk::WriteDescriptorSet write{
                    descriptor_variant.descsets[parity].get(), i, 0, 1,
                    vk::DescriptorType::eCombinedImageSampler};
                write.pImageInfo = &image_infos.back();
                writes.push_back(write);
            }
            for (uint32_t i = 0; i < input_buffers.size(); ++i) {
                const auto binding = static_cast<uint32_t>(input_rts.size()) + i;
                buffer_infos.push_back(frame_graph_resources.descriptorInfo(input_buffers[i]));
                vk::WriteDescriptorSet write{
                    descriptor_variant.descsets[parity].get(), binding, 0, 1,
                    vk::DescriptorType::eStorageBuffer};
                write.pBufferInfo = &buffer_infos.back();
                writes.push_back(write);
            }
            device.updateDescriptorSets(writes, {});
        }
    }
    input_textures.insert_or_assign(pass_id.value, std::move(info));
}

void FullscreenPassContainer::rebindInputResources(
    PassId pass_id,
    const RenderTargetImageViewResolver &rt_views,
    const FrameGraphResourceContainer &frame_graph_resources) {
    const auto found = input_textures.find(pass_id.value);
    if (found == input_textures.end()) return;
    const auto input_rts = found->second.input_rt_ids;
    const auto input_history =
        found->second.input_rt_history;
    const auto input_buffers =
        found->second.input_buffer_ids;
    const auto input_sampling =
        found->second.input_sampling;
    const auto input_views =
        found->second.input_rt_views;
    const auto view = found->second.view;
    setInputResourcesById(
        pass_id, input_rts, input_history, input_buffers,
        rt_views, frame_graph_resources, input_sampling,
        input_views, view);
}

std::vector<vk::ImageView>
FullscreenPassContainer::boundInputImageViewsForTesting(
    PassId pass_id,
    std::uint32_t view_index) const {
    const auto found = input_textures.find(pass_id.value);
    if (found == input_textures.end()) {
        return {};
    }
    return found->second.variants.at(view_index).bound_image_views[
        GET_MODULE(RenderTargetContainer).historyFrameIndex()];
}

uint64_t FullscreenPassContainer::inputBindingRevisionForTesting(PassId pass_id) const {
    const auto found = input_textures.find(pass_id.value);
    return found == input_textures.end() ? 0 : found->second.binding_revision;
}

vk::PipelineLayout FullscreenPassContainer::getPipelineLayout(PassId pass_id) const {
    return GET_MODULE(PipelineFactory).layout(requirePipelineHandle(pass_id, pipelines));
}

std::vector<FullscreenInputSampling>
FullscreenPassContainer::inputSamplingForTesting(
    PassId pass_id) const {
    const auto found = input_textures.find(pass_id.value);
    return found == input_textures.end()
               ? std::vector<FullscreenInputSampling>{}
               : found->second.input_sampling;
}

FullscreenPassContainer::RegistrationCheckpoint
FullscreenPassContainer::checkpointRegistrations() const noexcept {
    return RegistrationCheckpoint{
        registration_order.size(), next_binding_revision,
        next_pipeline_id};
}

void FullscreenPassContainer::rollbackRegistrations(
    RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Fullscreen pass registration checkpoint is invalid");
    }
    while (registration_order.size() >
           checkpoint.registration_count) {
        const auto id = registration_order.back();
        input_textures.erase(static_cast<int>(id.value));
        pipelines.erase(id);
        registration_order.pop_back();
    }
    next_binding_revision =
        checkpoint.next_binding_revision;
    next_pipeline_id = checkpoint.next_pipeline_id;
}

std::vector<FullscreenPassContainer::PipelineId>
FullscreenPassContainer::registrationsSince(
    RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Fullscreen pass registration checkpoint is invalid");
    }
    return {
        registration_order.begin() +
            static_cast<std::ptrdiff_t>(
                checkpoint.registration_count),
        registration_order.end()};
}

void FullscreenPassContainer::retireRegistrations(
    const std::vector<PipelineId> &ids) noexcept {
    for (const auto id : ids) {
        const auto textures =
            input_textures.find(static_cast<int>(id.value));
        if (textures != input_textures.end()) {
            auto retired = std::move(textures->second);
            input_textures.erase(textures);
            try {
                auto *queue =
                    FastModuleContainer::tryGet<DeletionQueue>();
                if (queue != nullptr &&
                    queue->acceptingResources()) {
                    queue->defer(std::move(retired));
                }
            } catch (...) {
            }
        }
        pipelines.erase(id);
        std::erase(registration_order, id);
    }
}

} // namespace Pelican
