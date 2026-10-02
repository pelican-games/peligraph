#pragma once
#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../resourcecontainer.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/image.hpp"
#include "rendertargetdefinition.hpp"
#include "rendertargetmetadata.hpp"
#include "rendertargetread.hpp"
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
        ImageResourceDimension dimension =
            ImageResourceDimension::two_d;
        RenderTargetStorageMode storage_mode;
        std::optional<std::string> alias_group;
        std::optional<std::uint64_t> alias_group_token;
        // PS-1: RenderTargetDefinition::persistent of the latest
        // registration (a lifetime edit keeps the image).
        bool persistent = false;
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

    // The authored contract travels as one value. Only the frame-time base
    // extent, the allocator preference, and the alias-group token assigned
    // by registration are outside the definition.
    GlobalRenderTargetId registerRenderTarget(
        const RenderTargetDefinition &definition,
        vk::Extent2D base_extent,
        vma::MemoryUsage memUsage,
        std::optional<std::uint64_t>
            alias_group_token = std::nullopt);
    std::uint64_t createAliasGroupToken();
    PreparedRenderTargetExtent prepareForExtent(
        vk::Extent2D base_extent) const;
    void publishPreparedExtent(
        PreparedRenderTargetExtent &&prepared);
    // Rebuilds one non-history 2D target with an exact runtime array-layer
    // count. Used by provider-owned secondary families whose cardinality is
    // scene data rather than a fixed render-config constant.
    bool setRuntimeArrayLayers(
        GlobalRenderTargetId id,
        std::uint32_t array_layers);
    void resetHistory();
    void advanceHistoryFrame();
    uint32_t historyFrameIndex() const { return history_frame_index; }
    // The one place a read turns into a surface index.
    uint32_t surfaceIndex(GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    GlobalRenderTargetId getRenderTargetIdByName(const std::string &name) const;
    // The registration name of one target. Diagnostics that only need the
    // identity say so without building a whole metadata value.
    std::string_view getRenderTargetName(
        GlobalRenderTargetId id) const;
    RenderTargetMetadata getMetadata(GlobalRenderTargetId id) const;
    const ImageWrapper &getImage(GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    const ImageWrapper &getAttachmentImage(
        GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    // Scalar compatibility accessor. For an array render target this returns
    // an e2D view of layer 0; consumers whose ABI is an array must call
    // getLayeredImageView() instead.
    vk::ImageView getImageView(GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    vk::ImageView getImageSubresourceView(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        ImageSubresourceViewDimension dimension,
        RenderTargetRead read = {}) const;
    vk::ImageView getImageLayerView(
        GlobalRenderTargetId id, std::uint32_t array_layer,
        RenderTargetRead read = {}) const;
    vk::ImageView getAttachmentImageView(
        GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    vk::ImageView getAttachmentImageLayerView(
        GlobalRenderTargetId id, std::uint32_t array_layer,
        RenderTargetRead read = {}) const;
    vk::ImageView getAttachmentImageSubresourceView(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        ImageSubresourceViewDimension dimension,
        RenderTargetRead read = {}) const;
    // Returns one e2DArray view spanning every layer, including the
    // single-layer case. This is the descriptor view for family-array ABIs.
    vk::ImageView getLayeredImageView(
        GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    vk::ImageView getLayeredAttachmentImageView(
        GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    bool hasSeparateAttachment(GlobalRenderTargetId id) const;
    vk::SampleCountFlagBits sampleCount(GlobalRenderTargetId id) const;
    vk::ResolveModeFlagBits resolveMode(GlobalRenderTargetId id) const;
    vk::ImageLayout initialLayout(GlobalRenderTargetId id,
                                  bool attachment = false) const;
    // PS-1: the target keeps its contents across frames (lifetime:
    // persistent), so a layout tracker reset must not forget its layout.
    bool persistent(GlobalRenderTargetId id) const;
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
