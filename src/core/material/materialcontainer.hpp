#pragma once

#include "../container.hpp"
#include "../renderingpass/passinputdescriptorvariant.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "materialdescriptorbuilder.hpp"
#include "materialtexturestore.hpp"
#include "../resourcecontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../watch/assetkey.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/deferredcallback.hpp"
#include "../vkcore/image.hpp"
#include "material.hpp"
#include "../model/modelresourcerelease.hpp"
#include "../../project/materialdrawloop.hpp"
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
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class MaterialContainer;
namespace test_support {
void setMaterialReleaseFailureForTesting(MaterialContainer &, bool) noexcept;
void setTextureReleaseFailureForTesting(MaterialContainer &, bool) noexcept;
// Defined in test/materialcontainer_testaccess.hpp. No production
// translation unit includes that header.
struct MaterialContainerTestAccess;
}

struct RendererRuntimeGeneration;
struct MaterialGenerationPlanner;
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

struct MaterialRuntimeGenerationReloadPlan {
    std::vector<SurfaceShaderReloadOverride>
        shader_overrides;
    std::vector<GraphicsPipelineReloadOverride>
        pipeline_overrides;
    std::function<void()> commit;

    bool changesPipelineAbi() const noexcept {
        return !shader_overrides.empty() ||
               !pipeline_overrides.empty();
    }
};

DECLARE_MODULE(MaterialContainer) {

    friend class TextureReloadHandler;
    friend class MaterialValuesReloadHandler;
    // Owns the two live-generation entry points below; it needs the
    // material table and nothing else of this class.
    friend struct MaterialGenerationPlanner;

    vk::Device device;
    bool split_custom_samplers = false;

    // The three pieces this container was split into. Texture upload and
    // the sampler cache; the descriptor pools and the pass-input
    // descriptor construction; and (statelessly, in its own TU) the
    // render-generation planner.
    MaterialTextureStore texture_store;
    MaterialDescriptorBuilder descriptor_builder;

    using InternalTextureResource =
        MaterialTextureResource;
    struct StagedMaterialDescriptor {
        GlobalMaterialId material;
        vk::UniqueDescriptorSet descriptor;
    };
    struct RetiredTextureResources {
        InternalTextureResource texture;
        std::vector<vk::UniqueDescriptorSet> descriptors;
    };

    struct InternalMaterialInfo {
        struct TextureBinding {
            GlobalTextureId texture;
            uint32_t binding = 0;
            vk::DescriptorType descriptor_type = vk::DescriptorType::eCombinedImageSampler;
            vk::Sampler sampler;
            bool srgb = false;
            SurfaceTextureDimension expected_dimension =
                SurfaceTextureDimension::two_d;
            std::optional<ResolvedMaterialSampler>
                sampler_resolution;
        };

        using ScreenInputResource =
            MaterialScreenInputResource;
        using ScreenInputDescriptor =
            MaterialScreenInputDescriptorSet;

        PipelineHandle pipeline;
        std::uint64_t ray_generation = 0;
        std::shared_ptr<const void> ray_lease;
        std::string name;
        std::string source_asset;
        std::optional<std::uint32_t> source_material_index;
        std::string source_document;
        std::vector<vk::Format> pipeline_color_formats;
        std::optional<vk::Format> pipeline_depth_format;
        vk::SampleCountFlagBits pipeline_rasterization_samples =
            vk::SampleCountFlagBits::e1;
        GraphicsPipelineRenderingLocalReadContract
            pipeline_local_read;
        std::vector<MaterialOutputAttachmentState>
            pipeline_output_states;
        std::vector<std::string> tags;
        MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
        MaterialShaderContract shader_contract = MaterialShaderContract::gbuffer_v1;
        std::optional<MaterialOutputSchema> output_schema;
        std::optional<std::string> exact_pass;
        std::optional<MaterialDefaultRoutes> default_routes;
        using PassInput = MaterialPassInputState;
        std::vector<PassInput> pass_inputs;
        std::vector<ShaderResourceInterfaceBinding>
            resource_interface;
        std::vector<MaterialScreenInputContract>
            declared_screen_inputs;
        std::vector<SurfaceResourcePortDefinition>
            declared_resource_ports;
        SurfaceRenderState render_state;
        bool skinned = false;
        GlobalTextureId base_color_texture;
        GlobalTextureId metallic_roughness_texture;
        GlobalTextureId normal_texture;
        GlobalTextureId emissive_texture;
        GlobalTextureId occlusion_texture;
        std::string base_color_texture_identity;
        std::optional<NormalizedAlphaIr> normalized_alpha_ir;
        MaterialAlphaMode alpha_mode = MaterialAlphaMode::opaque;
        float alpha_cutoff = 0.5F;
        glm::vec4 base_color_factor{1.0f};
        glm::vec3 emissive_factor{1.0f};
        float metallic_factor = 1.0f;
        float roughness_factor = 1.0f;
        float normal_scale = 1.0f;
        float occlusion_strength = 1.0f;
        std::optional<MaterialInfo::VatPlaybackInfo> vat;
        std::vector<TextureBinding> texture_bindings;
        std::vector<std::pair<std::string,
                              ResolvedMaterialSampler>>
            custom_sampler_resolutions;
        Std140Layout custom_values_layout;
        std::vector<std::byte> custom_values;
        mutable std::uint64_t descriptor_revision = 0;
        vk::UniqueDescriptorSet descset;
        mutable std::unordered_map<std::string, ScreenInputDescriptor>
            screen_input_descriptors;
        // User-visible identity remains the parent material. These handles
        // own the independent GPU records selected by material passes.
        std::map<std::string, GlobalMaterialId> variants;
    };

    // The appearance a named variant inherits wholesale from its base.
    // Deliberately adjacent to the member list above: a new appearance
    // field on MaterialInfo has to be added here in the same review, or
    // variants silently keep the registration's own value. The base is
    // templated so the same list snapshots an InternalMaterialInfo and
    // then applies to each MaterialInfo -- registering a variant can
    // reallocate the material table, so the snapshot has to be taken by
    // value before the first registration.
    template <typename Base>
    static void inheritFromBase(
        MaterialInfo &variant, const Base &base) {
        // A hidden runtime resource must never enter a draw queue through
        // the parent's selection tags.
        variant.tags.clear();
        variant.skinned = base.skinned;
        variant.base_color_texture =
            base.base_color_texture;
        variant.metallic_roughness_texture =
            base.metallic_roughness_texture;
        variant.normal_texture = base.normal_texture;
        variant.emissive_texture =
            base.emissive_texture;
        variant.occlusion_texture =
            base.occlusion_texture;
        variant.base_color_factor =
            base.base_color_factor;
        variant.emissive_factor =
            base.emissive_factor;
        variant.metallic_factor =
            base.metallic_factor;
        variant.roughness_factor =
            base.roughness_factor;
        variant.normal_scale = base.normal_scale;
        variant.occlusion_strength =
            base.occlusion_strength;
        variant.vat = base.vat;
    }

    std::unordered_map<std::string, PipelineHandle> pipelines;
    std::optional<PipelineHandle> default_pipeline;
    ResourceContainer<GlobalMaterialId, InternalMaterialInfo> materials;
    // G-4: the material_route_default declared by the render-pipeline
    // candidate being validated or planned, while one is. Unset, the engine's
    // start-time snapshot applies.
    std::optional<MaterialRouteDefault> candidate_route_default;
    std::unordered_map<GlobalTextureId,
                       std::unordered_set<GlobalMaterialId, GlobalMaterialId::Hash>,
                       GlobalTextureId::Hash>
        texture_materials;
    BufferWrapper material_buffer;
    std::unique_ptr<TextureReloadHandler> texture_reload_handler;
    std::unique_ptr<MaterialValuesReloadHandler> material_values_reload_handler;
    mutable std::uint64_t next_screen_input_binding_revision = 1;
    std::uint64_t next_ray_material_generation = 1;
    // LP-2: set while registerMaterialVariants registers the variant
    // records, which are no draw-loop subjects of their own.
    bool registering_variant_records = false;
    bool fail_material_release_for_testing = false;
    bool fail_texture_release_for_testing = false;
    // Declared last and explicitly closed at destructor entry. Deferred slot
    // callbacks pin this state while using the owner.
    DeferredCallbackLifetime deferred_callbacks;

    friend void test_support::setMaterialReleaseFailureForTesting(
        MaterialContainer &, bool) noexcept;
    friend void test_support::setTextureReleaseFailureForTesting(
        MaterialContainer &, bool) noexcept;
    // The reads only a test performs. Declared here rather than in the
    // public section so the public header states what production calls and
    // nothing else; the struct that reaches them lives in test/.
    friend struct test_support::MaterialContainerTestAccess;

    std::pair<vk::ImageView, vk::ImageView> textureViewsForTesting(GlobalTextureId texture) const;
    std::optional<ResolvedMaterialSampler>
    materialSamplerResolutionForTesting(
        GlobalMaterialId material,
        std::string_view texture_name) const;
    std::vector<uint8_t> texturePixelsForTesting(
        GlobalTextureId texture, uint32_t mip_level = 0) const;
    size_t materialCapacityForTesting() const;
    size_t referencingMaterialCountForTesting(GlobalTextureId texture) const;
    std::optional<GlobalTextureId>
    materialTextureForTesting(GlobalMaterialId material,
                              std::uint32_t binding) const;
    std::vector<std::byte> materialGpuValuesForTesting(GlobalMaterialId material) const;
    std::vector<std::byte> materialGpuRecordForTesting(GlobalMaterialId material) const;
    std::vector<vk::ImageView> boundScreenInputImageViewsForTesting(
        GlobalMaterialId material, const PassDefinition &pass,
        std::uint32_t view_index = 0) const;
    std::uint64_t screenInputBindingRevisionForTesting(
        GlobalMaterialId material, const PassDefinition &pass) const;

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
    struct RayDeformationMaterial {
        std::optional<MaterialInfo::VatPlaybackInfo> vat;
        std::array<vk::DescriptorImageInfo, 2> images;
    };
    RayDeformationMaterial rayDeformationMaterial(GlobalMaterialId material) const;

    struct NamedMaterialVariantRegistration {
        std::string name;
        MaterialInfo material;
    };
    // Registers a complete set atomically from the caller's perspective.
    // Variant resources are implementation details retired with their base.
    void registerMaterialVariants(
        GlobalMaterialId base,
        std::vector<NamedMaterialVariantRegistration> variants);
    GlobalMaterialId materialVariantResource(
        GlobalMaterialId base, std::string_view name) const;
    // Model generations own all IDs returned by glTF commit. A failed
    // candidate releases immediately; a published generation moves Vulkan
    // objects to DeletionQueue and delays material-slot reuse until the
    // in-flight window has passed.
    [[nodiscard]] ReleaseReport releaseModelResources(
        std::vector<GlobalMaterialId> material_ids,
        std::vector<GlobalTextureId> texture_ids,
        bool deferred) noexcept;
    struct ReloadableMaterialValuesBinding {
        std::string name;
        GlobalMaterialId material;
        std::optional<std::string> variant;
    };
    // Explicit opt-in preserves registerMaterial(). One document may bind
    // several named materials; each receives its own logical values resource.
    void registerReloadableMaterialValuesFile(
        const watch::AssetKey &key, const std::filesystem::path &path,
        MaterialSurfaceCatalog surfaces,
        std::span<const ReloadableMaterialValuesBinding> bindings);
    // MT-3: the document's materials were registered again (edit_material);
    // the values of the previous registration are no longer reloaded.
    void forgetReloadableMaterialValuesFile(const watch::AssetKey &key);
    // Same-layout SSBO update surface used by HR1-M. It never changes the
    // logical material id, descriptor set, or pipeline.
    void updateMaterialValues(GlobalMaterialId material, const Std140Layout &layout,
                              std::span<const std::byte> values);
    void updateMaterialValues(GlobalMaterialId material,
                              std::span<const std::byte> values);
    // memorydiagnostics.cpp publishes these two counts in the engine's
    // memory report, so they are production readers and stay public. Every
    // other test-only read of this container is a member of
    // test_support::MaterialContainerTestAccess.
    size_t textureCountForTesting() const {
        return texture_store.textures.size();
    }
    size_t materialCountForTesting() const { return materials.size(); }
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

    // A render-pipeline candidate is still private when this runs. Rejecting
    // it therefore preserves every live material/pipeline descriptor and the
    // previously published frame-graph generation.
    void validateRuntimeGenerationCompatibility(
        const RendererRuntimeGeneration &generation) const;
    // Resolves every live material against a private render generation and
    // prepares shader/pipeline ABI replacements without mutating live state.
    // The commit callback is invoked only after all shader and Vulkan
    // pipeline candidates have been built successfully.
    MaterialRuntimeGenerationReloadPlan
    prepareRuntimeGenerationReload(
        const RendererRuntimeGeneration &generation);
    // G-4: while a render-pipeline candidate is compiled, validated and
    // planned, the two calls above judge a material that follows the default
    // by the route the candidate's material_route_default gives it. Such a
    // material whose route changes is left alone until the engine restarts
    // and lowers it again.
    class CandidateRouteDefaultScope {
        MaterialContainer &container_;
        std::optional<MaterialRouteDefault> previous_;

      public:
        CandidateRouteDefaultScope(MaterialContainer &container,
                                   MaterialRouteDefault value)
            : container_{container},
              previous_{container.candidate_route_default} {
            container_.candidate_route_default = value;
        }
        ~CandidateRouteDefaultScope() {
            container_.candidate_route_default = previous_;
        }
        CandidateRouteDefaultScope(const CandidateRouteDefaultScope &) = delete;
        CandidateRouteDefaultScope &operator=(
            const CandidateRouteDefaultScope &) = delete;
    };

    bool isRenderRequired(const PassDefinition &pass, GlobalMaterialId material) const;
    GlobalMaterialId resolveMaterialForPass(
        const PassDefinition &pass, GlobalMaterialId material) const;
    std::uint32_t materialGpuIndexForPass(
        const PassDefinition &pass, GlobalMaterialId material) const;
    const std::vector<std::string> &
    tagsForMaterial(GlobalMaterialId material) const {
        return materials.get(material).tags;
    }
    MaterialRouteClass routeForMaterial(GlobalMaterialId material) const {
        return materials.get(material).route;
    }
    const std::string &materialDocumentReference(
        GlobalMaterialId material) const {
        return materials.get(material).source_document;
    }
    const std::string &materialName(GlobalMaterialId material) const {
        return materials.get(material).name;
    }
    // RTL-1a: what a ray binding's material selector matches this material
    // by: its source document, or the model asset and glTF material index
    // its rows carry.
    struct RayBindingIdentity {
        std::string document;
        std::string asset;
        std::optional<std::uint32_t> source_material_index;
    };
    RayBindingIdentity rayBindingIdentity(GlobalMaterialId material) const {
        const auto &info = materials.get(material);
        return {info.source_document, info.source_asset,
                info.source_material_index};
    }
    std::uint64_t rayMaterialGeneration(
        GlobalMaterialId material) const {
        return materials.get(material).ray_generation;
    }
    std::shared_ptr<const void> rayMaterialLease(
        GlobalMaterialId material) const {
        return materials.get(material).ray_lease;
    }
    RayMaterialTableSource rayMaterialTableSource(
        GlobalMaterialId material, std::string canonical_key,
        std::uint64_t expected_generation,
        const std::shared_ptr<const void> &expected_lease) const;
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id,
                      const PassDefinition &pass, GlobalMaterialId material,
                      GlobalMaterialId prev_material_id,
                      RenderPassViewInvocation invocation = {}) const;
    // LP-2: what a material pass last bound, by the effective pipeline and
    // descriptor set rather than by the material that asked for them. Two
    // steps of one range can share a material and still differ in pipeline.
    struct MaterialDrawBindState {
        std::optional<PipelineHandle> pipeline;
        vk::DescriptorSet descriptor_set;
    };
    struct MaterialDrawBinding {
        vk::PipelineLayout layout;
        PipelineHandle pipeline;
        vk::DescriptorSet descriptor_set;
        bool pipeline_bound = false;
        bool descriptor_set_bound = false;
    };
    // Binds `material`'s descriptor set and screen inputs with `pipeline`
    // (nullopt: the material's own), skipping what `bound` already holds.
    // With the material's own pipeline this issues exactly what
    // bindResource() issues.
    MaterialDrawBinding bindMaterialDraw(
        vk::CommandBuffer cmd_buf, const PassDefinition &pass,
        GlobalMaterialId material,
        std::optional<PipelineHandle> pipeline,
        MaterialDrawBindState &bound,
        RenderPassViewInvocation invocation = {}) const;
    // LP-2: the registered materials a draw-loop rule is asked about: every
    // material except the records registered for named variants.
    std::vector<GlobalMaterialId> drawLoopMaterials() const;
    MaterialDrawSubject drawLoopSubject(GlobalMaterialId material) const;
    PipelineHandle materialPipeline(GlobalMaterialId material) const;
    // The base material's live pipeline description with the render state
    // overridden (blend factors, cull, depth), as makeMaterialPipelineDesc
    // would have written it for that state.
    GraphicsPipelineDesc drawOverridePipelineDesc(
        GlobalMaterialId base,
        const MaterialDrawRenderStateOverride &render_state) const;
    void rebindScreenInputs(const RenderTargetImageViewResolver &rt_views) const;
    vk::PipelineLayout getPipelineLayout() const;
    vk::PipelineLayout pipelineLayout(GlobalMaterialId material) const;
    vk::PipelineLayout pipelineLayout(const PassDefinition &pass,
                                      GlobalMaterialId material) const;
};

} // namespace Pelican
