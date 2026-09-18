#pragma once

#include "../loader/imageloader.hpp"
#include "../resourcecontainer.hpp"
#include "../vkcore/image.hpp"
#include "material.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class TextureReloadHandler;

// One uploaded material texture and the two sampling views of it.
// Hoisted out of MaterialContainer::InternalTextureResource unchanged.
struct MaterialTextureResource {
    ImageWrapper image;
    vk::UniqueImageView linear_view;
    vk::UniqueImageView srgb_view;
    SurfaceTextureDimension dimension =
        SurfaceTextureDimension::two_d;
    vk::ImageViewType view_type =
        vk::ImageViewType::e2D;
};

// Format/shape vocabulary shared with the container's reload validation,
// which still has to consult the material table.
vk::Format vkFormatForLoadedImage(ImagePixelFormat format);
SurfaceTextureDimension surfaceTextureDimensionOf(
    LoadedImageDimension dimension);
void validateLoadedTextureShape(
    const LoadedImage &loaded, std::string_view name,
    const vk::PhysicalDeviceLimits &limits);
bool supportsDepthComparisonSampling(vk::Format format);

// Texture upload/registration and the sampler cache, with no knowledge of
// materials. The reverse index from texture to material and the descriptor
// restaging that a reload needs stay with MaterialContainer, which owns
// the material table.
class MaterialTextureStore {
  public:
    explicit MaterialTextureStore(vk::Device device);

    // The reload handler reaches the live resource of a tracked texture
    // through the owning container; the slot table is its one dependency
    // here, so it stays reachable rather than being copied out.
    friend class TextureReloadHandler;

    ResourceContainer<GlobalTextureId,
                      MaterialTextureResource>
        textures;

    GlobalTextureId registerTexture(vk::Extent3D extent,
                                    const void *data);
    GlobalTextureId registerTexture(
        vk::Extent3D extent, const void *data,
        vk::Format format, vk::DeviceSize bytes_num);
    GlobalTextureId registerTexture(
        const LoadedImage &loaded, std::string_view name);
    GlobalTextureId registerTextureFile(
        const std::filesystem::path &path);

    MaterialTextureResource createTextureResource(
        const LoadedImage &loaded,
        std::string_view name) const;
    bool textureShapeMatches(
        GlobalTextureId texture,
        const LoadedImage &image) const;
    void uploadTextureInPlace(
        GlobalTextureId texture,
        const LoadedImage &image) const;

    vk::Sampler nearestSampler() const {
        return nearest_sampler.get();
    }
    vk::Sampler linearSampler() const {
        return linear_sampler.get();
    }
    vk::Sampler materialTextureSampler(
        const ResolvedMaterialSampler &sampler);

    std::pair<vk::ImageView, vk::ImageView>
    textureViewsForTesting(
        GlobalTextureId texture) const;
    std::vector<std::uint8_t> texturePixelsForTesting(
        GlobalTextureId texture,
        std::uint32_t mip_level) const;

  private:
    using MaterialSamplerKey =
        std::tuple<SurfaceTextureFilter,
                   SurfaceTextureFilter,
                   SurfaceTextureAddressMode,
                   SurfaceTextureCompare, bool, float>;

    vk::Device device;
    vk::UniqueSampler nearest_sampler;
    vk::UniqueSampler linear_sampler;
    std::map<MaterialSamplerKey, vk::UniqueSampler>
        custom_texture_samplers;
};

} // namespace Pelican
