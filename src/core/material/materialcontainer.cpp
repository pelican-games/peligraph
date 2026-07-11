#include "materialcontainer.hpp"
#include "../renderingpass/materialpassattachments.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include "standardmaterialresource.hpp"
#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Pelican {


constexpr uint32_t imageDescriptorSetNumber = PELICAN_SET_MATERIAL;
constexpr uint32_t baseColorBinding = 0;
constexpr uint32_t metallicRoughnessBinding = 1;
constexpr uint32_t normalBinding = 2;
constexpr uint32_t emissiveBinding = 3;
constexpr uint32_t vatPositionBinding = 4;
constexpr uint32_t vatNormalBinding = 5;
constexpr uint32_t materialBufferBinding = PELICAN_MATERIAL_BUFFER_BINDING;
constexpr uint32_t baseMaterialTextureBindingCount = 4;
constexpr uint32_t vatMaterialTextureBindingCount = 6;
constexpr size_t maxMaterials = 1024;

static std::string makePipelineKey(const MaterialInfo &info) {
    std::ostringstream key;
    key << info.vert_shader.value << ':' << info.frag_shader.value << ':'
        << info.skinned << ':'
        << static_cast<int>(info.render_state.blend) << ':'
        << static_cast<int>(info.render_state.cull) << ':'
        << info.render_state.depth_test << ':' << info.render_state.depth_write << ':'
        << static_cast<int>(info.render_state.depth_compare);
    return key.str();
}

static vk::CompareOp toVkCompare(SurfaceDepthCompare compare) {
    switch (compare) {
    case SurfaceDepthCompare::never: return vk::CompareOp::eNever;
    case SurfaceDepthCompare::less: return vk::CompareOp::eLess;
    case SurfaceDepthCompare::equal: return vk::CompareOp::eEqual;
    case SurfaceDepthCompare::less_equal: return vk::CompareOp::eLessOrEqual;
    case SurfaceDepthCompare::greater: return vk::CompareOp::eGreater;
    case SurfaceDepthCompare::not_equal: return vk::CompareOp::eNotEqual;
    case SurfaceDepthCompare::greater_equal: return vk::CompareOp::eGreaterOrEqual;
    case SurfaceDepthCompare::always: return vk::CompareOp::eAlways;
    }
    throw std::runtime_error("unknown material depth compare state");
}

static vk::CullModeFlags toVkCull(SurfaceCullMode cull) {
    switch (cull) {
    case SurfaceCullMode::none: return vk::CullModeFlagBits::eNone;
    case SurfaceCullMode::front: return vk::CullModeFlagBits::eFront;
    case SurfaceCullMode::back: return vk::CullModeFlagBits::eBack;
    }
    throw std::runtime_error("unknown material cull state");
}

static GraphicsPipelineDesc makeMaterialPipelineDesc(const MaterialInfo &info) {
    GraphicsPipelineDesc desc;
    desc.vert = info.vert_shader;
    desc.frag = info.frag_shader;
    const auto &formats = materialPassColorAttachmentFormats(
        GET_MODULE(RenderingPassContainer).isFeatureEnabled("hdr"));
    desc.color_formats.assign(formats.begin(), formats.end());
    desc.depth_format = materialPassDepthAttachmentFormat;
    desc.use_engine_vertex_layout = !info.skinned;
    desc.use_skinned_vertex_layout = info.skinned;
    desc.depth_test = info.render_state.depth_test;
    desc.depth_write = info.render_state.depth_write;
    desc.depth_compare = toVkCompare(info.render_state.depth_compare);
    desc.cull_mode = toVkCull(info.render_state.cull);
    desc.front_face = vk::FrontFace::eClockwise;
    if (info.render_state.blend == SurfaceBlendMode::blend) {
        desc.blend = true;
        desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
        desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
        desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
        desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    } else if (info.render_state.blend == SurfaceBlendMode::additive) {
        desc.blend = true;
        desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
        desc.dst_color_blend_factor = vk::BlendFactor::eOne;
        desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
        desc.dst_alpha_blend_factor = vk::BlendFactor::eOne;
    }
    return desc;
}

static void validateMaterialCapabilities(const MaterialInfo &info) {
    const auto physical_device = GET_MODULE(VulkanManageCore).getPhysDevice();
    const auto limits = physical_device.getProperties().limits;
    const auto reserved = info.vat ? vatMaterialTextureBindingCount : baseMaterialTextureBindingCount;
    const auto sampler_count = static_cast<std::uint32_t>(reserved + info.custom_textures.size());
    if (sampler_count > limits.maxPerStageDescriptorSamplers ||
        sampler_count > limits.maxDescriptorSetSamplers) {
        throw std::runtime_error("material custom textures require " +
                                 std::to_string(sampler_count) +
                                 " samplers but device descriptor limit is " +
                                 std::to_string(std::min(limits.maxPerStageDescriptorSamplers,
                                                         limits.maxDescriptorSetSamplers)));
    }
    if (!info.render_state.depth_test && info.render_state.depth_write) {
        throw std::runtime_error(
            "material render_state requests depth_write while depth_test is disabled");
    }
    const auto &formats = materialPassColorAttachmentFormats(
        GET_MODULE(RenderingPassContainer).isFeatureEnabled("hdr"));
    if (info.render_state.blend != SurfaceBlendMode::opaque) {
        for (const auto format : formats) {
            const auto features = physical_device.getFormatProperties(format).optimalTilingFeatures;
            if (!(features & vk::FormatFeatureFlagBits::eColorAttachmentBlend)) {
                throw std::runtime_error("material render_state blend lacks device capability for color format " +
                                         vk::to_string(format));
            }
        }
    }
    const auto depth_features = physical_device.getFormatProperties(materialPassDepthAttachmentFormat)
                                    .optimalTilingFeatures;
    if ((info.render_state.depth_test || info.render_state.depth_write) &&
        !(depth_features & vk::FormatFeatureFlagBits::eDepthStencilAttachment)) {
        throw std::runtime_error("material render_state depth lacks device capability for format " +
                                 vk::to_string(materialPassDepthAttachmentFormat));
    }
}

MaterialGpuData makeMaterialGpuData(const MaterialInfo &info) {
    MaterialGpuData data;
    data.base_color_factor = info.base_color_factor;
    data.emissive_factor = glm::vec4{info.emissive_factor, 1.0f};
    data.surface_factors =
        glm::vec4{info.metallic_factor, info.roughness_factor, info.normal_scale, info.occlusion_strength};
    if (info.vat) {
        const auto &vat = *info.vat;
        data.vat_bounds_min_frame_count =
            glm::vec4{vat.bounds_min, static_cast<float>(vat.frame_count)};
        data.vat_bounds_extent_fps = glm::vec4{vat.bounds_max - vat.bounds_min, vat.fps};
        data.vat_flags = glm::ivec4{vat.base_vertex, vat.loop ? 1 : 0, vat.has_normal ? 1 : 0, 0};
    }
    if (info.custom_values.size() > materialCustomValueCapacity) {
        throw std::runtime_error("material custom values size " +
                                 std::to_string(info.custom_values.size()) +
                                 " exceeds MaterialBuffer capacity " +
                                 std::to_string(materialCustomValueCapacity));
    }
    if (!info.custom_values.empty()) {
        std::memcpy(data.custom_values.data(), info.custom_values.data(), info.custom_values.size());
    }
    return data;
}

static vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
    vk::DescriptorPoolSize pool_size[2];
    pool_size[0].type = vk::DescriptorType::eStorageBuffer;
    pool_size[0].descriptorCount = 1024;
    pool_size[1].type = vk::DescriptorType::eCombinedImageSampler;
    pool_size[1].descriptorCount = 32768;

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

static vk::UniqueImageView createImageView(vk::Device device, const ImageWrapper &image, vk::Format format) {
    vk::ImageViewCreateInfo create_info;
    create_info.image = image.image.get();
    create_info.viewType = vk::ImageViewType::e2D;
    create_info.format = format;
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
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)}, desc_pool{createDescriptorPool(device)},
      material_buffer{GET_MODULE(VulkanManageCore).allocBuf(
          sizeof(MaterialGpuData) * maxMaterials, vk::BufferUsageFlagBits::eStorageBuffer,
          vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite)} {}
MaterialContainer::~MaterialContainer() {}

GlobalTextureId MaterialContainer::registerTexture(vk::Extent3D extent, const void *data) {
    return registerTexture(extent, data, vk::Format::eR8G8B8A8Unorm,
                           static_cast<vk::DeviceSize>(extent.width) * extent.height * extent.depth * 4);
}

GlobalTextureId MaterialContainer::registerTexture(vk::Extent3D extent, const void *data, vk::Format format,
                                                   vk::DeviceSize bytes_num) {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    const bool rgba8 = format == vk::Format::eR8G8B8A8Unorm;
    const std::array mutable_formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb};
    auto image = vkcore.allocImage(extent, format,
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferDevice, {}, VulkanProcessType::graphics,
                                   rgba8 ? std::span<const vk::Format>{mutable_formats}
                                         : std::span<const vk::Format>{});

    auto &vkutil = GET_MODULE(VulkanUtils);
    vkutil.safeTransferMemoryToImage(image, data, bytes_num,
                                     VulkanUtils::ImageTransferInfo{
                                         .old_layout = vk::ImageLayout::eUndefined,
                                         .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                         .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                                                      vk::PipelineStageFlagBits::eFragmentShader,
                                         .dst_access = vk::AccessFlagBits::eShaderRead,
                                     });

    auto linear_view = createImageView(device, image, format);
    vk::UniqueImageView srgb_view;
    if (rgba8) {
        const auto features = vkcore.getPhysDevice()
                                  .getFormatProperties(vk::Format::eR8G8B8A8Srgb)
                                  .optimalTilingFeatures;
        const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                              vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        if ((features & required) != required) {
            throw std::runtime_error("SRGB material texture lacks sampled/linear-filter support");
        }
        srgb_view = createImageView(device, image, vk::Format::eR8G8B8A8Srgb);
    }

    return textures.reg(InternalTextureResource{
        .image = std::move(image),
        .linear_view = std::move(linear_view),
        .srgb_view = std::move(srgb_view),
    });
}
GlobalMaterialId MaterialContainer::registerMaterial(MaterialInfo info) {
    validateMaterialCapabilities(info);
    const auto pipeline_key = makePipelineKey(info);
    auto pipeline_it = pipelines.find(pipeline_key);
    if (pipeline_it == pipelines.end()) {
        const auto pipeline_handle = GET_MODULE(PipelineFactory).create(makeMaterialPipelineDesc(info));
        pipeline_it = pipelines.emplace(pipeline_key, pipeline_handle).first;
        if (!default_pipeline) {
            default_pipeline = pipeline_handle;
        }
    }
    const auto pipeline = pipeline_it->second;

    vk::DescriptorSetAllocateInfo desc_alloc_info;
    desc_alloc_info.descriptorPool = desc_pool.get();
    const auto material_set_layout = GET_MODULE(PipelineFactory).descriptorSetLayout(pipeline, imageDescriptorSetNumber);
    desc_alloc_info.setSetLayouts({material_set_layout});

    auto descsets = device.allocateDescriptorSetsUnique(desc_alloc_info);
    auto &descset = descsets[0];

    const auto texture_binding_count =
        info.vat ? vatMaterialTextureBindingCount : baseMaterialTextureBindingCount;
    std::vector<vk::DescriptorImageInfo> image_infos(
        materialCustomTextureFirstBinding + info.custom_textures.size());

    const auto setImageInfo = [&](uint32_t binding, GlobalTextureId texture, vk::Sampler sampler,
                                  bool srgb) {
        const auto &tex = textures.get(texture);
        if (srgb && !tex.srgb_view) {
            throw std::runtime_error("Color texture does not provide an SRGB view");
        }
        image_infos[binding].imageView = srgb ? tex.srgb_view.get() : tex.linear_view.get();
        image_infos[binding].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_infos[binding].sampler = sampler;
    };

    setImageInfo(baseColorBinding, info.base_color_texture, linear_sampler.get(), true);
    setImageInfo(metallicRoughnessBinding, info.metallic_roughness_texture, linear_sampler.get(), false);
    setImageInfo(normalBinding, info.normal_texture, linear_sampler.get(), false);
    setImageInfo(emissiveBinding, info.emissive_texture, linear_sampler.get(), true);
    if (info.vat) {
        setImageInfo(vatPositionBinding, info.vat->position_texture, nearest_sampler.get(), false);
        setImageInfo(vatNormalBinding, info.vat->normal_texture, nearest_sampler.get(), false);
    }
    for (std::size_t i = 0; i < info.custom_textures.size(); ++i) {
        const auto &custom = info.custom_textures[i];
        const auto texture = custom.texture.value_or(
            GET_MODULE(StandardMaterialResource).defaultTexture(custom.missing_default));
        try {
            setImageInfo(materialCustomTextureFirstBinding + static_cast<std::uint32_t>(i),
                         texture, linear_sampler.get(), custom.role == SurfaceTextureRole::color);
        } catch (const std::exception &error) {
            throw std::runtime_error("material texture '" + custom.name + "': " + error.what());
        }
    }

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(texture_binding_count + info.custom_textures.size() + 1);
    const auto addImageWrite = [&](uint32_t binding) {
        vk::WriteDescriptorSet write;
        write.dstSet = descset.get();
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.descriptorCount = 1;
        write.pImageInfo = &image_infos[binding];
        writes.push_back(write);
    };

    addImageWrite(baseColorBinding);
    addImageWrite(metallicRoughnessBinding);
    addImageWrite(normalBinding);
    addImageWrite(emissiveBinding);
    if (info.vat) {
        addImageWrite(vatPositionBinding);
        addImageWrite(vatNormalBinding);
    }
    for (std::size_t i = 0; i < info.custom_textures.size(); ++i) {
        addImageWrite(materialCustomTextureFirstBinding + static_cast<std::uint32_t>(i));
    }

    vk::DescriptorBufferInfo material_buffer_info{material_buffer.buffer.get(), 0, vk::WholeSize};
    vk::WriteDescriptorSet material_write;
    material_write.dstSet = descset.get();
    material_write.dstBinding = materialBufferBinding;
    material_write.descriptorType = vk::DescriptorType::eStorageBuffer;
    material_write.setBufferInfo(material_buffer_info);
    writes.push_back(material_write);

    device.updateDescriptorSets(writes, {});

    const auto gpu_data = makeMaterialGpuData(info);
    const auto material_id = materials.reg(InternalMaterialInfo{
        .pipeline = pipeline,
        .base_color_texture = info.base_color_texture,
        .metallic_roughness_texture = info.metallic_roughness_texture,
        .normal_texture = info.normal_texture,
        .emissive_texture = info.emissive_texture,
        .vat = info.vat,
        .descset = std::move(descset),
    });
    if (material_id.value < 0 || static_cast<size_t>(material_id.value) >= maxMaterials) {
        throw std::runtime_error("Material capacity exceeded");
    }
    GET_MODULE(VulkanManageCore)
        .writeBuf(material_buffer, &gpu_data, sizeof(MaterialGpuData) * material_id.value, sizeof(gpu_data));
    return material_id;
}

std::pair<vk::ImageView, vk::ImageView>
MaterialContainer::textureViewsForTesting(GlobalTextureId texture) const {
    const auto &resource = textures.get(texture);
    return {resource.linear_view.get(), resource.srgb_view.get()};
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
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(material.pipeline));
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, imageDescriptorSetNumber,
                                   {material.descset.get()}, {});
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

vk::PipelineLayout MaterialContainer::pipelineLayout(GlobalMaterialId material_id) const {
    const auto &material = materials.get(material_id);
    return GET_MODULE(PipelineFactory).layout(material.pipeline);
}

} // namespace Pelican
