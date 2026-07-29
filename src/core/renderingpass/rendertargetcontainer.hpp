#pragma once
#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../resourcecontainer.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/image.hpp"
#include "rendertargetmetadata.hpp"
#include "../../project/imagesubresource.hpp"
#include <map>
#include <optional>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

struct RenderTargetResourceSet {
    std::array<ImageWrapper, 2> images;
    std::array<std::vector<vk::UniqueImageView>, 2>
        image_layer_views;
    std::array<vk::UniqueImageView, 2>
        layered_image_views;
    std::array<
        std::map<ImageSubresourceViewKey,
                 vk::UniqueImageView>,
        2>
        subresource_image_views;
    std::array<ImageWrapper, 2> attachment_images;
    std::array<std::vector<vk::UniqueImageView>, 2>
        attachment_image_layer_views;
    std::array<vk::UniqueImageView, 2>
        layered_attachment_image_views;
    std::array<
        std::map<ImageSubresourceViewKey,
                 vk::UniqueImageView>,
        2>
        attachment_subresource_image_views;
};

class PreparedRenderTargetExtent {
    friend class RenderTargetContainer;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit PreparedRenderTargetExtent(
        std::unique_ptr<Impl> impl);

  public:
    PreparedRenderTargetExtent();
    ~PreparedRenderTargetExtent();
    PreparedRenderTargetExtent(
        const PreparedRenderTargetExtent &) = delete;
    PreparedRenderTargetExtent &operator=(
        const PreparedRenderTargetExtent &) = delete;
    PreparedRenderTargetExtent(
        PreparedRenderTargetExtent &&) noexcept;
    PreparedRenderTargetExtent &operator=(
        PreparedRenderTargetExtent &&) noexcept;

    bool valid() const noexcept;
    vk::Extent2D extent() const noexcept;
    std::size_t targetCount() const noexcept;
};

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
        ImageMipLevelCount mip_levels;
        std::uint32_t array_layers;
        RenderTargetStorageMode storage_mode;
        std::optional<std::string> alias_group;
        std::optional<std::uint64_t> alias_group_token;
        std::unique_ptr<RenderTargetResourceSet>
            resources;
    };
    ResourceContainer<GlobalRenderTargetId, InternalRenderTarget> render_targets;

    std::unordered_map<std::string, GlobalRenderTargetId> name_to_id;
    std::vector<GlobalRenderTargetId> registration_order;
    std::unordered_map<std::uint64_t, GlobalRenderTargetId>
        alias_group_owners;

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
                                              std::uint32_t samples = 1,
                                              ImageMipLevelCount mip_levels = {},
                                              std::uint32_t array_layers = 1,
                                              RenderTargetStorageMode storage_mode =
                                                  RenderTargetStorageMode::materialized,
                                              std::optional<std::string> alias_group =
                                                  std::nullopt,
                                              std::optional<std::uint64_t>
                                                  alias_group_token =
                                                      std::nullopt);
    std::uint64_t createAliasGroupToken();
    PreparedRenderTargetExtent prepareForExtent(
        vk::Extent2D base_extent) const;
    void publishPreparedExtent(
        PreparedRenderTargetExtent &&prepared);
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
    vk::ImageView getImageSubresourceView(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        bool array_view,
        bool history_read = false) const;
    vk::ImageView getImageSubresourceViewForFrame(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        bool array_view, bool history_read,
        std::uint32_t frame_index) const;
    vk::ImageView getImageLayerView(
        GlobalRenderTargetId id, std::uint32_t array_layer,
        bool history_read = false) const;
    vk::ImageView getImageLayerViewForFrame(
        GlobalRenderTargetId id, std::uint32_t array_layer,
        bool history_read, std::uint32_t frame_index) const;
    vk::ImageView getAttachmentImageView(
        GlobalRenderTargetId id, bool history_read = false) const;
    vk::ImageView getAttachmentImageLayerView(
        GlobalRenderTargetId id, std::uint32_t array_layer,
        bool history_read = false) const;
    vk::ImageView getAttachmentImageSubresourceView(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        bool array_view,
        bool history_read = false) const;
    vk::ImageView getLayeredImageView(
        GlobalRenderTargetId id, bool history_read = false) const;
    vk::ImageView getLayeredImageViewForFrame(
        GlobalRenderTargetId id, bool history_read,
        std::uint32_t frame_index) const;
    vk::ImageView getLayeredAttachmentImageView(
        GlobalRenderTargetId id, bool history_read = false) const;
    bool hasSeparateAttachment(GlobalRenderTargetId id) const;
    vk::SampleCountFlagBits sampleCount(GlobalRenderTargetId id) const;
    vk::ResolveModeFlagBits resolveMode(GlobalRenderTargetId id) const;
    vk::ImageLayout initialLayout(GlobalRenderTargetId id,
                                  bool attachment = false) const;
    std::optional<std::uint64_t>
    aliasGroup(GlobalRenderTargetId id) const;
    bool sharesAllocation(GlobalRenderTargetId left,
                          GlobalRenderTargetId right) const;

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
    void rebuildAliasGroupOwners();
    void bumpResourceRevision();
    uint32_t history_frame_index = 0;
    std::uint64_t next_alias_group_token = 1;
    std::uint64_t resource_revision = 0;
    vk::ResolveModeFlagBits depth_resolve_mode =
        vk::ResolveModeFlagBits::eSampleZero;
};

} // namespace Pelican
