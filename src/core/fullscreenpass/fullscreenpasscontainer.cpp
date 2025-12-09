#include "fullscreenpasscontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include "../material/materialcontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"

namespace Pelican {

static vk::UniqueDescriptorSetLayout createInputDescSetLayout(vk::Device device, uint32_t maxInputs = 8) {
    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    binding.descriptorCount = 1;
    binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
    
    vk::DescriptorSetLayoutCreateInfo ci{};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    return device.createDescriptorSetLayoutUnique(ci);
}

static vk::UniquePipelineLayout createDefaultPipelineLayout(vk::Device device,
                                                            std::span<vk::UniqueDescriptorSetLayout> layouts) {
    vk::PushConstantRange push_constant_range;
    push_constant_range.stageFlags = vk::ShaderStageFlagBits::eVertex;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(PushConstantStruct);

    std::vector<vk::DescriptorSetLayout> layouts_raw(layouts.size());
    for (int i = 0; i < layouts.size(); i++)
        layouts_raw[i] = layouts[i].get();

    vk::PipelineLayoutCreateInfo create_info;
    //create_info.setPushConstantRanges({push_constant_range});
    create_info.setSetLayouts(layouts_raw);
    return device.createPipelineLayoutUnique(create_info);
}

static vk::UniquePipeline createFullscreenPipeline(vk::Device device, vk::PipelineLayout layout,
                                                   vk::Format colorFormat, vk::ShaderModule vertShader,
                                                   vk::ShaderModule fragShader) {
    vk::PipelineShaderStageCreateInfo vert_stage;
    vert_stage.stage = vk::ShaderStageFlagBits::eVertex;
    vert_stage.module = vertShader;
    vert_stage.pName = "main";
    
    vk::PipelineShaderStageCreateInfo frag_stage;
    frag_stage.stage = vk::ShaderStageFlagBits::eFragment;
    frag_stage.module = fragShader;
    frag_stage.pName = "main";

    const auto stages = {vert_stage, frag_stage};

    // フルスクリーンクワッドは頂点バッファ不要
    vk::PipelineVertexInputStateCreateInfo vertex_input_info;

    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
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

    vk::PipelineColorBlendAttachmentState blend_attachment;
    blend_attachment.blendEnable = false;
    blend_attachment.colorWriteMask = vk::ColorComponentFlagBits::eA | vk::ColorComponentFlagBits::eR |
                                      vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB;

    vk::PipelineColorBlendStateCreateInfo blend;
    blend.logicOpEnable = false;
    blend.setAttachments({blend_attachment});

    auto dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};

    vk::PipelineDynamicStateCreateInfo dynamic_state_info;
    dynamic_state_info.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info;
    rendering_info.setColorAttachmentFormats(colorFormat);

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

    auto result = device.createGraphicsPipelineUnique({}, create_info_chain.get());
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed on vkCreateGraphicsPipeline : " + vk::to_string(result.result));
    }
    return std::move(result.value);
}

static vk::UniqueDescriptorPool createDescPool(vk::Device device, uint32_t maxSets = 64) {
    vk::DescriptorPoolSize poolSize{};
    poolSize.type = vk::DescriptorType::eCombinedImageSampler;
    poolSize.descriptorCount = maxSets * 8;  // 1セットあたり最大8入力想定
    
    vk::DescriptorPoolCreateInfo ci{};
    ci.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    ci.maxSets = maxSets;
    ci.poolSizeCount = 1;
    ci.pPoolSizes = &poolSize;
    return device.createDescriptorPoolUnique(ci);
}

static vk::UniqueSampler createSampler(vk::Device device, vk::Filter filter) {
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

FullscreenPassContainer::FullscreenPassContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)},
      desc_pool{createDescPool(device)} {
    
    descset_layouts.push_back(createInputDescSetLayout(device));
    pipeline_layout = createDefaultPipelineLayout(device, descset_layouts);
}

FullscreenPassContainer::~FullscreenPassContainer() {}

FullscreenPassContainer::PipelineId FullscreenPassContainer::registerFullscreenPass(vk::Format colorFormat, vk::ShaderModule vertShader,
                                                           vk::ShaderModule fragShader) {
    PipelineId pipeline_id = {static_cast<uint32_t>(pipelines.size())};
    
    pipelines.insert({
        pipeline_id,
        createFullscreenPipeline(device, pipeline_layout.get(), colorFormat, vertShader, fragShader)
    });
    
    return pipeline_id;
}

void FullscreenPassContainer::bindResource(vk::CommandBuffer cmd_buf, PassId pass_id) {
    // パイプラインとディスクリプタセットをバインド
    auto pipeline_id = PipelineId{static_cast<uint32_t>(pass_id.value)};  // PassIdからPipelineIdへの変換が必要な場合
    
    if (pipelines.find(pipeline_id) != pipelines.end()) {
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines.at(pipeline_id).get());
        
        if (input_attachments_descset) {
            cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, 
                                      pipeline_layout.get(), 
                                      0,
                                      {input_attachments_descset.get()}, 
                                      {});
        }
    }
}

void FullscreenPassContainer::setInputTexture(GlobalRenderTargetId input_rt) {
    auto& rt_container = GET_MODULE(RenderTargetContainer);
    const auto& rt = rt_container.get(input_rt);
    
    // ディスクリプタセット割り当て
    vk::DescriptorSetAllocateInfo alloc_info;
    alloc_info.descriptorPool = desc_pool.get();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descset_layouts[0].get();
    
    auto desc_sets = device.allocateDescriptorSetsUnique(alloc_info);
    input_attachments_descset = std::move(desc_sets[0]);
    
    // ディスクリプタ更新
    vk::DescriptorImageInfo image_info;
    image_info.sampler = linear_sampler.get();
    image_info.imageView = rt.image_view.get();
    image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    
    vk::WriteDescriptorSet write;
    write.dstSet = input_attachments_descset.get();
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.pImageInfo = &image_info;
    
    device.updateDescriptorSets({write}, {});
}

} // namespace Pelican
