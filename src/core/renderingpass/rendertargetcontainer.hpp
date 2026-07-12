#pragma once
#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../resourcecontainer.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/image.hpp"
#include "rendertargetmetadata.hpp"
#include <optional>
#include <array>
#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(RenderTargetContainer) {

    vk::Device device;

    struct InternalRenderTarget {
        std::string name;
        std::string format_class;
        std::string role;
        float extent_scale;
        std::optional<vk::Extent2D> fixed_extent;
        vk::Format format;
        vk::ImageUsageFlags usage;
        vma::MemoryUsage memory_usage;
        bool history;
        vk::ClearColorValue history_clear_color;
        std::array<ImageWrapper, 2> images;
        std::array<vk::UniqueImageView, 2> image_views;
    };
    ResourceContainer<GlobalRenderTargetId, InternalRenderTarget> render_targets;

    std::unordered_map<std::string, GlobalRenderTargetId> name_to_id;

  public:
    RenderTargetContainer();
    ~RenderTargetContainer();

    GlobalRenderTargetId registerRenderTarget(const std::string &name, vk::Extent2D base_extent,
                                              const std::string &format_class,
                                              const std::string &role,
                                              float extent_scale, std::optional<vk::Extent2D> fixed_extent,
                                              vk::Format format, vk::ImageUsageFlags usage,
                                              vma::MemoryUsage memUsage, bool history = false,
                                              vk::ClearColorValue history_clear_color =
                                                  vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}});
    void recreateForExtent(vk::Extent2D base_extent);
    void resetHistory();
    void advanceHistoryFrame();
    uint32_t historyFrameIndex() const { return history_frame_index; }
    uint32_t surfaceIndex(GlobalRenderTargetId id, bool history_read = false) const;
    GlobalRenderTargetId getRenderTargetIdByName(const std::string &name) const;
    RenderTargetMetadata getMetadata(GlobalRenderTargetId id) const;
    const ImageWrapper &getImage(GlobalRenderTargetId id, bool history_read = false) const;
    const ImageWrapper &getImageForFrame(GlobalRenderTargetId id, bool history_read,
                                         uint32_t frame_index) const;
    vk::ImageView getImageView(GlobalRenderTargetId id, bool history_read = false) const;
    vk::ImageView getImageViewForFrame(GlobalRenderTargetId id, bool history_read,
                                       uint32_t frame_index) const;
    vk::ImageLayout initialLayout(GlobalRenderTargetId id) const;

  private:
    uint32_t history_frame_index = 0;
};

} // namespace Pelican
