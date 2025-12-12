#include "materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../shader/shadercontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"

namespace Pelican {

constexpr uint32_t modelMatDescriptorSetNumber = 0;
constexpr uint32_t modelMatDescriptorBinding = 0;
constexpr uint32_t modelMatDescriptorArrayCount = 1;
constexpr uint32_t imageDescriptorSetNumber = 1;
constexpr uint32_t baseColorBinding = 0;
constexpr uint32_t metallicRoughnessBinding = 1;
constexpr uint32_t normalBinding = 2;
constexpr uint32_t materialTextureBindingCount = 3;



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
    std::array<vk::DescriptorSetLayoutBinding, materialTextureBindingCount> bindings{};

    bindings[0].binding = baseColorBinding;
    bindings[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = vk::ShaderStageFlagBits::eFragment;

    bindings[1].binding = metallicRoughnessBinding;
    bindings[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = vk::ShaderStageFlagBits::eFragment;

    bindings[2].binding = normalBinding;
    bindings[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = vk::ShaderStageFlagBits::eFragment;

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

    std::array<vk::PipelineColorBlendAttachmentState, 4> blend_attachments{};
    for (auto &blend_attachment : blend_attachments) {
        blend_attachment.blendEnable = false;
        blend_attachment.colorWriteMask = vk::ColorComponentFlagBits::eA | vk::ColorComponentFlagBits::eR |
                                          vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB;
    }

    vk::PipelineColorBlendStateCreateInfo blend;
    blend.logicOpEnable = false;
    blend.setAttachments(blend_attachments);

    auto dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};

    vk::PipelineDynamicStateCreateInfo dynamic_state_info;
    dynamic_state_info.setDynamicStates(dynamic_states);

    const std::array<vk::Format, 4> color_formats = {
        vk::Format::eB8G8R8A8Unorm,      // albedo
        vk::Format::eR16G16B16A16Sfloat, // normal
        vk::Format::eR8G8B8A8Unorm,      // material
        vk::Format::eR16G16B16A16Sfloat, // worldpos
    };

    vk::PipelineRenderingCreateInfo rendering_info;
    rendering_info.setColorAttachmentFormats(color_formats);
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
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
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
    : device{GET_MODULE(VulkanManageCore).getDevice()}, descset_layouts{createDefaultDescriptorSetLayouts(device)},
      pipeline_layout{createDefaultPipelineLayout(GET_MODULE(VulkanManageCore).getDevice(), descset_layouts)},
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)}, desc_pool{createDescriptorPool(device)} {}
MaterialContainer::~MaterialContainer() {}

GlobalTextureId MaterialContainer::registerTexture(vk::Extent3D extent, const void *data) {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    auto image = vkcore.allocImage(extent, vk::Format::eR8G8B8A8Unorm,
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
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

    return textures.reg(InternalTextureResource{
        .image = std::move(image),
        .image_view = std::move(image_view),
    });
}
GlobalMaterialId MaterialContainer::registerMaterial(MaterialInfo info) {
    const uint32_t pipeline_key = (static_cast<uint32_t>(info.vert_shader.value) << 16) |
                                  static_cast<uint32_t>(info.frag_shader.value);
    PipelineId pipeline_id{pipeline_key};
    if (pipelines.find(pipeline_id) == pipelines.end()) {
        pipelines.insert({
            pipeline_id,
            createDefaultPipeline(
                device, pipeline_layout.get(), GET_MODULE(ShaderContainer).getShader(info.vert_shader),
                GET_MODULE(ShaderContainer).getShader(info.frag_shader), VertBufContainer::getDescription()),
        });
    }

    vk::DescriptorSetAllocateInfo desc_alloc_info;
    desc_alloc_info.descriptorPool = desc_pool.get();
    desc_alloc_info.setSetLayouts({descset_layouts[imageDescriptorSetNumber].get()});

    auto descsets = device.allocateDescriptorSetsUnique(desc_alloc_info);
    auto &descset = descsets[0];

    // テクスチャを先に取得してimage_infosを構築
    std::array<vk::DescriptorImageInfo, materialTextureBindingCount> image_infos{};
    
    // Base Color
    {
        const auto &tex = textures.get(info.base_color_texture);
        image_infos[0].imageView = tex.image_view.get();
        image_infos[0].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_infos[0].sampler = linear_sampler.get();
    }
    
    // Metallic Roughness
    {
        const auto &tex = textures.get(info.metallic_roughness_texture);
        image_infos[1].imageView = tex.image_view.get();
        image_infos[1].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_infos[1].sampler = linear_sampler.get();
    }
    
    // Normal
    {
        const auto &tex = textures.get(info.normal_texture);
        image_infos[2].imageView = tex.image_view.get();
        image_infos[2].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_infos[2].sampler = linear_sampler.get();
    }

    std::array<vk::WriteDescriptorSet, materialTextureBindingCount> writes{};
    writes[0].dstSet = descset.get();
    writes[0].dstBinding = baseColorBinding;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &image_infos[0];

    writes[1].dstSet = descset.get();
    writes[1].dstBinding = metallicRoughnessBinding;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &image_infos[1];

    writes[2].dstSet = descset.get();
    writes[2].dstBinding = normalBinding;
    writes[2].dstArrayElement = 0;
    writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &image_infos[2];

    device.updateDescriptorSets(writes, {});

    return materials.reg(InternalMaterialInfo{
        .pipeline = pipeline_id,
        .base_color_texture = info.base_color_texture,
        .metallic_roughness_texture = info.metallic_roughness_texture,
        .normal_texture = info.normal_texture,
        .descset = std::move(descset),
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

bool MaterialContainer::isRenderRequired(PassId pass_id, GlobalMaterialId material) const {
    return true; // TODO
}

void MaterialContainer::bindResource(vk::CommandBuffer cmd_buf, PassId pass_id, GlobalMaterialId material_id,
                                     GlobalMaterialId prev_material_id) const {
    const auto &material = materials.get(material_id);

    if (prev_material_id.value < 0) {
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines.at(material.pipeline).get());
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout.get(), 0,
                                   {
                                       model_mat_buf_descset.get(), // model matrix buffer: set = 0
                                       material.descset.get(),      // material textures: set = 1
                                   },
                                   {});
    } else {
        const auto &prev_material = materials.get(prev_material_id);
        if (material.pipeline != prev_material.pipeline)
            cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines.at(material.pipeline).get());
        if (material.descset.get() != prev_material.descset.get())
            cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout.get(),
                                       imageDescriptorSetNumber, {material.descset.get()}, {});
    }
}

vk::PipelineLayout MaterialContainer::getPipelineLayout() const { return pipeline_layout.get(); }

} // namespace Pelican
