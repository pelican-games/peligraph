#include "materialcontainer.hpp"
#include "../loader/imageloader.hpp"
#include "../renderingpass/materialpassattachments.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/surfacecompiler.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/util.hpp"
#include "../watch/reloadservice.hpp"
#include "standardmaterialresource.hpp"
#include "materialvaluesreloadhandler.hpp"
#include "texturereloadhandler.hpp"
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
        << static_cast<int>(info.shader_contract) << ':'
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
    if (info.shader_contract == MaterialShaderContract::forward_scene_color_v1) {
        desc.color_formats.push_back(forwardMaterialPassColorAttachmentFormat);
    } else {
        desc.color_formats.assign(formats.begin(), formats.end());
    }
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
    if (info.render_state.blend != SurfaceBlendMode::opaque) {
        std::vector<vk::Format> formats;
        if (info.shader_contract == MaterialShaderContract::forward_scene_color_v1) {
            formats.push_back(forwardMaterialPassColorAttachmentFormat);
        } else {
            const auto &material_formats = materialPassColorAttachmentFormats(
                GET_MODULE(RenderingPassContainer).isFeatureEnabled("hdr"));
            formats.assign(material_formats.begin(), material_formats.end());
        }
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

static vk::UniqueDescriptorPool createDescriptorPool(vk::Device device, bool split_custom_samplers) {
    std::vector<vk::DescriptorPoolSize> pool_sizes{
        {vk::DescriptorType::eStorageBuffer, 1024},
        {vk::DescriptorType::eCombinedImageSampler, 32768},
    };
    if (split_custom_samplers) {
        pool_sizes.push_back({vk::DescriptorType::eSampledImage, 16384});
        pool_sizes.push_back({vk::DescriptorType::eSampler, 16384});
    }

    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = 1024;
    create_info.setPoolSizes(pool_sizes);
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
    create_info.maxLod = VK_LOD_CLAMP_NONE;
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
    create_info.subresourceRange.levelCount = image.mip_levels;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount = 1;

    return device.createImageViewUnique(create_info);
}

MaterialContainer::MaterialContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      split_custom_samplers{surfaceSpvLinkExperimentalEnabled()},
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)},
      desc_pool{createDescriptorPool(device, split_custom_samplers)},
      material_buffer{GET_MODULE(VulkanManageCore).allocBuf(
          sizeof(MaterialGpuData) * maxMaterials, vk::BufferUsageFlagBits::eStorageBuffer,
          vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite)} {}
MaterialContainer::~MaterialContainer() {
    deferred_callbacks.closeAndWait();
}

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
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                                       vk::ImageUsageFlagBits::eTransferSrc,
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

namespace {

vk::Format vkFormatForLoaded(ImagePixelFormat format) {
    switch (format) {
    case ImagePixelFormat::Rgba8Unorm: return vk::Format::eR8G8B8A8Unorm;
    case ImagePixelFormat::Rgba8Srgb: return vk::Format::eR8G8B8A8Srgb;
    case ImagePixelFormat::Rgba16Sfloat: return vk::Format::eR16G16B16A16Sfloat;
    case ImagePixelFormat::Rgba32Sfloat: return vk::Format::eR32G32B32A32Sfloat;
    case ImagePixelFormat::Bc5Unorm: return vk::Format::eBc5UnormBlock;
    case ImagePixelFormat::Bc7Unorm: return vk::Format::eBc7UnormBlock;
    case ImagePixelFormat::Bc7Srgb: return vk::Format::eBc7SrgbBlock;
    }
    throw std::runtime_error("Unknown loaded image format");
}

void requireCompressedTextureFeatures(vk::PhysicalDevice physical_device, vk::Format format,
                                      std::string_view name) {
    if (format != vk::Format::eBc5UnormBlock && format != vk::Format::eBc7UnormBlock &&
        format != vk::Format::eBc7SrgbBlock) return;
    const auto features = physical_device.getFormatProperties(format).optimalTilingFeatures;
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
                          vk::FormatFeatureFlagBits::eTransferDst;
    if ((features & required) != required)
        throw std::runtime_error("KTX2 texture '" + std::string{name} + "' format " +
                                 vk::to_string(format) +
                                 " lacks sampled/linear-filter/transfer-dst GPU support");
}

} // namespace

MaterialContainer::InternalTextureResource
MaterialContainer::createTextureResource(const LoadedImage &loaded, std::string_view name) const {
    if (loaded.pixels.empty() || loaded.levels.empty())
        throw std::runtime_error("Texture '" + std::string{name} + "' has no image levels");
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto format = vkFormatForLoaded(loaded.format);
    requireCompressedTextureFeatures(vkcore.getPhysDevice(), format, name);

    const bool rgba8 = loaded.format == ImagePixelFormat::Rgba8Unorm ||
                       loaded.format == ImagePixelFormat::Rgba8Srgb;
    const bool bc7 = loaded.format == ImagePixelFormat::Bc7Unorm ||
                     loaded.format == ImagePixelFormat::Bc7Srgb;
    const std::array rgba_formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb};
    const std::array bc7_formats{vk::Format::eBc7UnormBlock, vk::Format::eBc7SrgbBlock};
    const auto compatible = rgba8 ? std::span<const vk::Format>{rgba_formats}
                                  : bc7 ? std::span<const vk::Format>{bc7_formats}
                                        : std::span<const vk::Format>{};
    auto image = vkcore.allocImage({loaded.width, loaded.height, 1}, format,
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                                       vk::ImageUsageFlagBits::eTransferSrc,
                                   vma::MemoryUsage::eAutoPreferDevice, {}, VulkanProcessType::graphics,
                                   compatible, loaded.mipLevels());
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(loaded.levels.size());
    for (std::uint32_t mip = 0; mip < loaded.levels.size(); ++mip) {
        const auto &level = loaded.levels[mip];
        vk::BufferImageCopy copy;
        copy.bufferOffset = level.offset;
        copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1};
        copy.imageExtent = vk::Extent3D{level.width, level.height, 1};
        regions.push_back(copy);
    }
    GET_MODULE(VulkanUtils).safeTransferMemoryToImageLevels(
        image, loaded.pixels.data(), loaded.pixels.size(), regions,
        VulkanUtils::ImageTransferInfo{.old_layout = vk::ImageLayout::eUndefined,
                                       .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                       .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                                                    vk::PipelineStageFlagBits::eFragmentShader,
                                       .dst_access = vk::AccessFlagBits::eShaderRead});

    const auto linear_format = rgba8 ? vk::Format::eR8G8B8A8Unorm
                                     : bc7 ? vk::Format::eBc7UnormBlock : format;
    requireCompressedTextureFeatures(vkcore.getPhysDevice(), linear_format, name);
    auto linear_view = createImageView(device, image, linear_format);
    vk::UniqueImageView srgb_view;
    if (rgba8 || bc7) {
        const auto srgb_format = rgba8 ? vk::Format::eR8G8B8A8Srgb : vk::Format::eBc7SrgbBlock;
        const auto features = vkcore.getPhysDevice().getFormatProperties(srgb_format).optimalTilingFeatures;
        const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                              vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        if ((features & required) != required)
            throw std::runtime_error("KTX2 texture '" + std::string{name} + "' SRGB view " +
                                     vk::to_string(srgb_format) + " lacks sampled/linear-filter GPU support");
        srgb_view = createImageView(device, image, srgb_format);
    }
    return {std::move(image), std::move(linear_view), std::move(srgb_view)};
}

GlobalTextureId MaterialContainer::registerTexture(const LoadedImage &loaded, std::string_view name) {
    return textures.reg(createTextureResource(loaded, name));
}

GlobalTextureId MaterialContainer::registerTextureFile(const std::filesystem::path &path) {
    return registerTexture(loadImageFile(path), path.string());
}

GlobalTextureId MaterialContainer::registerReloadableTextureFile(
    const watch::AssetKey &key, const std::filesystem::path &path) {
    const auto texture = registerTextureFile(path);
    auto &coordinator = GET_MODULE(watch::ReloadService).transactions();
    if (!texture_reload_handler) {
        texture_reload_handler = std::make_unique<TextureReloadHandler>(*this, coordinator);
    }
    texture_reload_handler->track(key, path, texture);
    return texture;
}

GlobalMaterialId MaterialContainer::registerMaterial(MaterialInfo info) {
    if (materials.size() >= maxMaterials) {
        throw std::runtime_error("Material capacity exceeded");
    }
    validateMaterialCapabilities(info);
    const auto &rendering_passes = GET_MODULE(RenderingPassContainer);
    if (rendering_passes.hasMaterialPasses() &&
        !rendering_passes.supportsMaterialPass(info.route, info.shader_contract,
                                               info.exact_pass)) {
        const auto selected = info.exact_pass
                                  ? " exact pass '" + *info.exact_pass + "'"
                                  : std::string{};
        throw std::runtime_error("material route '" +
                                 std::string{materialRouteClassName(info.route)} +
                                 "' with shader contract '" +
                                 std::string{materialShaderContractName(info.shader_contract)} +
                                 "' has no compatible registered material pass" + selected);
    }
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
    const auto image_info_count = materialCustomTextureFirstBinding +
        info.custom_textures.size() * (split_custom_samplers ? 2 : 1);
    std::vector<vk::DescriptorImageInfo> image_infos(image_info_count);
    std::vector<InternalMaterialInfo::TextureBinding> texture_bindings;
    texture_bindings.reserve(texture_binding_count +
                             info.custom_textures.size() * (split_custom_samplers ? 2 : 1));

    const auto setImageInfo = [&](uint32_t binding, GlobalTextureId texture, vk::Sampler sampler,
                                  bool srgb) {
        const auto &tex = textures.get(texture);
        if (srgb && !tex.srgb_view) {
            throw std::runtime_error("Color texture does not provide an SRGB view");
        }
        image_infos[binding].imageView = srgb ? tex.srgb_view.get() : tex.linear_view.get();
        image_infos[binding].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_infos[binding].sampler = sampler;
        texture_bindings.push_back({texture, binding,
                                    vk::DescriptorType::eCombinedImageSampler, sampler, srgb});
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
            const auto binding = materialCustomTextureFirstBinding + static_cast<std::uint32_t>(
                i * (split_custom_samplers ? 2 : 1));
            setImageInfo(binding, texture, linear_sampler.get(),
                         custom.role == SurfaceTextureRole::color);
            if (split_custom_samplers) {
                texture_bindings.back().descriptor_type = vk::DescriptorType::eSampledImage;
                image_infos[binding + 1] = image_infos[binding];
                texture_bindings.push_back({texture, binding + 1,
                                            vk::DescriptorType::eSampler,
                                            linear_sampler.get(),
                                            custom.role == SurfaceTextureRole::color});
            }
        } catch (const std::exception &error) {
            throw std::runtime_error("material texture '" + custom.name + "': " + error.what());
        }
    }

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(texture_binding_count + info.custom_textures.size() + 1);
    const auto addImageWrite = [&](uint32_t binding, vk::DescriptorType type) {
        vk::WriteDescriptorSet write;
        write.dstSet = descset.get();
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorType = type;
        write.descriptorCount = 1;
        write.pImageInfo = &image_infos[binding];
        writes.push_back(write);
    };

    addImageWrite(baseColorBinding, vk::DescriptorType::eCombinedImageSampler);
    addImageWrite(metallicRoughnessBinding, vk::DescriptorType::eCombinedImageSampler);
    addImageWrite(normalBinding, vk::DescriptorType::eCombinedImageSampler);
    addImageWrite(emissiveBinding, vk::DescriptorType::eCombinedImageSampler);
    if (info.vat) {
        addImageWrite(vatPositionBinding, vk::DescriptorType::eCombinedImageSampler);
        addImageWrite(vatNormalBinding, vk::DescriptorType::eCombinedImageSampler);
    }
    for (std::size_t i = 0; i < info.custom_textures.size(); ++i) {
        const auto binding = materialCustomTextureFirstBinding + static_cast<std::uint32_t>(
            i * (split_custom_samplers ? 2 : 1));
        if (split_custom_samplers) {
            addImageWrite(binding, vk::DescriptorType::eSampledImage);
            addImageWrite(binding + 1, vk::DescriptorType::eSampler);
        } else {
            addImageWrite(binding, vk::DescriptorType::eCombinedImageSampler);
        }
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
        .route = info.route,
        .shader_contract = info.shader_contract,
        .exact_pass = std::move(info.exact_pass),
        .base_color_texture = info.base_color_texture,
        .metallic_roughness_texture = info.metallic_roughness_texture,
        .normal_texture = info.normal_texture,
        .emissive_texture = info.emissive_texture,
        .vat = info.vat,
        .texture_bindings = std::move(texture_bindings),
        .custom_values_layout = info.custom_values_layout,
        .custom_values = info.custom_values,
        .descriptor_revision = 0,
        .descset = std::move(descset),
    }, static_cast<GlobalMaterialId::BaseType>(maxMaterials));
    try {
        if (material_id.value < 0 || static_cast<size_t>(material_id.value) >= maxMaterials) {
            throw std::runtime_error("Material capacity exceeded");
        }
        GET_MODULE(VulkanManageCore)
            .writeBuf(material_buffer, &gpu_data, sizeof(MaterialGpuData) * material_id.value,
                      sizeof(gpu_data));
        for (const auto &binding : materials.get(material_id).texture_bindings) {
            texture_materials[binding.texture].insert(material_id);
        }
        if (texture_reload_handler) texture_reload_handler->materialRegistered(material_id);
    } catch (...) {
        for (auto it = texture_materials.begin(); it != texture_materials.end();) {
            it->second.erase(material_id);
            if (it->second.empty()) it = texture_materials.erase(it);
            else ++it;
        }
        (void)materials.extract(material_id);
        throw;
    }
    return material_id;
}

void MaterialContainer::releaseModelResources(
    std::vector<GlobalMaterialId> material_ids,
    std::vector<GlobalTextureId> texture_ids, bool deferred) noexcept {
    try {
        struct RetiredResources {
            std::vector<GlobalMaterialId> material_ids;
            std::vector<InternalMaterialInfo> materials;
            std::vector<InternalTextureResource> textures;
            std::function<void()> recycle_material_ids;

            RetiredResources() = default;
            RetiredResources(const RetiredResources &) = delete;
            RetiredResources &operator=(const RetiredResources &) = delete;
            RetiredResources(RetiredResources &&other) noexcept
                : material_ids{std::move(other.material_ids)},
                  materials{std::move(other.materials)},
                  textures{std::move(other.textures)},
                  recycle_material_ids{std::exchange(other.recycle_material_ids, {})} {}
            RetiredResources &operator=(RetiredResources &&) = delete;
            ~RetiredResources() noexcept {
                if (!recycle_material_ids) return;
                try {
                    recycle_material_ids();
                } catch (const std::exception &error) {
                    if (logger)
                        LOG_ERROR(logger, "failed to recycle retired model material slots: {}",
                                  error.what());
                } catch (...) {
                    if (logger)
                        LOG_ERROR(logger, "failed to recycle retired model material slots");
                }
            }
        } retired;
        retired.material_ids.reserve(material_ids.size());
        retired.materials.reserve(material_ids.size());
        retired.textures.reserve(texture_ids.size());

        for (const auto material : material_ids) {
            if (!materials.contains(material)) continue;
            const auto bindings = materials.get(material).texture_bindings;
            for (const auto &binding : bindings) {
                const auto found = texture_materials.find(binding.texture);
                if (found == texture_materials.end()) continue;
                found->second.erase(material);
                if (found->second.empty()) texture_materials.erase(found);
            }
            auto value = materials.extract(material, !deferred);
            if (value) {
                if (deferred) retired.material_ids.push_back(material);
                retired.materials.push_back(std::move(*value));
            }
        }

        for (const auto texture : texture_ids) {
            const auto reverse = texture_materials.find(texture);
            if (reverse != texture_materials.end() && !reverse->second.empty()) {
                if (logger) {
                    LOG_ERROR(logger,
                              "model texture {} still has {} material references during retirement",
                              texture.value, reverse->second.size());
                }
                continue;
            }
            texture_materials.erase(texture);
            auto value = textures.extract(texture);
            if (value) retired.textures.push_back(std::move(*value));
        }

        if (!retired.material_ids.empty()) {
            retired.recycle_material_ids =
                [this, callback = deferred_callbacks.callback(),
                 ids = retired.material_ids] {
                    const auto lease = callback.acquire();
                    if (!lease) return;
                    for (const auto id : ids) materials.recycle(id);
                };
        }
        if (deferred && (!retired.materials.empty() || !retired.textures.empty())) {
            if (auto *queue = FastModuleContainer::tryGet<DeletionQueue>()) {
                queue->defer(std::move(retired));
            }
        }
    } catch (const std::exception &error) {
        if (logger) LOG_ERROR(logger, "failed to release model material resources: {}", error.what());
    } catch (...) {
        if (logger) LOG_ERROR(logger, "failed to release model material resources");
    }
}

namespace {

bool sameValuesLayout(const Std140Layout &left, const Std140Layout &right) {
    if (left.size != right.size || left.alignment != right.alignment ||
        left.members.size() != right.members.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.members.size(); ++i) {
        const auto &a = left.members[i];
        const auto &b = right.members[i];
        if (a.name != b.name || a.type != b.type || a.offset != b.offset ||
            a.size != b.size || a.alignment != b.alignment) {
            return false;
        }
    }
    return true;
}

} // namespace

bool MaterialContainer::materialValuesLayoutMatches(GlobalMaterialId material,
                                                     const Std140Layout &layout) const {
    return sameValuesLayout(materials.get(material).custom_values_layout, layout);
}

void MaterialContainer::updateMaterialValues(GlobalMaterialId material,
                                             const Std140Layout &layout,
                                             std::span<const std::byte> values) {
    auto &info = materials.get(material);
    if (!sameValuesLayout(info.custom_values_layout, layout)) {
        throw std::runtime_error("material values update requires an identical std140 layout");
    }
    if (values.size() != layout.size || values.size() > materialCustomValueCapacity) {
        throw std::runtime_error("material values update byte count does not match its layout");
    }

    std::array<std::uint32_t, materialCustomValueCapacity / 4> packed{};
    if (!values.empty()) std::memcpy(packed.data(), values.data(), values.size());
    // MaterialBuffer is shared by in-flight frames. The frame-boundary caller
    // owns publication, while this wait protects readers of the live SSBO.
    GET_MODULE(VulkanManageCore).waitIdle();
    const auto offset = sizeof(MaterialGpuData) * material.value +
                        offsetof(MaterialGpuData, custom_values);
    GET_MODULE(VulkanManageCore).writeBuf(material_buffer, packed.data(), offset,
                                          sizeof(packed));
    info.custom_values.assign(values.begin(), values.end());
}

void MaterialContainer::updateMaterialValues(GlobalMaterialId material,
                                             std::span<const std::byte> values) {
    updateMaterialValues(material, materials.get(material).custom_values_layout, values);
}

void MaterialContainer::registerReloadableMaterialValuesFile(
    const watch::AssetKey &key, const std::filesystem::path &path,
    MaterialSurfaceCatalog surfaces,
    std::span<const ReloadableMaterialValuesBinding> bindings) {
    auto &coordinator = GET_MODULE(watch::ReloadService).transactions();
    if (!material_values_reload_handler) {
        material_values_reload_handler =
            std::make_unique<MaterialValuesReloadHandler>(*this, coordinator);
    }
    material_values_reload_handler->track(key, path, std::move(surfaces), bindings);
}

std::function<void()> MaterialContainer::prepareSurfaceMaterialReload(
    const std::map<watch::AssetKey, SurfaceFormatDocument> &surface_documents,
    std::span<const watch::AssetKey> material_documents) {
    if (!material_values_reload_handler) return {};
    return material_values_reload_handler->prepareSurfaceReload(
        surface_documents, material_documents);
}

bool MaterialContainer::textureShapeMatches(GlobalTextureId texture,
                                            const LoadedImage &image) const {
    const auto &live = textures.get(texture).image;
    return live.extent == vk::Extent3D{image.width, image.height, 1} &&
           live.format == vkFormatForLoaded(image.format) &&
           live.mip_levels == image.mipLevels();
}

void MaterialContainer::validateTextureReload(GlobalTextureId texture,
                                              const LoadedImage &image) const {
    const bool has_srgb_view = image.format == ImagePixelFormat::Rgba8Unorm ||
                               image.format == ImagePixelFormat::Rgba8Srgb ||
                               image.format == ImagePixelFormat::Bc7Unorm ||
                               image.format == ImagePixelFormat::Bc7Srgb;
    const auto reverse = texture_materials.find(texture);
    if (reverse == texture_materials.end()) return;
    for (const auto material_id : reverse->second) {
        const auto &material = materials.get(material_id);
        if (!has_srgb_view && std::any_of(material.texture_bindings.begin(),
                                         material.texture_bindings.end(),
                                         [texture](const auto &binding) {
                                             return binding.texture == texture && binding.srgb;
                                         })) {
            throw std::runtime_error(
                "reloaded color texture format does not provide an SRGB view");
        }
    }
}

void MaterialContainer::uploadTextureInPlace(GlobalTextureId texture,
                                             const LoadedImage &image) const {
    if (!textureShapeMatches(texture, image)) {
        throw std::runtime_error("in-place texture upload requires identical extent/format/mips");
    }
    // The logical image handle must stay unchanged, so there is no old image
    // to defer. Drain prior readers before writing the live allocation.
    GET_MODULE(VulkanManageCore).waitIdle();
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(image.levels.size());
    for (std::uint32_t mip = 0; mip < image.levels.size(); ++mip) {
        const auto &level = image.levels[mip];
        vk::BufferImageCopy copy;
        copy.bufferOffset = level.offset;
        copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1};
        copy.imageExtent = vk::Extent3D{level.width, level.height, 1};
        regions.push_back(copy);
    }
    GET_MODULE(VulkanUtils).safeTransferMemoryToImageLevels(
        textures.get(texture).image, image.pixels.data(), image.pixels.size(), regions,
        VulkanUtils::ImageTransferInfo{
            .old_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                         vk::PipelineStageFlagBits::eFragmentShader,
            .dst_access = vk::AccessFlagBits::eShaderRead});
}

std::vector<MaterialContainer::StagedMaterialDescriptor>
MaterialContainer::stageTextureRebind(
    GlobalTextureId texture, const InternalTextureResource &replacement) const {
    std::vector<StagedMaterialDescriptor> staged;
    const auto reverse = texture_materials.find(texture);
    if (reverse == texture_materials.end()) return staged;
    staged.reserve(reverse->second.size());
    for (const auto material_id : reverse->second) {
        const auto &material = materials.get(material_id);
        vk::DescriptorSetAllocateInfo allocation;
        allocation.descriptorPool = desc_pool.get();
        const auto layout = GET_MODULE(PipelineFactory).descriptorSetLayout(
            material.pipeline, imageDescriptorSetNumber);
        allocation.setSetLayouts({layout});
        auto descriptor = std::move(device.allocateDescriptorSetsUnique(allocation).front());

        std::vector<vk::DescriptorImageInfo> infos;
        std::vector<vk::WriteDescriptorSet> writes;
        infos.reserve(material.texture_bindings.size());
        writes.reserve(material.texture_bindings.size() + 1);
        for (const auto &binding : material.texture_bindings) {
            const auto &resource = binding.texture == texture
                                       ? replacement
                                       : textures.get(binding.texture);
            if (binding.srgb && !resource.srgb_view) {
                throw std::runtime_error("reloaded color texture does not provide an SRGB view");
            }
            infos.push_back(vk::DescriptorImageInfo{
                binding.sampler,
                binding.srgb ? resource.srgb_view.get() : resource.linear_view.get(),
                vk::ImageLayout::eShaderReadOnlyOptimal});
            vk::WriteDescriptorSet write;
            write.dstSet = descriptor.get();
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = binding.descriptor_type;
            write.pImageInfo = &infos.back();
            writes.push_back(write);
        }

        vk::DescriptorBufferInfo buffer_info{material_buffer.buffer.get(), 0, vk::WholeSize};
        vk::WriteDescriptorSet buffer_write;
        buffer_write.dstSet = descriptor.get();
        buffer_write.dstBinding = materialBufferBinding;
        buffer_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        buffer_write.setBufferInfo(buffer_info);
        writes.push_back(buffer_write);
        device.updateDescriptorSets(writes, {});
        staged.push_back({material_id, std::move(descriptor)});
    }
    return staged;
}

MaterialContainer::RetiredTextureResources MaterialContainer::commitTextureRebind(
    GlobalTextureId texture, InternalTextureResource replacement,
    std::vector<StagedMaterialDescriptor> descriptors) {
    const auto reverse = texture_materials.find(texture);
    const auto expected = reverse == texture_materials.end() ? 0 : reverse->second.size();
    if (descriptors.size() != expected) {
        throw std::runtime_error("staged texture descriptor set is incomplete");
    }
    std::unordered_set<GlobalMaterialId, GlobalMaterialId::Hash> unique;
    for (const auto &descriptor : descriptors) {
        if (!descriptor.descriptor || !unique.insert(descriptor.material).second ||
            reverse == texture_materials.end() ||
            !reverse->second.contains(descriptor.material)) {
            throw std::runtime_error("staged texture descriptor set is invalid");
        }
        (void)materials.get(descriptor.material);
    }
    auto &slot = textures.get(texture);
    RetiredTextureResources retired{std::move(slot), {}};
    retired.descriptors.reserve(descriptors.size());
    slot = std::move(replacement);
    for (auto &descriptor : descriptors) {
        auto &material = materials.get(descriptor.material);
        retired.descriptors.push_back(std::move(material.descset));
        material.descset = std::move(descriptor.descriptor);
        ++material.descriptor_revision;
    }
    return retired;
}

size_t MaterialContainer::referencingMaterialCountForTesting(GlobalTextureId texture) const {
    const auto found = texture_materials.find(texture);
    return found == texture_materials.end() ? 0 : found->second.size();
}

size_t MaterialContainer::materialCapacityForTesting() const { return maxMaterials; }

bool MaterialContainer::handlesTextureReload(const watch::AssetKey &key) const {
    return texture_reload_handler && texture_reload_handler->handles(key);
}

bool MaterialContainer::enqueueTextureReload(const watch::ReloadRequest &request,
                                             watch::ReloadCoordinator &coordinator) {
    return texture_reload_handler && texture_reload_handler->enqueue(request, coordinator);
}

bool MaterialContainer::retireTextureReloadPayload(
    std::shared_ptr<const void> payload, watch::ReloadCoordinator &coordinator) noexcept {
    return texture_reload_handler &&
           texture_reload_handler->retire(std::move(payload), coordinator);
}

bool MaterialContainer::handlesMaterialValuesReload(const watch::AssetKey &key) const {
    return material_values_reload_handler && material_values_reload_handler->handles(key);
}

bool MaterialContainer::enqueueMaterialValuesReload(
    const watch::ReloadRequest &request, watch::ReloadCoordinator &coordinator) {
    return material_values_reload_handler &&
           material_values_reload_handler->enqueue(request, coordinator);
}

bool MaterialContainer::retireMaterialValuesReloadPayload(
    std::shared_ptr<const void> payload, watch::ReloadCoordinator &coordinator) noexcept {
    return material_values_reload_handler &&
           material_values_reload_handler->retire(std::move(payload), coordinator);
}

std::pair<vk::ImageView, vk::ImageView>
MaterialContainer::textureViewsForTesting(GlobalTextureId texture) const {
    const auto &resource = textures.get(texture);
    return {resource.linear_view.get(), resource.srgb_view.get()};
}

std::vector<uint8_t> MaterialContainer::texturePixelsForTesting(GlobalTextureId texture) const {
    const auto &resource = textures.get(texture);
    if (resource.image.format != vk::Format::eR8G8B8A8Unorm &&
        resource.image.format != vk::Format::eR8G8B8A8Srgb) {
        throw std::runtime_error("texture test readback only supports RGBA8");
    }
    const auto bytes = static_cast<vk::DeviceSize>(resource.image.extent.width) *
                       resource.image.extent.height * 4;
    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto staging = vkcore.allocBuf(bytes, vk::BufferUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferHost,
                                   vma::AllocationCreateFlagBits::eHostAccessRandom);
    auto &utils = GET_MODULE(VulkanUtils);
    utils.executeOneTimeCmd(
        [&](vk::CommandBuffer command) {
            utils.changeImageLayoutCmd(
                command, resource.image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::ImageLayout::eTransferSrcOptimal,
                {.src_stage = vk::PipelineStageFlagBits::eVertexShader |
                              vk::PipelineStageFlagBits::eFragmentShader,
                 .dst_stage = vk::PipelineStageFlagBits::eTransfer,
                 .src_access = vk::AccessFlagBits::eShaderRead,
                 .dst_access = vk::AccessFlagBits::eTransferRead});
            vk::BufferImageCopy copy;
            copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copy.imageExtent = resource.image.extent;
            command.copyImageToBuffer(resource.image.image.get(),
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      staging.buffer.get(), copy);
            utils.changeImageLayoutCmd(
                command, resource.image, vk::ImageLayout::eTransferSrcOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                {.src_stage = vk::PipelineStageFlagBits::eTransfer,
                 .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                              vk::PipelineStageFlagBits::eFragmentShader,
                 .src_access = vk::AccessFlagBits::eTransferRead,
                 .dst_access = vk::AccessFlagBits::eShaderRead});
        },
        true);
    return vkcore.readBuf(staging, bytes);
}

std::vector<std::byte>
MaterialContainer::materialGpuValuesForTesting(GlobalMaterialId material) const {
    const auto &info = materials.get(material);
    const auto offset = sizeof(MaterialGpuData) * material.value +
                        offsetof(MaterialGpuData, custom_values);
    const auto bytes = GET_MODULE(VulkanManageCore).readBuf(
        material_buffer, offset + materialCustomValueCapacity);
    std::vector<std::byte> values(info.custom_values_layout.size);
    if (!values.empty()) {
        std::memcpy(values.data(), bytes.data() + offset, values.size());
    }
    return values;
}

std::vector<std::byte>
MaterialContainer::materialGpuRecordForTesting(GlobalMaterialId material) const {
    (void)materials.get(material);
    const auto offset = sizeof(MaterialGpuData) * material.value;
    const auto bytes = GET_MODULE(VulkanManageCore).readBuf(
        material_buffer, offset + sizeof(MaterialGpuData));
    std::vector<std::byte> record(sizeof(MaterialGpuData));
    std::memcpy(record.data(), bytes.data() + offset, record.size());
    return record;
}

bool MaterialContainer::isRenderRequired(const PassDefinition &pass,
                                         GlobalMaterialId material_id) const {
    if (!pass.isMaterial()) return false;
    const auto &material = materials.get(material_id);
    const auto contract = pass.materialInfo().contract;
    return materialPassAcceptsMaterial(contract, pass.name, material.route,
                                       material.shader_contract, material.exact_pass);
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
