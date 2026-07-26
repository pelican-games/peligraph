#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../resourcecontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../watch/assetkey.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/deferredcallback.hpp"
#include "../vkcore/image.hpp"
#include "material.hpp"
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <map>
#include <limits>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct LoadedImage;
class TextureReloadHandler;
class MaterialValuesReloadHandler;
class RenderTargetImageViewResolver;
namespace watch {
struct AssetKey;
struct ReloadRequest;
class ReloadCoordinator;
} // namespace watch

struct PushConstantStruct {
    glm::mat4 mvp;
};

static_assert(sizeof(PushConstantStruct) == PELICAN_PUSH_ENGINE_BYTES);

struct MaterialIndexPushConstant {
    uint32_t material_index = 0;
    uint32_t source_material_index = std::numeric_limits<uint32_t>::max();
};

static_assert(sizeof(MaterialIndexPushConstant) <= PELICAN_PUSH_SHADER_BYTES);

DECLARE_MODULE(MaterialContainer) {

    friend class TextureReloadHandler;
    friend class MaterialValuesReloadHandler;

    vk::Device device;
    bool split_custom_samplers = false;

    vk::UniqueSampler nearest_sampler, linear_sampler;
    vk::UniqueSampler screen_nearest_sampler, screen_linear_sampler;
    vk::UniqueDescriptorPool desc_pool;
    vk::UniqueDescriptorPool screen_input_desc_pool;

    struct InternalTextureResource {
        ImageWrapper image;
        vk::UniqueImageView linear_view;
        vk::UniqueImageView srgb_view;
    };
    struct StagedMaterialDescriptor {
        GlobalMaterialId material;
        vk::UniqueDescriptorSet descriptor;
    };
    struct RetiredTextureResources {
        InternalTextureResource texture;
        std::vector<vk::UniqueDescriptorSet> descriptors;
    };
    ResourceContainer<GlobalTextureId, InternalTextureResource> textures;

    struct InternalMaterialInfo {
        struct TextureBinding {
            GlobalTextureId texture;
            uint32_t binding = 0;
            vk::DescriptorType descriptor_type = vk::DescriptorType::eCombinedImageSampler;
            vk::Sampler sampler;
            bool srgb = false;
        };

        struct ScreenInputResource {
            MaterialScreenInputContract contract;
            GlobalRenderTargetId target = noRenderTargetId();
            bool history = false;
            PassInputViewDimension view_dimension =
                PassInputViewDimension::shared_2d;
        };
        struct ScreenInputDescriptor {
            struct DescriptorVariant {
                std::array<vk::UniqueDescriptorSet, 2> descsets;
                std::array<std::vector<vk::ImageView>, 2>
                    bound_image_views;
            };
            std::vector<DescriptorVariant> variants;
            std::vector<ScreenInputResource> resources;
            std::uint64_t binding_revision = 0;
        };

        PipelineHandle pipeline;
        MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
        MaterialShaderContract shader_contract = MaterialShaderContract::gbuffer_v1;
        std::optional<std::string> exact_pass;
        std::vector<MaterialPassInputContract> pass_inputs;
        GlobalTextureId base_color_texture;
        GlobalTextureId metallic_roughness_texture;
        GlobalTextureId normal_texture;
        GlobalTextureId emissive_texture;
        std::optional<MaterialInfo::VatPlaybackInfo> vat;
        std::vector<TextureBinding> texture_bindings;
        Std140Layout custom_values_layout;
        std::vector<std::byte> custom_values;
        mutable std::uint64_t descriptor_revision = 0;
        vk::UniqueDescriptorSet descset;
        mutable std::unordered_map<std::string, ScreenInputDescriptor>
            screen_input_descriptors;
    };
    std::unordered_map<std::string, PipelineHandle> pipelines;
    std::optional<PipelineHandle> default_pipeline;
    ResourceContainer<GlobalMaterialId, InternalMaterialInfo> materials;
    std::unordered_map<GlobalTextureId,
                       std::unordered_set<GlobalMaterialId, GlobalMaterialId::Hash>,
                       GlobalTextureId::Hash>
        texture_materials;
    BufferWrapper material_buffer;
    std::unique_ptr<TextureReloadHandler> texture_reload_handler;
    std::unique_ptr<MaterialValuesReloadHandler> material_values_reload_handler;
    mutable std::uint64_t next_screen_input_binding_revision = 1;
    // Declared last and explicitly closed at destructor entry. Deferred slot
    // callbacks pin this state while using the owner.
    DeferredCallbackLifetime deferred_callbacks;

    InternalTextureResource createTextureResource(const LoadedImage &image,
                                                  std::string_view name) const;
    bool textureShapeMatches(GlobalTextureId texture, const LoadedImage &image) const;
    void validateTextureReload(GlobalTextureId texture, const LoadedImage &image) const;
    void uploadTextureInPlace(GlobalTextureId texture, const LoadedImage &image) const;
    std::vector<StagedMaterialDescriptor>
    stageTextureRebind(GlobalTextureId texture,
                       const InternalTextureResource &replacement) const;
    RetiredTextureResources
    commitTextureRebind(GlobalTextureId texture, InternalTextureResource replacement,
                        std::vector<StagedMaterialDescriptor> descriptors);
    bool materialValuesLayoutMatches(GlobalMaterialId material,
                                     const Std140Layout &layout) const;
    InternalMaterialInfo::ScreenInputDescriptor buildScreenInputDescriptor(
        PipelineHandle pipeline,
        std::vector<InternalMaterialInfo::ScreenInputResource> resources,
        const RenderTargetImageViewResolver &rt_views) const;
    const InternalMaterialInfo::ScreenInputDescriptor *ensureScreenInputDescriptor(
        GlobalMaterialId material, const PassDefinition &pass) const;

  public:
    MaterialContainer();
    ~MaterialContainer();

    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data);
    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data, vk::Format format,
                                    vk::DeviceSize bytes_num);
    GlobalTextureId registerTexture(const LoadedImage &image, std::string_view name);
    GlobalTextureId registerTextureFile(const std::filesystem::path &path);
    // Explicit opt-in keeps the existing registerTexture* semantics intact.
    // The logical key is stable even when a mounted store changes location.
    GlobalTextureId registerReloadableTextureFile(const watch::AssetKey &key,
                                                  const std::filesystem::path &path);
    GlobalMaterialId registerMaterial(MaterialInfo info);
    // Model generations own all IDs returned by glTF commit. A failed
    // candidate releases immediately; a published generation moves Vulkan
    // objects to DeletionQueue and delays material-slot reuse until the
    // in-flight window has passed.
    void releaseModelResources(std::vector<GlobalMaterialId> material_ids,
                               std::vector<GlobalTextureId> texture_ids,
                               bool deferred) noexcept;
    struct ReloadableMaterialValuesBinding {
        std::string name;
        GlobalMaterialId material;
    };
    // Explicit opt-in preserves registerMaterial(). One document may bind
    // several named materials; each receives its own logical values resource.
    void registerReloadableMaterialValuesFile(
        const watch::AssetKey &key, const std::filesystem::path &path,
        MaterialSurfaceCatalog surfaces,
        std::span<const ReloadableMaterialValuesBinding> bindings);
    // Same-layout SSBO update surface used by HR1-M. It never changes the
    // logical material id, descriptor set, or pipeline.
    void updateMaterialValues(GlobalMaterialId material, const Std140Layout &layout,
                              std::span<const std::byte> values);
    void updateMaterialValues(GlobalMaterialId material,
                              std::span<const std::byte> values);
    std::pair<vk::ImageView, vk::ImageView> textureViewsForTesting(GlobalTextureId texture) const;
    std::vector<uint8_t> texturePixelsForTesting(GlobalTextureId texture) const;
    uint32_t textureMipLevelsForTesting(GlobalTextureId texture) const { return textures.get(texture).image.mip_levels; }
    size_t textureCountForTesting() const { return textures.size(); }
    size_t materialCountForTesting() const { return materials.size(); }
    size_t materialCapacityForTesting() const;
    size_t referencingMaterialCountForTesting(GlobalTextureId texture) const;
    std::uint64_t materialDescriptorRevisionForTesting(GlobalMaterialId material) const {
        return materials.get(material).descriptor_revision;
    }
    std::vector<std::byte> materialValuesForTesting(GlobalMaterialId material) const {
        return materials.get(material).custom_values;
    }
    std::vector<std::byte> materialGpuValuesForTesting(GlobalMaterialId material) const;
    std::vector<std::byte> materialGpuRecordForTesting(GlobalMaterialId material) const;
    bool handlesTextureReload(const watch::AssetKey &key) const;
    bool enqueueTextureReload(const watch::ReloadRequest &request,
                              watch::ReloadCoordinator &coordinator);
    bool retireTextureReloadPayload(std::shared_ptr<const void> payload,
                                    watch::ReloadCoordinator &coordinator) noexcept;
    bool handlesMaterialValuesReload(const watch::AssetKey &key) const;
    bool enqueueMaterialValuesReload(const watch::ReloadRequest &request,
                                     watch::ReloadCoordinator &coordinator);
    bool retireMaterialValuesReloadPayload(std::shared_ptr<const void> payload,
                                           watch::ReloadCoordinator &coordinator) noexcept;
    // Prepared together with surface shader/pipeline candidates. The returned
    // callback publishes only prevalidated material/layout payloads.
    std::function<void()> prepareSurfaceMaterialReload(
        const std::map<watch::AssetKey, SurfaceFormatDocument> &surface_documents,
        std::span<const watch::AssetKey> material_documents);

    bool isRenderRequired(const PassDefinition &pass, GlobalMaterialId material) const;
    MaterialRouteClass routeForMaterial(GlobalMaterialId material) const {
        return materials.get(material).route;
    }
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id,
                      const PassDefinition &pass, GlobalMaterialId material,
                      GlobalMaterialId prev_material_id,
                      RenderPassViewInvocation invocation = {}) const;
    void rebindScreenInputs(const RenderTargetImageViewResolver &rt_views) const;
    std::vector<vk::ImageView> boundScreenInputImageViewsForTesting(
        GlobalMaterialId material, const PassDefinition &pass) const;
    std::uint64_t screenInputBindingRevisionForTesting(
        GlobalMaterialId material, const PassDefinition &pass) const;
    vk::PipelineLayout getPipelineLayout() const;
    vk::PipelineLayout pipelineLayout(GlobalMaterialId material) const;
};

} // namespace Pelican
