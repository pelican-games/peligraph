#pragma once

#include "../container.hpp"
#include "../material/material.hpp"
#include "../model/modeltemplate.hpp"
#include "../model/occurrenceidentity.hpp"
#include "../userpublic/animation/abi_v1.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/accelerationstructure.hpp"
#include "modelinstance.hpp"
#include "modelinstanceslots.hpp"
#include "drawqueuebuilder.hpp"
#include "viewfamily.hpp"
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>
#include <array>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct SceneLoadPhaseTrace;
struct ResolvedSceneView;
struct RayDeformationPipeline;

struct RayScenePlanState {
    std::shared_ptr<const RayTracingProjectConfig> project;
    std::shared_ptr<const RaySceneInventorySnapshot> inventory;
    std::shared_ptr<const RaySceneDeclarationCatalog> declarations;
    std::shared_ptr<const RaySceneRowSnapshot> resolved_rows;
};

// Who published an instance's animation frame this revision. The animation
// component's clip path writes this container directly from the ECS update;
// everything else reaches the same slot through an AnimationServiceV1 source.
// Two different writers inside one frame revision are a conflict that is named,
// never a silent overwrite of one pose by the other.
enum class AnimationFrameWriterKind : std::uint32_t {
    none = 0,
    animation_component_clip = 1,
    animation_source = 2,
};

struct AnimationFrameWriter {
    AnimationFrameWriterKind kind = AnimationFrameWriterKind::none;
    // Tells two writers of the same kind apart -- the claimed source handle for
    // an animation source. One object carries at most one animation component,
    // so the clip path needs no further identity.
    std::uint64_t identity = 0;
    // Filled in by the container when it accepts a publish; a caller leaves it
    // at 0, because the revision it is publishing for is in the frame itself.
    std::uint64_t frame_revision = 0;
};

// The animation component's clip path is one writer per object, so it needs no
// identity beyond its kind.
constexpr std::uint64_t animationComponentClipWriter = 1;

// The name a single-writer conflict reports for a writer kind.
std::string_view animationFrameWriterName(AnimationFrameWriterKind kind) noexcept;

class PolygonInstanceContainer;
namespace test_support {
// WP382c fault seam. The counter it writes is a production field and the
// throw it reaches ("ray-query model rebuild generation exhausted") is
// production code in prepareModelInstanceRebuild; only the write is
// test-only. Defined in a test-only translation unit so no production TU
// carries the symbol (guarded by
// wp380c_production_has_no_fault_setter_symbol).
void setRayQueryGeometryGenerationForTesting(
    PolygonInstanceContainer &instances, std::uint64_t generation) noexcept;

// The reads and pokes only a test needs. Defined in
// test/polygoninstancecontainer_testaccess.hpp, so the container's public
// header states what production calls and nothing else. The five counters the
// editor runtime factory publishes in its preview-state JSON stay public
// below; they are production readers.
struct PolygonInstanceContainerTestAccess;
} // namespace test_support

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

struct ModelInstanceRebuild {
    ModelAssetId asset_id{};
    const ModelTemplate *replacement = nullptr;
};

// Host-published companion record for scene_draw_commands_v1. Each entry has
// the same flattened DrawQueue index as its indexed draw command. minimum.w is
// 1 when the AABB is valid; custom geometry without bounds uses 0 so GPU
// policies can keep it conservatively.
struct alignas(16) SceneDrawBoundsV1 {
    std::array<float, 4> minimum{};
    std::array<float, 4> maximum{};
};

static_assert(sizeof(SceneDrawBoundsV1) == sizeof(float) * 8);

struct SceneDrawCandidatesV1 {
    std::vector<vk::DrawIndexedIndirectCommand> commands;
    std::vector<SceneDrawBoundsV1> bounds;
    std::vector<SceneDrawSegmentV1> segments;
};

struct DrawQueueSortView {
    RenderPolicy::DrawSortLogicalViewV1 logical_view =
        RenderPolicy::DrawSortLogicalViewV1::shared;
    std::array<float, 3> origin{};
    std::array<float, 3> forward{0.0F, 0.0F, -1.0F};
};

// One canonical conversion is shared by main and secondary families so every
// public draw-sort provider receives the same validated camera snapshot.
DrawQueueSortView drawQueueSortView(
    const RenderViewParameters &view);

struct DrawQueueFramePlan {
    std::string_view opaque_provider =
        builtinStateBatchedDrawSortProvider;
    std::string_view transparent_provider =
        builtinBackToFrontDrawSortProvider;
    std::span<const DrawQueueSortView> sort_views;
    std::span<const MaterialDrawTagFilter> material_filters;
};

// The per-slot state of one model instance, as the value a publication moves
// into a slot. Staging fills it; ModelInstanceSlotTable::placeSlot() is the
// only thing that reads it.
struct ModelInstanceSlotSeed {
    glm::mat4 model_matrix{1.0F};
    std::vector<glm::mat4> skin_palette;
    std::vector<glm::mat4> animation_node_matrices;
    ModelAssetId asset_id{};
    std::uint64_t authoring_object_id = 0;
    std::shared_ptr<const SourceMaterialInitialValueTable>
        material_initial_values;
    std::shared_ptr<const VrmSemanticData> vrm_semantic;
    std::shared_ptr<const MorphTargetLayout> morph_layout;
    MorphWeightFrame morph_weight_frame;
    // MT-2: the scene object's material overrides (null: none), kept so a
    // model reload resolves them again against the new template.
    std::shared_ptr<const InstanceMaterialOverrides> material_overrides;
};

// Every per-slot array of the model-instance inventory, as one value. The same
// slot index addresses the same instance in all of them, so they are only ever
// copied, swapped and grown together: a scene candidate copies the whole table
// and publication swaps the whole table back. A new per-slot array is one
// member here, which the operations below then carry everywhere at once
// instead of asking six call sites to remember it.
struct ModelInstanceSlotTable {
    // The arrays below are parallel: index i is instance i in every one of
    // them, and instance_slots owns which of those indices are live.
    std::vector<glm::mat4> model_instances_data;
    std::vector<glm::mat4> previous_model_instances_data;
    std::vector<bool> model_history_valid;
    std::vector<std::vector<glm::mat4>> skin_palettes;
    // Evaluated model-space hierarchy, indexed by original glTF node.
    std::vector<std::vector<glm::mat4>> animation_node_matrices;
    std::vector<std::vector<glm::mat4>> previous_skin_palettes;
    std::vector<std::uint64_t> animation_revisions;
    std::vector<std::uint64_t> previous_animation_revisions;
    std::vector<std::uint32_t> animation_generations;
    RendererInternal::ModelInstanceSlots instance_slots;
    std::vector<ModelAssetId> model_asset_ids;
    std::vector<std::uint64_t> ray_authoring_object_ids;
    std::vector<std::shared_ptr<const SourceMaterialInitialValueTable>>
        material_initial_value_tables;
    std::vector<std::shared_ptr<const VrmSemanticData>> vrm_semantics;
    std::vector<std::shared_ptr<const MorphTargetLayout>> morph_layouts;
    std::vector<MorphWeightFrame> morph_weight_frames;
    std::vector<bool> morph_history_valid;
    std::vector<std::shared_ptr<const InstanceMaterialOverrides>>
        material_override_specs;

    static constexpr std::size_t arrayCount = 18;

    std::size_t slotCount() const noexcept {
        return model_instances_data.size();
    }
    // The size each array reports for itself, in declaration order. They are
    // equal by construction; a test reads them to say so out loud.
    std::array<std::size_t, arrayCount> slotCounts() const;

    void reserveSlots(std::size_t capacity);
    // Moves one staged instance into `index`, appending a slot when `index` is
    // the end and reusing the slot otherwise. A reused slot keeps the
    // generation resetSlot() bumped when its previous occupant died.
    void placeSlot(std::uint32_t index, ModelInstanceSlotSeed &&seed);
    // Returns a slot to its unoccupied shape and bumps its animation
    // generation (never to 0), so a record the previous occupant left behind
    // can never be mistaken for the current one.
    void clearSlot(std::uint32_t index);
    void swap(ModelInstanceSlotTable &other) noexcept;
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
  public:
    struct PreparedViewFamilyDraws {
        std::size_t slot_base = 0;
        std::vector<std::size_t>
            visible_draw_counts;
        // Present when a secondary family owns a transparent material pass.
        // Each entry is a complete one-view queue compiled with that view's
        // camera snapshot, so custom opaque/transparent sort providers and
        // material filters retain exactly the same contract as the main
        // family.
        std::vector<CompiledDrawQueueSet>
            view_queues;
    };

  private:
    std::vector<DrawItemSnapshot> draw_inventory;
    CompiledDrawQueueSet compiled_draw_queue;
    std::vector<MaterialDrawTagFilter>
        draw_queue_material_filters;
    std::uint64_t next_draw_declaration_ordinal = 0;
    std::uint64_t ray_query_geometry_generation = 1;
    friend void test_support::setRayQueryGeometryGenerationForTesting(
        PolygonInstanceContainer &instances,
        std::uint64_t generation) noexcept;
    friend struct test_support::PolygonInstanceContainerTestAccess;
    BufferWrapper indirect_buf;
    BufferWrapper view_family_indirect_buf;
    std::map<std::string,
             PreparedViewFamilyDraws,
             std::less<>>
        prepared_view_family_draws;

    ModelInstanceSlotTable slots;
    BufferWrapper model_data_buffer;
    BufferWrapper previous_model_data_buffer;
    vk::Device device;
    // Slot -> who published its animation frame, with the instance generation
    // the record was written for. A retired slot bumps that generation, so a
    // record left behind by the previous occupant can never be mistaken for the
    // current one and needs no separate teardown.
    struct AnimationFrameWriterRecord {
        std::uint32_t generation = 0;
        AnimationFrameWriter writer;
    };
    std::unordered_map<std::uint32_t, AnimationFrameWriterRecord> animation_frame_writers;
    std::shared_ptr<const RaySceneDeclarationCatalog> ray_scene_declarations =
        std::make_shared<const RaySceneDeclarationCatalog>();
    std::shared_ptr<const RayScenePlanState> ray_scene_plan;
    std::uint64_t temporal_history_advance_count = 0;
    BufferWrapper skin_palette_buffer;
    std::shared_ptr<RayDeformationPipeline> ray_deformation_pipeline;
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
    // VertBufContainer::morphMetadataGeneration() the sets above bind.
    std::uint64_t deformation_descriptor_metadata_generation = 0;
    std::vector<std::vector<std::uint32_t>> scene_retirement_queue;
    bool published_scene_gpu_dirty = false;

    bool isLive(ModelInstanceId id) const noexcept;
    std::uint32_t requireLive(ModelInstanceId id, const char *api_name) const;
    void resetSlot(std::uint32_t index);
    void uploadMaterialAbsoluteOverrides();
    void uploadPublishedSceneState();
    // Writes the four deformation/material sets on a fresh pool when the
    // morph metadata buffers they bind were replaced (they grow with the
    // vertex buffers); the old pool and sets retire with the frames that
    // may still read them.
    void refreshDeformationDescriptors();

  public:
    // The candidate scene: the live state a prepare phase may edit before a
    // noexcept publication swaps it in. It carries the same fields as the
    // container, so a copy here and a swap back is the whole protocol.
    struct PreparedSceneState {
        ModelInstanceSlotTable slots;
        std::vector<DrawItemSnapshot> draw_inventory;
        CompiledDrawQueueSet compiled_draw_queue;
        std::vector<MaterialDrawTagFilter> draw_queue_material_filters;
        std::uint64_t next_draw_declaration_ordinal = 0;
        std::uint64_t ray_query_geometry_generation = 1;
        std::map<std::string, PreparedViewFamilyDraws, std::less<>>
            prepared_view_family_draws;
        std::shared_ptr<const RaySceneDeclarationCatalog> ray_scene_declarations =
        std::make_shared<const RaySceneDeclarationCatalog>();
        std::shared_ptr<const RayScenePlanState> ray_scene_plan;
        std::uint64_t temporal_history_advance_count = 0;
        std::unordered_map<std::uint32_t, MaterialInstanceOverrideFrame>
            material_override_frames;
        std::unordered_map<MaterialInstanceAbsoluteOverrideKey,
                           MaterialInstanceAbsoluteOverrideFrame,
                           MaterialInstanceAbsoluteOverrideKeyHash>
            material_absolute_override_frames;
    };

    struct PreparedModelTrs {
        std::uint32_t index = 0;
        glm::mat4 old_value{1.0F};
        glm::mat4 next_value{1.0F};
        bool published = false;
    };

    PolygonInstanceContainer();
    PreparedSceneState snapshotPreparedScene() const;
    PreparedSceneState prepareSceneReplacement(
        std::vector<std::uint32_t> &retire_indices);
    void publishPreparedScene(PreparedSceneState &&prepared) noexcept;
    void publishPreparedScene(PreparedSceneState &&prepared,
                              SceneLoadPhaseTrace &phase_trace) noexcept;
    // MT-2: `material_overrides` (null: none) is the scene object's own
    // materials; a name neither the model nor the project registers leaves
    // its primitives on the model's material, with a warning.
    StagedModelInstance stagePreparedSceneModel(
        PreparedSceneState &prepared, const ModelTemplate &model,
        std::shared_ptr<const InstanceMaterialOverrides> material_overrides = nullptr);
    // MT-2: the live instance `id` of `model` takes `material_overrides` in
    // the candidate: its draw items are rebuilt with the materials they
    // resolve to, keeping the instance, its slot and its animation state.
    void setPreparedSceneModelMaterials(
        PreparedSceneState &prepared, ModelInstanceId id, const ModelTemplate &model,
        std::shared_ptr<const InstanceMaterialOverrides> material_overrides);
    void setPreparedSceneModelMatrix(StagedModelInstance &staged,
                                     const glm::mat4 &matrix) noexcept;
    void publishPreparedSceneModel(PreparedSceneState &prepared,
                                   StagedModelInstance staged) noexcept;
    void enqueueSceneRetirement(
        std::vector<std::uint32_t> retire_indices) noexcept;
    void enqueueSceneRetirement(
        std::vector<std::uint32_t> retire_indices,
        SceneLoadPhaseTrace &phase_trace) noexcept;
    void drainSceneRetirements();
    void preflightModelInstance(const ModelTemplate &model) const;
    StagedModelInstance stageModelInstance(
        const ModelTemplate &model,
        std::shared_ptr<const InstanceMaterialOverrides> material_overrides = nullptr);
    void setStagedModelMatrix(StagedModelInstance &staged,
                              const glm::mat4 &matrix) noexcept;
    void publishModelInstance(StagedModelInstance staged) noexcept;
    ModelInstanceId placeModelInstance(
        const ModelTemplate &model,
        std::shared_ptr<const InstanceMaterialOverrides> material_overrides = nullptr);
    // MT-2: the material each primitive of a live instance draws with now,
    // ordered by mesh then primitive (one entry per GLB primitive).
    struct InstancePrimitiveMaterial {
        std::uint32_t mesh = 0;
        std::uint32_t primitive = 0;
        GlobalMaterialId material;
        std::string name;
    };
    std::vector<InstancePrimitiveMaterial> instancePrimitiveMaterials(ModelInstanceId id) const;
    // MT-3: the models of the live instances that draw a primitive with one
    // of `materials` (a scene object's own material included).
    std::vector<ModelAssetId> modelsDrawingMaterials(std::span<const GlobalMaterialId> materials) const;
    // MI-1 (unregister_model): whether a live instance draws the model.
    bool hasModelInstances(ModelAssetId asset_id) const;
    bool removeModelInstance(ModelInstanceId id);
    void clear();
    void triggerUpdate();
    void triggerUpdate(const DrawQueueFramePlan &frame_plan);
    void commitFrameHistory();
    void advanceMorphHistoryAfterRender();
    void advanceMaterialOverrideHistoryAfterRender();
    void advanceTemporalHistoryAfterRender();
    void resetTemporalHistory();
    bool canRebuildModelInstances(std::span<const ModelInstanceRebuild> replacements) const;
    PreparedSceneState prepareModelInstanceRebuild(
        std::span<const ModelInstanceRebuild> replacements);
    void rebuildModelInstances(std::span<const ModelInstanceRebuild> replacements);
    void rebuildModelInstances(ModelAssetId asset_id, const ModelTemplate &replacement);
    std::vector<RayQueryGeometryInstanceSnapshot>
    rayQueryGeometryInstances(const PreparedSceneState *prepared = nullptr) const;
    nlohmann::ordered_json rayComponentWitness(std::uint64_t object,
        const PreparedSceneState *prepared = nullptr) const;
    void prepareRayScenePlan(PreparedSceneState &prepared,
        std::shared_ptr<const RayTracingProjectConfig> project,
        const ResolvedSceneView *scene = nullptr,
        std::span<const RayQueryGeometryInstanceSnapshot> procedurals = {},
        SceneLoadPhaseTrace *phase_trace = nullptr);
    std::shared_ptr<const RaySceneRowSnapshot> rayAsPlan() const noexcept {
        return ray_scene_plan ? ray_scene_plan->resolved_rows : nullptr;
    }
    std::shared_ptr<const RaySceneInventorySnapshot> rayAsInventory() const noexcept {
        return ray_scene_plan ? ray_scene_plan->inventory : nullptr;
    }
    std::shared_ptr<const RayScenePlanState> prepareFrameRayScenePlan(
        std::shared_ptr<const RayTracingProjectConfig> project,
        std::shared_ptr<const RaySceneInventorySnapshot> inventory,
        std::shared_ptr<const RaySceneDeclarationCatalog> declarations = nullptr) const;
    void publishRayScenePlan(std::shared_ptr<const RayScenePlanState> &prepared) noexcept {
        ray_scene_plan.swap(prepared);
    }
    std::vector<RayOccurrenceCatalogEntry> rayOccurrences(
        std::uint64_t authoring_object_id = 0) const;
    RayOccurrenceResolution resolveRayOccurrence(
        std::string_view occurrence_id) const;
    void setRayInstanceDeclaration(
        ModelInstanceId id, std::uint64_t authoring_object_id,
        std::optional<RayInstanceComponent> declaration);
    void setStagedAuthoringObject(StagedModelInstance &staged,
                                 std::uint64_t object) noexcept;
    std::uint64_t rayQueryGeometryGeneration() const noexcept {
        return ray_query_geometry_generation;
    }

    PreparedModelTrs prepareTrs(ModelInstanceId id, glm::vec3 pos,
                                glm::quat rotation, glm::vec3 scale) const;
    void publishPreparedTrs(PreparedModelTrs &prepared) noexcept;
    void rollbackPreparedTrs(PreparedModelTrs &prepared) noexcept;
    void setTrs(ModelInstanceId id, glm::vec3 pos, glm::quat rotation,
                glm::vec3 scale);
    void setSkinningPalette(ModelInstanceId id, std::span<const glm::mat4> palette);
    bool isModelInstanceAlive(ModelInstanceId id) const noexcept { return isLive(id); }
    std::size_t modelInstanceSlotCount() const noexcept {
        return slots.instance_slots.slotCount();
    }
    std::optional<ModelInstanceId>
    modelInstanceIdAt(std::uint32_t index) const;
    Animation::InstanceHandle animationInstance(ModelInstanceId id) const;
    // `writer` names who is publishing. A second writer on the same instance in
    // the same frame revision is Animation::Status::authority_conflict; the
    // caller that sees it names the conflict. There is no unattributed publish:
    // every caller says which writer it is.
    Animation::Status publishAnimationFrame(ModelInstanceId id, AnimationFrameWriter writer,
                                             const Animation::PublishAnimationFrameDescV1 &frame,
                                             std::span<const Animation::Matrix4fV1> model_matrices = {},
                                             std::span<const std::uint32_t> original_to_layout = {});
    // Who last published this instance's animation frame, and for which frame
    // revision. A retired or re-used slot reports no writer.
    AnimationFrameWriter animationFrameWriter(ModelInstanceId id) const;
    std::optional<glm::mat4> animationNodeMatrix(ModelInstanceId id, std::uint32_t source_node) const;
    Animation::Status publishMorphWeightFrame(
        ModelInstanceId id, const PublishMorphWeightFrameDescV1 &frame);
    std::optional<float> morphWeight(ModelInstanceId id, std::uint32_t node,
                                     std::uint32_t target) const;
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
    void recordGeometryDeformation(
        vk::CommandBuffer cmd_buf,
        std::vector<RayQueryGeometryInstanceSnapshot> &instances,
        bool for_acceleration_structures);
    void bindMaterialInstanceResources(vk::CommandBuffer cmd_buf,
                                       vk::PipelineLayout pipeline_layout,
                                       bool skinned) const;
    void bindSkinning(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const;

    const BufferWrapper &getIndirectBuf() const;
    // Builds conservative per-view indirect command regions for every
    // secondary family that fits the bounded cache. Families named in
    // locally_sorted_families compile their own one-view queue before
    // culling, while other families retain the canonical main-view order.
    // Rejected draws become Vulkan zero-instance draws. A family that exceeds
    // the cache simply uses the canonical CPU queue.
    void prepareViewFamilyDraws(
        const RenderViewFamilies &view_families,
        std::span<const std::string>
            locally_sorted_families = {});
    const BufferWrapper &
    viewFamilyIndirectBuffer() const;
    std::optional<vk::DeviceSize>
    viewFamilyDrawOffset(
        std::string_view family_id,
        std::uint32_t view_index) const;
    const std::vector<DrawIndirectInfo> &
    getViewFamilyDrawCalls(
        std::string_view family_id,
        std::uint32_t view_index,
        bool first_person_view = false,
        std::optional<MaterialPhase> phase =
            std::nullopt,
        std::optional<MaterialDrawTagFilterId>
            material_filter = std::nullopt) const;
    SceneDrawCandidatesV1
    sceneDrawCandidatesForFrameGraph() const;
    const SceneDrawSegmentV1 &
    sceneDrawSegment(std::uint32_t index) const {
        return compiled_draw_queue.sceneDrawSegment(index);
    }
    const BufferWrapper &getObjectBuf() const;
    const BufferWrapper &getPreviousObjectBuf() const;
    const std::vector<DrawIndirectInfo> &
    getDrawCalls(bool first_person_view = false,
                 std::optional<MaterialPhase> phase = std::nullopt,
                 std::uint32_t sort_view_index = 0,
                 std::optional<MaterialDrawTagFilterId>
                     material_filter = std::nullopt) const;
    const MaterialDrawFilterResolution *
    materialFilterResolution(
        MaterialDrawTagFilterId filter_id) const noexcept {
        return compiled_draw_queue
            .materialFilterResolution(filter_id);
    }
    const MaterialDrawFilterResolution *
    materialFilterResolution(
        MaterialPhase phase,
        MaterialDrawTagFilterId filter_id,
        std::uint32_t sort_view_index = 0) const noexcept {
        return compiled_draw_queue
            .materialFilterResolution(
                phase == MaterialPhase::opaque
                    ? DrawQueuePhase::opaque
                    : DrawQueuePhase::transparent,
                sort_view_index, filter_id);
    }
    // The five counters below are read by production: the editor runtime
    // factory publishes them in its preview-state JSON. Everything else a test
    // used to reach through this class now lives behind
    // test_support::PolygonInstanceContainerTestAccess.
    size_t instanceCountForTesting() const { return slots.instance_slots.liveCount(); }
    size_t slotCountForTesting() const { return slots.model_instances_data.size(); }
    std::uint64_t temporalHistoryAdvanceCountForTesting() const {
        return temporal_history_advance_count;
    }
    size_t materialOverrideStorageEntryCountForTesting() const {
        return material_override_frames.size();
    }
    size_t materialAbsoluteOverrideStorageEntryCountForTesting() const {
        return material_absolute_override_frames.size();
    }
};

} // namespace Pelican
