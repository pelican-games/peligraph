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
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
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
        std::uint32_t samples;
        std::array<ImageWrapper, 2> images;
        std::array<vk::UniqueImageView, 2> image_views;
        std::array<ImageWrapper, 2> attachment_images;
        std::array<vk::UniqueImageView, 2> attachment_image_views;
    };
    ResourceContainer<GlobalRenderTargetId, InternalRenderTarget> render_targets;

    std::unordered_map<std::string, GlobalRenderTargetId> name_to_id;
    std::vector<GlobalRenderTargetId> registration_order;

  public:
    struct RegistrationCheckpoint {
        std::size_t registration_count = 0;
        std::unordered_map<std::string, GlobalRenderTargetId>
            name_to_id;
    };

    RenderTargetContainer();
    ~RenderTargetContainer();

    GlobalRenderTargetId registerRenderTarget(const std::string &name, vk::Extent2D base_extent,
                                              const std::string &format_class,
                                              const std::string &role,
                                              float extent_scale, std::optional<vk::Extent2D> fixed_extent,
                                              vk::Format format, vk::ImageUsageFlags usage,
                                              vma::MemoryUsage memUsage, bool history = false,
                                              vk::ClearColorValue history_clear_color =
                                                  vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}},
                                              std::uint32_t samples = 1);
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
    const ImageWrapper &getAttachmentImage(
        GlobalRenderTargetId id, bool history_read = false) const;
    vk::ImageView getImageView(GlobalRenderTargetId id, bool history_read = false) const;
    vk::ImageView getImageViewForFrame(GlobalRenderTargetId id, bool history_read,
                                       uint32_t frame_index) const;
    vk::ImageView getAttachmentImageView(
        GlobalRenderTargetId id, bool history_read = false) const;
    bool hasSeparateAttachment(GlobalRenderTargetId id) const;
    vk::SampleCountFlagBits sampleCount(GlobalRenderTargetId id) const;
    vk::ResolveModeFlagBits resolveMode(GlobalRenderTargetId id) const;
    vk::ImageLayout initialLayout(GlobalRenderTargetId id,
                                  bool attachment = false) const;

    // Internal append-only transaction surface used by render-config
    // candidate registration.
    RegistrationCheckpoint checkpointRegistrations() const;
    void rollbackRegistrations(RegistrationCheckpoint checkpoint);
    std::vector<std::pair<std::string, GlobalRenderTargetId>>
    registrationsSince(RegistrationCheckpoint checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return registration_order.size();
    }
    std::unordered_map<std::string, GlobalRenderTargetId>
    currentNameBindings() const {
        return name_to_id;
    }
    void hideRegistrationName(const std::string &name,
                              GlobalRenderTargetId expected);
    void retireRegistrations(
        const std::vector<GlobalRenderTargetId> &ids) noexcept;

  private:
    uint32_t history_frame_index = 0;
    vk::ResolveModeFlagBits depth_resolve_mode =
        vk::ResolveModeFlagBits::eSampleZero;
};

} // namespace Pelican
