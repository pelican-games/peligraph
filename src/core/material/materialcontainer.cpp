#include "materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"

namespace Pelican {

constexpr uint32_t modelMatDescriptorSetNumber = 0;
constexpr uint32_t modelMatDescriptorBinding = 0;
constexpr uint32_t modelMatDescriptorArrayCount = 1;
constexpr uint32_t imageDescriptorSetNumber = 1;
constexpr uint32_t imageDescriptorBinding = 0;
constexpr uint32_t imageDescriptorArrayCount = 1;

static vk::UniqueDescriptorSetLayout createModelBufDescriptorSetLayout(vk::Device device) {
    std::array<vk::DescriptorSetLayoutBinding, 1> bindings;
    bindings[0].binding = modelMatDescriptorBinding;
    bindings[0].descriptorType = vk::DescriptorType::eStorageBuffer;
    bindings[0].descriptorCount = modelMatDescriptorArrayCount;
    bindings[0].stageFlags = vk::ShaderStageFlagBits::eVertex;

    vk::DescriptorSetLayoutCreateInfo create_info;
    create_info.setBindings(bindings);

    return device.createDescriptorSetLayoutUnique(create_info);
}

static vk::UniqueDescriptorSetLayout createTextureDescriptorSetLayout(vk::Device device) {
    std::array<vk::DescriptorSetLayoutBinding, 1> bindings;
    bindings[0].binding = imageDescriptorBinding;
    bindings[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[0].descriptorCount = imageDescriptorArrayCount;
    bindings[0].stageFlags = vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo create_info;
    create_info.setBindings(bindings);

    return device.createDescriptorSetLayoutUnique(create_info);
}

static std::vector<vk::UniqueDescriptorSetLayout> createDefaultDescriptorSetLayouts(vk::Device device) {
    std::vector<vk::UniqueDescriptorSetLayout> descset_layouts(2);
    descset_layouts[modelMatDescriptorSetNumber] = createModelBufDescriptorSetLayout(device);
    descset_layouts[imageDescriptorSetNumber] = createTextureDescriptorSetLayout(device);
    return descset_layouts;
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
    create_info.setPushConstantRanges({push_constant_range});
    create_info.setSetLayouts(layouts_raw);
    return device.createPipelineLayoutUnique(create_info);
}

static vk::UniquePipeline createDefaultPipeline(vk::Device device, vk::PipelineLayout layout,
                                                vk::ShaderModule vert_shader, vk::ShaderModule frag_shader,
                                                VertBufContainer::CommonVertDataDescription input_descs) {
    vk::PipelineShaderStageCreateInfo vert_stage;
    vert_stage.stage = vk::ShaderStageFlagBits::eVertex;
    vert_stage.module = vert_shader;
    vert_stage.pName = "main";
    vk::PipelineShaderStageCreateInfo frag_stage;
    frag_stage.stage = vk::ShaderStageFlagBits::eFragment;
    frag_stage.module = frag_shader;
    frag_stage.pName = "main";

    const auto stages = {vert_stage, frag_stage};

    vk::PipelineVertexInputStateCreateInfo vertex_input_info;
    vertex_input_info.setVertexAttributeDescriptions(input_descs.attr_descs);
    vertex_input_info.setVertexBindingDescriptions(input_descs.binding_descs);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    input_assembly.primitiveRestartEnable = false;

    vk::PipelineViewportStateCreateInfo viewport; // dynamic state, only count is specified
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterization;
    rasterization.depthClampEnable = false;
    rasterization.rasterizerDiscardEnable = false;
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eBack;
    rasterization.frontFace = vk::FrontFace::eClockwise;
    rasterization.depthBiasEnable = false;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample;
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    multisample.sampleShadingEnable = false;

    vk::PipelineDepthStencilStateCreateInfo depth;
    depth.depthTestEnable = true;
    depth.depthWriteEnable = true;
    depth.depthCompareOp = vk::CompareOp::eLess;
    depth.depthBoundsTestEnable = false;
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

    vk::Format color_format = vk::Format::eB8G8R8A8Unorm;
    vk::PipelineRenderingCreateInfo rendering_info;
    rendering_info.setColorAttachmentFormats(color_format);
    rendering_info.depthAttachmentFormat = vk::Format::eD32Sfloat;

    vk::GraphicsPipelineCreateInfo create_info;
    create_info.setStages(stages);
    create_info.pVertexInputState = &vertex_input_info;
    create_info.pInputAssemblyState = &input_assembly;
    // create_info.pTessellationState =
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

static vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
    vk::DescriptorPoolSize pool_size[2];
    pool_size[0].type = vk::DescriptorType::eStorageBuffer;
    pool_size[0].descriptorCount = 1024;
    pool_size[1].type = vk::DescriptorType::eCombinedImageSampler;
    pool_size[1].descriptorCount = 1024;

    vk::DescriptorPoolCreateInfo create_info;
    create_info.maxSets = 1024;
    create_info.setPoolSizes(pool_size);
    return device.createDescriptorPoolUnique(create_info);
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

static vk::UniqueShaderModule createShaderModule(vk::Device device, size_t len, const char *data) {
    vk::ShaderModuleCreateInfo create_info;
    create_info.codeSize = len;
    create_info.pCode = reinterpret_cast<const uint32_t *>(data);

    return device.createShaderModuleUnique(create_info);
}

static vk::UniqueImageView createImageView(vk::Device device, const ImageWrapper &image) {
    vk::ImageViewCreateInfo create_info;
    create_info.image = image.image.get();
    create_info.viewType = vk::ImageViewType::e2D;
    create_info.format = image.format;
    create_info.components.r = vk::ComponentSwizzle::eR;
    create_info.components.g = vk::ComponentSwizzle::eG;
    create_info.components.b = vk::ComponentSwizzle::eB;
    create_info.components.a = vk::ComponentSwizzle::eA;
    create_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    create_info.subresourceRange.baseMipLevel = 0;
    create_info.subresourceRange.levelCount = 1;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount = 1;

    return device.createImageViewUnique(create_info);
}

MaterialContainer::MaterialContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      descset_layouts{createDefaultDescriptorSetLayouts(device)},
      pipeline_layout{createDefaultPipelineLayout(GET_MODULE(VulkanManageCore).getDevice(), descset_layouts)},
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)}, desc_pool{createDescriptorPool(device)} {}
MaterialContainer::~MaterialContainer() {}

GlobalShaderId MaterialContainer::registerShader(size_t len, const char *data) {
    return shaders.reg(createShaderModule(device, len, data));
}
GlobalTextureId MaterialContainer::registerTexture(vk::Extent3D extent, const void *data) {
    vk::DescriptorSetAllocateInfo desc_alloc_info;
    desc_alloc_info.descriptorPool = desc_pool.get();
    desc_alloc_info.setSetLayouts({descset_layouts[imageDescriptorSetNumber].get()});

    auto descsets = device.allocateDescriptorSetsUnique(desc_alloc_info);
    auto &descset = descsets[0];

    const auto &vkcore = GET_MODULE(VulkanManageCore);
    auto image = vkcore.allocImage(
        extent, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferDevice, {});

    auto &vkutil = GET_MODULE(VulkanUtils);
    vkutil.safeTransferMemoryToImage(image, data, extent.width * extent.height * extent.depth * 4,
                                     VulkanUtils::ImageTransferInfo{
                                         .old_layout = vk::ImageLayout::eUndefined,
                                         .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                         .dst_stage = vk::PipelineStageFlagBits::eFragmentShader,
                                         .dst_access = vk::AccessFlagBits::eShaderRead,
                                     });

    auto image_view = createImageView(device, image);

    vk::DescriptorImageInfo image_info;
    image_info.imageView = image_view.get();
    image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    image_info.sampler = linear_sampler.get();

    vk::WriteDescriptorSet write_descset;
    write_descset.dstSet = descset.get();
    write_descset.dstBinding = imageDescriptorBinding;
    write_descset.dstArrayElement = 0;
    write_descset.setImageInfo({image_info});
    write_descset.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    device.updateDescriptorSets({write_descset}, {});

    return textures.reg(InternalTextureResource{
        .image = std::move(image),
        .image_view = std::move(image_view),
        .descset = std::move(descset),
    });
}
GlobalMaterialId MaterialContainer::registerMaterial(MaterialInfo info) {
    PipelineId pipeline_id = {info.vert_shader.value << 16 | info.frag_shader.value};
    if (pipelines.find(pipeline_id) == pipelines.end()) {
        pipelines.insert({
            pipeline_id,
            createDefaultPipeline(device, pipeline_layout.get(), shaders.get(info.vert_shader).get(),
                                  shaders.get(info.frag_shader).get(), VertBufContainer::getDescription()),
        });
    }

    return materials.reg(InternalMaterialInfo{
        .pipeline = pipeline_id,
        .base_color_texture = info.base_color_texture,
    });
}

void MaterialContainer::setModelMatBuf(const BufferWrapper &buf) {
    vk::DescriptorSetAllocateInfo desc_alloc_info;
    desc_alloc_info.descriptorPool = desc_pool.get();
    desc_alloc_info.setSetLayouts({descset_layouts[modelMatDescriptorSetNumber].get()});

    auto descsets = device.allocateDescriptorSetsUnique(desc_alloc_info);
    auto &descset = descsets[0];

    vk::DescriptorBufferInfo buf_info;
    buf_info.buffer = buf.buffer.get();
    buf_info.offset = 0;
    buf_info.range = vk::WholeSize;

    vk::WriteDescriptorSet write_descset;
    write_descset.dstSet = descset.get();
    write_descset.dstBinding = modelMatDescriptorBinding;
    write_descset.dstArrayElement = 0;
    write_descset.setBufferInfo({buf_info});
    write_descset.descriptorType = vk::DescriptorType::eStorageBuffer;
    device.updateDescriptorSets({write_descset}, {});

    model_mat_buf_descset = std::move(descset);
}

void MaterialContainer::bindResource(vk::CommandBuffer cmd_buf, GlobalMaterialId material_id,
                                     GlobalMaterialId prev_material_id) const {
    const auto &material = materials.get(material_id);
    const auto &texture = textures.get(material.base_color_texture);

    if (prev_material_id.value < 0) {
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines.at(material.pipeline).get());
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout.get(), 0,
                                   {
                                       model_mat_buf_descset.get(), // model matrix buffer: set = 0
                                       texture.descset.get(),       // texture: set = 1
                                   },
                                   {});
    } else {
        const auto prev_material = materials.get(prev_material_id);
        if (material.pipeline != prev_material.pipeline)
            cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines.at(material.pipeline).get());
        if (material.base_color_texture.value != prev_material.base_color_texture.value)
            cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout.get(),
                                       imageDescriptorSetNumber,
                                       {
                                           texture.descset.get(), // texture: set = 1
                                       },
                                       {});
    }
}

vk::PipelineLayout MaterialContainer::getPipelineLayout() const { return pipeline_layout.get(); }

} // namespace Pelican
