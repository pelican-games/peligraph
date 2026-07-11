#pragma once
#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../resourcecontainer.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/image.hpp"
#include "rendertargetmetadata.hpp"
#include <optional>
#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(RenderTargetContainer) {

    vk::Device device;

    struct InternalRenderTarget {
        std::string name;
        std::string format_class;
        float extent_scale;
        std::optional<vk::Extent2D> fixed_extent;
        vk::Format format;
        vk::ImageUsageFlags usage;
        vma::MemoryUsage memory_usage;
        ImageWrapper image;
        vk::UniqueImageView image_view;
    };
    ResourceContainer<GlobalRenderTargetId, InternalRenderTarget> render_targets;

    std::unordered_map<std::string, GlobalRenderTargetId> name_to_id;

  public:
    RenderTargetContainer();
    ~RenderTargetContainer();

    GlobalRenderTargetId registerRenderTarget(const std::string &name, vk::Extent2D base_extent,
                                              const std::string &format_class,
                                              float extent_scale, std::optional<vk::Extent2D> fixed_extent,
                                              vk::Format format, vk::ImageUsageFlags usage,
                                              vma::MemoryUsage memUsage);
    void recreateForExtent(vk::Extent2D base_extent);
    GlobalRenderTargetId getRenderTargetIdByName(const std::string &name) const;
    RenderTargetMetadata getMetadata(GlobalRenderTargetId id) const;
    const ImageWrapper &getImage(GlobalRenderTargetId id) const;
    vk::ImageView getImageView(GlobalRenderTargetId id) const;
};

} // namespace Pelican
