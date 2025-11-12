#include "materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "battery/embed.hpp"

namespace Pelican {

constexpr uint32_t imageDescriptorBinding = 0;
constexpr uint32_t imageDescriptorArrayCount = 1;

static std::vector<vk::UniqueDescriptorSetLayout> createDefaultDescriptorSetLayout(vk::Device device) {
    std::array<vk::DescriptorSetLayoutBinding, 1> bindings;
    bindings[0].binding = imageDescriptorBinding;
    bindings[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[0].descriptorCount = imageDescriptorArrayCount;
    bindings[0].stageFlags = vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo create_info;
    create_info.setBindings(bindings);

    std::vector<vk::UniqueDescriptorSetLayout> descset_layouts(1);
    descset_layouts[0] = device.createDescriptorSetLayoutUnique(create_info);
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
    // rendering_info.depthAttachmentFormat = vk::Format::eD32Sfloat;

    vk::GraphicsPipelineCreateInfo create_info;
    create_info.setStages(stages);
    create_info.pVertexInputState = &vertex_input_info;
    create_info.pInputAssemblyState = &input_assembly;
    // create_info.pTessellationState =
    create_info.pViewportState = &viewport;
    create_info.pRasterizationState = &rasterization;
    create_info.pMultisampleState = &multisample;
    // create_info.pDepthStencilState = ;
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
    vk::DescriptorPoolSize pool_size;
    pool_size.type = vk::DescriptorType::eCombinedImageSampler;
    pool_size.descriptorCount = 1024;

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

MaterialContainer::MaterialContainer(DependencyContainer &_con)
    : con{_con}, device{con.get<VulkanManageCore>().getDevice()},
      descset_layouts{createDefaultDescriptorSetLayout(device)},
      pipeline_layout{createDefaultPipelineLayout(con.get<VulkanManageCore>().getDevice(), descset_layouts)},
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)}, desc_pool{createDescriptorPool(device)} {
    const auto vert_shader = b::embed<"default.vert.spv">();
    shaders.insert({GlobalShaderId{0}, createShaderModule(device, vert_shader.length(), vert_shader.data())});

    const auto frag_shader = b::embed<"default.frag.spv">();
    shaders.insert({GlobalShaderId{1}, createShaderModule(device, frag_shader.length(), frag_shader.data())});
}
MaterialContainer::~MaterialContainer() {}

GlobalShaderId MaterialContainer::registerShader() {
    return {}; // TODO
}
GlobalTextureId MaterialContainer::registerTexture(vk::Extent3D extent, const void *data) {
    vk::DescriptorSetAllocateInfo desc_alloc_info;
    desc_alloc_info.descriptorPool = desc_pool.get();
    desc_alloc_info.setSetLayouts({descset_layouts[0].get()});

    auto descsets = device.allocateDescriptorSetsUnique(desc_alloc_info);
    auto &descset = descsets[0];

    const auto &vkcore = con.get<VulkanManageCore>();
    auto image =
        vkcore.allocImage(extent, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled,
                          vma::MemoryUsage::eAutoPreferHost, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    vkcore.writeImage(image, data, extent.width * extent.height * extent.depth * 4);
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

    GlobalTextureId id{textures.size() + 1}; // TODO
    textures.insert({
        id,
        InternalTextureResource{
            .image = std::move(image),
            .image_view = std::move(image_view),
            .descset = std::move(descset),
        },
    });
    return id;
}
GlobalMaterialId MaterialContainer::registerMaterial(MaterialInfo info) {
    const auto id = GlobalMaterialId{static_cast<int>(materials.size())}; // TODO
    materials.insert({
        id,
        InternalMaterialInfo{
            .pipeline = createDefaultPipeline(device, pipeline_layout.get(), shaders.at(info.vert_shader).get(),
                                              shaders.at(info.frag_shader).get(), VertBufContainer::getDescription()),
            .base_color_texture = info.base_color_texture,
        },
    });
    return id;
}

void MaterialContainer::bindResource(vk::CommandBuffer cmd_buf, GlobalMaterialId material_id) const {
    const auto &material = materials.at(material_id);
    const auto &texture = textures.at(material.base_color_texture);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, material.pipeline.get());
    cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout.get(), 0, {texture.descset.get()}, {});
}

vk::PipelineLayout MaterialContainer::getPipelineLayout() const { return pipeline_layout.get(); }

} // namespace Pelican
