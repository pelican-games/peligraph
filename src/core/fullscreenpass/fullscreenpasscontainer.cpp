#include "fullscreenpasscontainer.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
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

void requireInputBindings(const ShaderReflection &reflection, size_t input_count) {
    for (uint32_t binding = 0; binding < input_count; ++binding) {
        if (!hasInputBinding(reflection, binding)) {
            throw std::runtime_error("Fullscreen pass input texture does not match shader reflection");
        }
    }
}

vk::UniqueDescriptorPool createDescPool(vk::Device device, uint32_t maxSets = 64) {
    vk::DescriptorPoolSize poolSize{};
    poolSize.type = vk::DescriptorType::eCombinedImageSampler;
    poolSize.descriptorCount = maxSets * fullscreenInputBindingCount;

    vk::DescriptorPoolCreateInfo ci{};
    ci.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    ci.maxSets = maxSets;
    ci.poolSizeCount = 1;
    ci.pPoolSizes = &poolSize;
    return device.createDescriptorPoolUnique(ci);
}

vk::UniqueSampler createSampler(vk::Device device, vk::Filter filter) {
    vk::SamplerCreateInfo create_info;
    create_info.magFilter = filter;
    create_info.minFilter = filter;
    create_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    create_info.addressModeU = vk::SamplerAddressMode::eRepeat;
    create_info.addressModeV = vk::SamplerAddressMode::eRepeat;
    create_info.addressModeW = vk::SamplerAddressMode::eRepeat;
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
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)}, desc_pool{createDescPool(device)} {}

FullscreenPassContainer::~FullscreenPassContainer() {}

FullscreenPassContainer::PipelineId
FullscreenPassContainer::registerFullscreenPass(vk::Format colorFormat, ShaderBundleId vertShader,
                                                ShaderBundleId fragShader,
                                                std::vector<std::string> shader_defines) {
    PipelineId pipeline_id = {static_cast<uint32_t>(pipelines.size())};

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline_handle = pipeline_factory.create(GraphicsPipelineDesc{
        vertShader,
        fragShader,
        {colorFormat},
        {},
        std::move(shader_defines),
    });
    pipelines.insert({pipeline_id, pipeline_handle});

    return pipeline_id;
}

void FullscreenPassContainer::bindResource(vk::CommandBuffer cmd_buf, PassId pass_id) {
    const auto pipeline_handle = requirePipelineHandle(pass_id, pipelines);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(pipeline_handle));

    auto it = input_textures.find(pass_id.value);
    if (it != input_textures.end()) {
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_factory.layout(pipeline_handle),
                                   PELICAN_SET_PASS_INPUT, it->second.descset.get(), {});
    }
}

void FullscreenPassContainer::setInputTextures(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                                               const RenderTargetImageViewResolver &rt_views) {
    const auto pipeline_handle = requirePipelineHandle(pass_id, pipelines);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    requireInputBindings(pipeline_factory.reflection(pipeline_handle), input_rts.size());

    if (input_rts.size() > fullscreenInputBindingCount) {
        throw std::runtime_error("Fullscreen pass has too many input textures");
    }
    if (input_rts.empty()) {
        input_textures.erase(pass_id.value);
        return;
    }

    vk::DescriptorSetLayout layout = pipeline_factory.descriptorSetLayout(pipeline_handle, PELICAN_SET_PASS_INPUT);

    vk::DescriptorSetAllocateInfo alloc_info;
    alloc_info.descriptorPool = desc_pool.get();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;

    auto descsets = device.allocateDescriptorSetsUnique(alloc_info);
    auto descset = std::move(descsets[0]);

    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<vk::DescriptorImageInfo> image_infos;
    image_infos.reserve(input_rts.size());

    for (uint32_t i = 0; i < input_rts.size(); ++i) {
        const auto &rt_id = input_rts[i];
        if (!isConcreteRenderTarget(rt_id)) {
            throw std::runtime_error("Fullscreen pass input texture must be a render target");
        }

        vk::DescriptorImageInfo image_info;
        image_info.sampler = linear_sampler.get();
        image_info.imageView = rt_views.getImageView(rt_id);
        image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_infos.push_back(image_info);

        vk::WriteDescriptorSet write;
        write.dstSet = descset.get();
        write.dstBinding = i;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.pImageInfo = &image_infos.back();
        writes.push_back(write);
    }

    device.updateDescriptorSets(writes, {});

    input_textures.insert_or_assign(pass_id.value, InputTextureInfo{
                                                       std::move(descset),
                                                       input_rts,
                                                   });
}

vk::PipelineLayout FullscreenPassContainer::getPipelineLayout(PassId pass_id) const {
    return GET_MODULE(PipelineFactory).layout(requirePipelineHandle(pass_id, pipelines));
}

} // namespace Pelican
