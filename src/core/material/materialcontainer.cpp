#include "materialcontainer.hpp"
#include "../renderingpass/materialpassattachments.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include <stdexcept>

namespace Pelican {


constexpr uint32_t modelMatDescriptorSetNumber = 0;
constexpr uint32_t modelMatDescriptorBinding = 0;
constexpr uint32_t imageDescriptorSetNumber = 1;
constexpr uint32_t baseColorBinding = 0;
constexpr uint32_t metallicRoughnessBinding = 1;
constexpr uint32_t normalBinding = 2;
constexpr uint32_t emissiveBinding = 3;
constexpr uint32_t materialTextureBindingCount = 4;

static uint64_t makePipelineKey(ShaderBundleId vert_shader, ShaderBundleId frag_shader) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(vert_shader.value)) << 32) |
           static_cast<uint32_t>(frag_shader.value);
}

static GraphicsPipelineDesc makeMaterialPipelineDesc(const MaterialInfo &info) {
    GraphicsPipelineDesc desc;
    desc.vert = info.vert_shader;
    desc.frag = info.frag_shader;
    desc.color_formats.assign(materialPassColorAttachmentFormats.begin(), materialPassColorAttachmentFormats.end());
    desc.depth_format = materialPassDepthAttachmentFormat;
    desc.use_engine_vertex_layout = true;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = vk::CompareOp::eLess;
    desc.cull_mode = vk::CullModeFlagBits::eBack;
    desc.front_face = vk::FrontFace::eClockwise;
    return desc;
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

static vk::UniqueDescriptorSet createModelMatDescriptorSet(vk::Device device, vk::DescriptorPool desc_pool,
                                                           vk::DescriptorSetLayout layout, vk::Buffer buffer) {
    vk::DescriptorSetAllocateInfo desc_alloc_info;
    desc_alloc_info.descriptorPool = desc_pool;
    desc_alloc_info.setSetLayouts({layout});

    auto descsets = device.allocateDescriptorSetsUnique(desc_alloc_info);
    auto &descset = descsets[0];

    vk::DescriptorBufferInfo buf_info;
    buf_info.buffer = buffer;
    buf_info.offset = 0;
    buf_info.range = vk::WholeSize;

    vk::WriteDescriptorSet write_descset;
    write_descset.dstSet = descset.get();
    write_descset.dstBinding = modelMatDescriptorBinding;
    write_descset.dstArrayElement = 0;
    write_descset.setBufferInfo({buf_info});
    write_descset.descriptorType = vk::DescriptorType::eStorageBuffer;
    device.updateDescriptorSets({write_descset}, {});

    return std::move(descset);
}

MaterialContainer::MaterialContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
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
    const auto pipeline_key = makePipelineKey(info.vert_shader, info.frag_shader);
    auto pipeline_it = pipelines.find(pipeline_key);
    if (pipeline_it == pipelines.end()) {
        const auto pipeline_handle = GET_MODULE(PipelineFactory).create(makeMaterialPipelineDesc(info));
        pipeline_it = pipelines.emplace(pipeline_key, pipeline_handle).first;
        if (!default_pipeline) {
            default_pipeline = pipeline_handle;
            if (model_mat_buffer) {
                const auto model_set_layout =
                    GET_MODULE(PipelineFactory).descriptorSetLayout(*default_pipeline, modelMatDescriptorSetNumber);
                model_mat_buf_descset =
                    createModelMatDescriptorSet(device, desc_pool.get(), model_set_layout, model_mat_buffer);
            }
        }
    }
    const auto pipeline = pipeline_it->second;

    vk::DescriptorSetAllocateInfo desc_alloc_info;
    desc_alloc_info.descriptorPool = desc_pool.get();
    const auto material_set_layout = GET_MODULE(PipelineFactory).descriptorSetLayout(pipeline, imageDescriptorSetNumber);
    desc_alloc_info.setSetLayouts({material_set_layout});

    auto descsets = device.allocateDescriptorSetsUnique(desc_alloc_info);
    auto &descset = descsets[0];

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
    
    // Emissive
    {
        const auto &tex = textures.get(info.emissive_texture);
        image_infos[3].imageView = tex.image_view.get();
        image_infos[3].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_infos[3].sampler = linear_sampler.get();
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
    
    writes[3].dstSet = descset.get();
    writes[3].dstBinding = emissiveBinding;
    writes[3].dstArrayElement = 0;
    writes[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &image_infos[3];

    device.updateDescriptorSets(writes, {});

    return materials.reg(InternalMaterialInfo{
        .pipeline = pipeline,
        .base_color_texture = info.base_color_texture,
        .metallic_roughness_texture = info.metallic_roughness_texture,
        .normal_texture = info.normal_texture,
        .emissive_texture = info.emissive_texture,
        .descset = std::move(descset),
    });
}

void MaterialContainer::setModelMatBuf(const BufferWrapper &buf) {
    model_mat_buffer = buf.buffer.get();
    if (!default_pipeline) {
        return;
    }

    const auto model_set_layout =
        GET_MODULE(PipelineFactory).descriptorSetLayout(*default_pipeline, modelMatDescriptorSetNumber);
    model_mat_buf_descset = createModelMatDescriptorSet(device, desc_pool.get(), model_set_layout, model_mat_buffer);
}

bool MaterialContainer::isRenderRequired(PassId pass_id, GlobalMaterialId material) const {
    // Pass-specific material filtering is not defined yet.
    return true;
}

void MaterialContainer::bindResource(vk::CommandBuffer cmd_buf, PassId pass_id, GlobalMaterialId material_id,
                                     GlobalMaterialId prev_material_id) const {
    const auto &material = materials.get(material_id);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline_layout = pipeline_factory.layout(material.pipeline);

    if (!isValidMaterialId(prev_material_id)) {
        if (!model_mat_buf_descset) {
            throw std::runtime_error("MaterialContainer has no model matrix descriptor set");
        }
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(material.pipeline));
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                   {
                                       model_mat_buf_descset.get(), // model matrix buffer: set = 0
                                       material.descset.get(),      // material textures: set = 1
                                   },
                                   {});
    } else {
        const auto &prev_material = materials.get(prev_material_id);
        if (material.pipeline != prev_material.pipeline)
            cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(material.pipeline));
        if (material.descset.get() != prev_material.descset.get())
            cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout,
                                       imageDescriptorSetNumber, {material.descset.get()}, {});
    }
}

vk::PipelineLayout MaterialContainer::getPipelineLayout() const {
    if (!default_pipeline) {
        throw std::runtime_error("MaterialContainer has no material pipeline");
    }
    return GET_MODULE(PipelineFactory).layout(*default_pipeline);
}

} // namespace Pelican
