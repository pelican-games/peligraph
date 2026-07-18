#pragma once

#include "../container.hpp"
#include "../material/material.hpp"
#include "../model/modeltemplate.hpp"
#include "../userpublic/animation/abi_v1.hpp"
#include "../vkcore/buf.hpp"
#include "modelinstance.hpp"
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::uint32_t morphWeightDescriptorVersionV1 = 1;
inline constexpr std::uint32_t morphCommitResetHistory = 1u << 0u;
inline constexpr std::uint32_t morphCommitDiscontinuity = 1u << 1u;

struct PublishMorphWeightFrameDescV1 {
    std::uint32_t struct_size = sizeof(PublishMorphWeightFrameDescV1);
    std::uint32_t version = morphWeightDescriptorVersionV1;
    std::uint64_t layout_generation = 0;
    std::uint64_t frame_revision = 0;
    const float *weights = nullptr;
    std::uint32_t weight_count = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct MorphWeightFrame {
    std::uint64_t instance_identity = 0;
    std::uint32_t instance_generation = 0;
    std::uint64_t layout_generation = 0;
    std::uint64_t current_revision = 0;
    std::uint64_t previous_revision = 0;
    std::vector<float> current;
    std::vector<float> previous;
};

struct alignas(16) MorphInstanceGpuData {
    std::uint32_t weight_count = 0;
    std::uint32_t layout_generation_low = 0;
    std::uint32_t layout_generation_high = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(MorphInstanceGpuData) == 16);

inline constexpr std::uint32_t materialInstanceOverrideDescriptorVersionV1 = 1;
inline constexpr std::uint32_t materialOverrideBaseColor = 1u << 0u;
inline constexpr std::uint32_t materialOverrideEmissive = 1u << 1u;
inline constexpr std::uint32_t materialOverrideUvTransform = 1u << 2u;
inline constexpr std::uint32_t materialOverrideAll =
    materialOverrideBaseColor | materialOverrideEmissive | materialOverrideUvTransform;
inline constexpr std::uint32_t materialOverrideCommitResetHistory = 1u << 0u;
inline constexpr std::uint32_t materialOverrideCommitDiscontinuity = 1u << 1u;

struct MaterialInstanceOverrideValues {
    std::uint32_t mask = 0;
    glm::vec4 base_color_factor{1.0f};
    glm::vec4 emissive_factor{1.0f};
    glm::vec2 uv_offset{0.0f};
    glm::vec2 uv_scale{1.0f};
    float uv_rotation = 0.0f;
};

struct PublishMaterialInstanceOverrideDescV1 {
    std::uint32_t struct_size = sizeof(PublishMaterialInstanceOverrideDescV1);
    std::uint32_t version = materialInstanceOverrideDescriptorVersionV1;
    Animation::InstanceHandle instance{};
    std::uint64_t frame_revision = 0;
    MaterialInstanceOverrideValues values{};
    std::uint32_t flags = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct MaterialInstanceOverrideFrame {
    std::uint64_t instance_identity = 0;
    std::uint32_t instance_generation = 0;
    std::uint64_t current_revision = 0;
    std::uint64_t previous_revision = 0;
    MaterialInstanceOverrideValues current{};
    MaterialInstanceOverrideValues previous{};
};

struct alignas(16) MaterialInstanceOverrideGpuData {
    glm::vec4 base_color_factor{0.0f};
    glm::vec4 emissive_factor{0.0f};
    glm::vec4 uv_offset_scale{0.0f};
    glm::vec4 uv_rotation_reserved{0.0f};
    glm::uvec4 metadata{0u};
};

static_assert(sizeof(MaterialInstanceOverrideGpuData) == 80);

inline constexpr std::uint32_t materialInstanceAbsoluteOverrideDescriptorVersionV2 = 2;
inline constexpr std::size_t maxMaterialInstanceAbsoluteOverrideRecords = 4096;

struct PublishMaterialInstanceAbsoluteOverrideDescV2 {
    std::uint32_t struct_size =
        sizeof(PublishMaterialInstanceAbsoluteOverrideDescV2);
    std::uint32_t version = materialInstanceAbsoluteOverrideDescriptorVersionV2;
    Animation::InstanceHandle instance{};
    std::uint64_t frame_revision = 0;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    MaterialInstanceOverrideValues values{};
    std::uint32_t flags = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct PublishVrmApplicationTransactionDescV1 {
    bool publish_morph = false;
    PublishMorphWeightFrameDescV1 morph{};
    std::span<const PublishMaterialInstanceAbsoluteOverrideDescV2>
        material_overrides{};
};

struct VrmApplicationModelView {
    ModelInstanceId model_instance{};
    Animation::InstanceHandle instance{};
    std::shared_ptr<const VrmSemanticData> semantic;
    std::shared_ptr<const MorphTargetLayout> morph_layout;
    std::shared_ptr<const SourceMaterialInitialValueTable>
        material_initial_values;
};

struct MaterialInstanceAbsoluteOverrideKey {
    std::uint64_t instance_identity = 0;
    std::uint32_t source_material_index = noSourceMaterialIndex;

    bool operator==(const MaterialInstanceAbsoluteOverrideKey &) const = default;
};

struct MaterialInstanceAbsoluteOverrideKeyHash {
    std::size_t operator()(const MaterialInstanceAbsoluteOverrideKey &key) const noexcept {
        const auto mixed = key.instance_identity ^
                           (static_cast<std::uint64_t>(key.source_material_index) << 32u);
        return std::hash<std::uint64_t>{}(mixed);
    }
};

struct MaterialInstanceAbsoluteOverrideFrame {
    std::uint64_t instance_identity = 0;
    std::uint32_t instance_generation = 0;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    std::uint64_t current_revision = 0;
    std::uint64_t previous_revision = 0;
    MaterialInstanceOverrideValues current{};
    MaterialInstanceOverrideValues previous{};
};

struct alignas(16) MaterialInstanceAbsoluteOverrideGpuHeader {
    std::uint32_t record_offset = 0;
    std::uint32_t record_count = 0;
    std::uint32_t instance_generation = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(MaterialInstanceAbsoluteOverrideGpuHeader) == 16);

struct alignas(16) MaterialInstanceAbsoluteOverrideGpuData {
    glm::vec4 base_color_factor{0.0f};
    glm::vec4 emissive_factor{0.0f};
    glm::vec4 uv_offset_scale{0.0f};
    glm::vec4 uv_rotation_reserved{0.0f};
    glm::uvec4 metadata{0u};
};

static_assert(sizeof(MaterialInstanceAbsoluteOverrideGpuData) == 80);

struct RenderCommand {
    vk::DrawIndexedIndirectCommand command;
    GlobalMaterialId material;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    std::uint32_t node_index = noSourceNodeIndex;
    bool skinned = false;
    PrimitiveViewVisibility view_visibility = PrimitiveViewVisibility::both;
};

struct DrawIndirectInfo {
    GlobalMaterialId material;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    vk::DeviceSize offset;
    uint32_t draw_count, stride;
    bool skinned = false;
};

struct ModelInstanceRebuild {
    ModelAssetId asset_id{};
    const ModelTemplate *replacement = nullptr;
};

// A fully allocated CPU candidate for one model instance. Staging performs all
// capacity checks and allocations without changing the live instance/command
// inventories. publishModelInstance() is the single no-fail publication point.
class StagedModelInstance {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit StagedModelInstance(std::unique_ptr<Impl> impl) noexcept;
    friend class PolygonInstanceContainer;

  public:
    StagedModelInstance() noexcept;
    ~StagedModelInstance();
    StagedModelInstance(StagedModelInstance &&) noexcept;
    StagedModelInstance &operator=(StagedModelInstance &&) noexcept;
    StagedModelInstance(const StagedModelInstance &) = delete;
    StagedModelInstance &operator=(const StagedModelInstance &) = delete;

    ModelInstanceId id() const noexcept;
};

DECLARE_MODULE(PolygonInstanceContainer) {
    std::vector<RenderCommand> render_commands;
    BufferWrapper indirect_buf;
    std::array<std::vector<DrawIndirectInfo>, 2> draw_calls;

    std::vector<glm::mat4> model_instances_data;
    std::vector<glm::mat4> previous_model_instances_data;
    std::vector<bool> model_history_valid;
    BufferWrapper model_data_buffer;
    BufferWrapper previous_model_data_buffer;
    vk::Device device;
    std::vector<std::vector<glm::mat4>> skin_palettes;
    std::vector<std::vector<glm::mat4>> previous_skin_palettes;
    std::vector<std::uint64_t> animation_revisions;
    std::vector<std::uint64_t> previous_animation_revisions;
    std::vector<std::uint32_t> animation_generations;
    // Slot identity is independent from animation generation and model asset
    // content revision. It advances only when a model-instance slot dies.
    std::vector<std::uint32_t> instance_generations;
    std::vector<bool> instance_alive;
    std::vector<std::uint32_t> free_instance_indices;
    std::size_t live_instance_count = 0;
    std::uint64_t scene_epoch = 1;
    std::vector<ModelAssetId> model_asset_ids;
    std::vector<std::shared_ptr<const SourceMaterialInitialValueTable>>
        material_initial_value_tables;
    std::vector<std::shared_ptr<const VrmSemanticData>> vrm_semantics;
    std::vector<std::shared_ptr<const MorphTargetLayout>> morph_layouts;
    std::vector<MorphWeightFrame> morph_weight_frames;
    std::vector<bool> morph_history_valid;
    std::uint64_t temporal_history_advance_count = 0;
    BufferWrapper skin_palette_buffer;
    BufferWrapper previous_skin_palette_buffer;
    BufferWrapper morph_instance_buffer;
    BufferWrapper morph_weight_buffer;
    BufferWrapper previous_morph_weight_buffer;
    std::unordered_map<std::uint32_t, MaterialInstanceOverrideFrame>
        material_override_frames;
    BufferWrapper material_override_buffer;
    std::unordered_map<MaterialInstanceAbsoluteOverrideKey,
                       MaterialInstanceAbsoluteOverrideFrame,
                       MaterialInstanceAbsoluteOverrideKeyHash>
        material_absolute_override_frames;
    BufferWrapper material_absolute_override_header_buffer;
    BufferWrapper material_absolute_override_record_buffer;
    vk::UniqueDescriptorSetLayout static_deformation_descriptor_layout;
    vk::UniqueDescriptorSetLayout skinned_deformation_descriptor_layout;
    vk::UniqueDescriptorSetLayout static_material_descriptor_layout;
    vk::UniqueDescriptorSetLayout skinned_material_descriptor_layout;
    vk::UniqueDescriptorPool deformation_descriptor_pool;
    vk::UniqueDescriptorSet static_deformation_descriptor_set;
    vk::UniqueDescriptorSet skinned_deformation_descriptor_set;
    vk::UniqueDescriptorSet static_material_descriptor_set;
    vk::UniqueDescriptorSet skinned_material_descriptor_set;

    bool isLive(ModelInstanceId id) const noexcept;
    std::uint32_t requireLive(ModelInstanceId id, const char *api_name) const;
    void resetSlot(std::uint32_t index);
    void uploadMaterialAbsoluteOverrides();

  public:
    struct PreparedModelTrs {
        std::uint32_t index = 0;
        glm::mat4 old_value{1.0F};
        glm::mat4 next_value{1.0F};
        bool published = false;
    };

    PolygonInstanceContainer();
    void preflightModelInstance(const ModelTemplate &model) const;
    StagedModelInstance stageModelInstance(const ModelTemplate &model);
    void publishModelInstance(StagedModelInstance staged) noexcept;
    ModelInstanceId placeModelInstance(const ModelTemplate &model);
    bool removeModelInstance(ModelInstanceId id);
    void clear();
    void triggerUpdate();
    void commitFrameHistory();
    void advanceMorphHistoryAfterRender();
    void advanceMaterialOverrideHistoryAfterRender();
    void advanceTemporalHistoryAfterRender();
    void resetTemporalHistory();
    bool canRebuildModelInstances(std::span<const ModelInstanceRebuild> replacements) const;
    void rebuildModelInstances(std::span<const ModelInstanceRebuild> replacements);
    void rebuildModelInstances(ModelAssetId asset_id, const ModelTemplate &replacement);

    PreparedModelTrs prepareTrs(ModelInstanceId id, glm::vec3 pos,
                                glm::quat rotation, glm::vec3 scale) const;
    void publishPreparedTrs(PreparedModelTrs &prepared) noexcept;
    void rollbackPreparedTrs(PreparedModelTrs &prepared) noexcept;
    void setTrs(ModelInstanceId id, glm::vec3 pos, glm::quat rotation,
                glm::vec3 scale);
    void setSkinningPalette(ModelInstanceId id, std::span<const glm::mat4> palette);
    bool isModelInstanceAlive(ModelInstanceId id) const noexcept { return isLive(id); }
    Animation::InstanceHandle animationInstance(ModelInstanceId id) const;
    Animation::Status publishAnimationFrame(ModelInstanceId id,
                                             const Animation::PublishAnimationFrameDescV1 &frame);
    Animation::Status publishMorphWeightFrame(
        ModelInstanceId id, const PublishMorphWeightFrameDescV1 &frame);
    Animation::Status publishMaterialInstanceOverride(
        ModelInstanceId id, const PublishMaterialInstanceOverrideDescV1 &frame);
    Animation::Status publishMaterialInstanceAbsoluteOverride(
        ModelInstanceId id,
        const PublishMaterialInstanceAbsoluteOverrideDescV2 &frame);
    Animation::Status publishVrmApplicationTransaction(
        ModelInstanceId id,
        const PublishVrmApplicationTransactionDescV1 &transaction);
    std::optional<VrmApplicationModelView> vrmApplicationModel(
        Animation::InstanceHandle instance) const;
    void bindDeformation(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout,
                         bool skinned) const;
    void bindMaterialInstanceResources(vk::CommandBuffer cmd_buf,
                                       vk::PipelineLayout pipeline_layout,
                                       bool skinned) const;
    void bindSkinning(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const;

    const BufferWrapper &getIndirectBuf() const;
    const BufferWrapper &getObjectBuf() const;
    const BufferWrapper &getPreviousObjectBuf() const;
    const std::vector<DrawIndirectInfo> &
    getDrawCalls(bool first_person_view = false) const;
    size_t instanceCountForTesting() const { return live_instance_count; }
    size_t slotCountForTesting() const { return model_instances_data.size(); }
    ModelInstanceId modelInstanceIdForTesting(std::uint32_t index) const;
    ModelInstanceId forceGenerationForTesting(ModelInstanceId id,
                                               std::uint32_t generation);
    void forceSceneEpochForTesting(std::uint64_t epoch);
    std::uint64_t temporalHistoryAdvanceCountForTesting() const {
        return temporal_history_advance_count;
    }
    glm::mat4 currentModelMatrixForTesting(ModelInstanceId id) const;
    glm::mat4 previousModelMatrixForTesting(ModelInstanceId id) const;
    std::uint64_t currentAnimationRevisionForTesting(ModelInstanceId id) const {
        return animation_revisions.at(requireLive(id, "currentAnimationRevisionForTesting"));
    }
    std::uint64_t previousAnimationRevisionForTesting(ModelInstanceId id) const {
        return previous_animation_revisions.at(requireLive(id, "previousAnimationRevisionForTesting"));
    }
    std::uint32_t animationGenerationForTesting(ModelInstanceId id) const {
        return animation_generations.at(requireLive(id, "animationGenerationForTesting"));
    }
    const MorphWeightFrame &morphWeightFrameForTesting(ModelInstanceId id) const {
        return morph_weight_frames.at(requireLive(id, "morphWeightFrameForTesting"));
    }
    std::uint64_t morphLayoutGenerationForTesting(ModelInstanceId id) const {
        const auto &layout = morph_layouts.at(requireLive(id, "morphLayoutGenerationForTesting"));
        return layout ? layout->generation : 0;
    }
    const MaterialInstanceOverrideFrame *materialOverrideFrameForTesting(
        ModelInstanceId id) const {
        const auto index = requireLive(id, "materialOverrideFrameForTesting");
        const auto found = material_override_frames.find(index);
        return found == material_override_frames.end() ? nullptr : &found->second;
    }
    size_t materialOverrideStorageEntryCountForTesting() const {
        return material_override_frames.size();
    }
    const MaterialInstanceAbsoluteOverrideFrame *
    materialAbsoluteOverrideFrameForTesting(
        ModelInstanceId id, std::uint32_t source_material_index) const {
        const auto index = requireLive(id, "materialAbsoluteOverrideFrameForTesting");
        const MaterialInstanceAbsoluteOverrideKey key{
            static_cast<std::uint64_t>(index) + 1, source_material_index};
        const auto found = material_absolute_override_frames.find(key);
        return found == material_absolute_override_frames.end() ? nullptr
                                                                 : &found->second;
    }
    size_t materialAbsoluteOverrideStorageEntryCountForTesting() const {
        return material_absolute_override_frames.size();
    }
    size_t instanceCountForAssetForTesting(ModelAssetId asset_id) const;
    const std::vector<RenderCommand> &renderCommandsForTesting() const {
        return render_commands;
    }
    const std::vector<glm::mat4> &
    currentSkinPaletteForTesting(ModelInstanceId id) const {
        return skin_palettes.at(requireLive(id, "currentSkinPaletteForTesting"));
    }
};

} // namespace Pelican
