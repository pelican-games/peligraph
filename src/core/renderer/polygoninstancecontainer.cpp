#include "polygoninstancecontainer.hpp"
#include "../loader/sceneload.hpp"
#include "../loader/editorprojectiontransaction.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../model/occurrenceidentity.hpp"
#include "../shader/pelican_sets.hpp"
#include "../vkcore/core.hpp"
#include "../../project/viewfamilyrelation.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <cmath>
#include <glm/ext/matrix_transform.hpp>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace Pelican {

namespace {

constexpr size_t maxModelInstances = 1024;
constexpr size_t maxRenderCommands = 1024;
constexpr size_t maxDrawSortViews = 2;
constexpr size_t maxPreparedSecondaryViewFamilyViews = 32;

MaterialInstanceOverrideGpuData materialOverrideGpuData(
    const MaterialInstanceOverrideFrame &frame) {
    return {
        .base_color_factor = frame.current.base_color_factor,
        .emissive_factor = frame.current.emissive_factor,
        .uv_offset_scale = glm::vec4{frame.current.uv_offset, frame.current.uv_scale},
        .uv_rotation_reserved = glm::vec4{frame.current.uv_rotation, 0.0f, 0.0f, 0.0f},
        .metadata = glm::uvec4{
            frame.current.mask, frame.instance_generation,
            static_cast<std::uint32_t>(frame.current_revision),
            static_cast<std::uint32_t>(frame.current_revision >> 32u)},
    };
}

} // namespace

DrawQueueSortView drawQueueSortView(
    const RenderViewParameters &view) {
    if (!std::isfinite(view.camera_position.x) ||
        !std::isfinite(view.camera_position.y) ||
        !std::isfinite(view.camera_position.z)) {
        throw std::invalid_argument(
            "draw sorting requires a finite camera position");
    }
    for (glm::length_t column = 0;
         column < 4; ++column) {
        for (glm::length_t row = 0;
             row < 4; ++row) {
            if (!std::isfinite(
                    view.view[column][row])) {
                throw std::invalid_argument(
                    "draw sorting requires a finite view matrix");
            }
        }
    }
    const auto world_from_view =
        glm::inverse(view.view);
    auto forward =
        -glm::vec3{world_from_view[2]};
    const auto length = glm::length(forward);
    if (!std::isfinite(length) ||
        length <= 0.0F) {
        throw std::invalid_argument(
            "draw sorting requires a valid forward direction");
    }
    forward /= length;
    return DrawQueueSortView{
        .logical_view =
            view.first_person_view
                ? RenderPolicy::
                      DrawSortLogicalViewV1::
                          first_person
                : RenderPolicy::
                      DrawSortLogicalViewV1::
                          third_person,
        .origin = {
            view.camera_position.x,
            view.camera_position.y,
            view.camera_position.z,
        },
        .forward = {
            forward.x,
            forward.y,
            forward.z,
        },
    };
}

static BufferWrapper createIndirectBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(RenderCommand) * num,
                           vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createModelInstanceDataBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(glm::mat4) * num,
                           vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createSkinPaletteBuf(VulkanManageCore &vkcore, size_t instances) {
    return vkcore.allocBuf(sizeof(glm::mat4) * instances * maxSkinJoints,
                           vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createMorphInstanceBuf(VulkanManageCore &vkcore, size_t instances) {
    return vkcore.allocBuf(sizeof(MorphInstanceGpuData) * instances,
                           vk::BufferUsageFlagBits::eStorageBuffer |
                               vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createMorphWeightBuf(VulkanManageCore &vkcore, size_t instances) {
    return vkcore.allocBuf(sizeof(float) * instances * maxMorphWeightsPerInstance,
                           vk::BufferUsageFlagBits::eStorageBuffer |
                               vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createMaterialOverrideBuf(VulkanManageCore &vkcore,
                                               size_t instances) {
    auto buffer = vkcore.allocBuf(
        sizeof(MaterialInstanceOverrideGpuData) * instances,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc |
            vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferDevice,
        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    const std::vector<MaterialInstanceOverrideGpuData> empty(instances);
    vkcore.writeBuf(buffer, empty.data(), 0,
                    sizeof(MaterialInstanceOverrideGpuData) * empty.size());
    return buffer;
}

static BufferWrapper createMaterialAbsoluteOverrideHeaderBuf(
    VulkanManageCore &vkcore, size_t instances) {
    auto buffer = vkcore.allocBuf(
        sizeof(MaterialInstanceAbsoluteOverrideGpuHeader) * instances,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc |
            vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferDevice,
        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    const std::vector<MaterialInstanceAbsoluteOverrideGpuHeader> empty(instances);
    vkcore.writeBuf(buffer, empty.data(), 0,
                    sizeof(MaterialInstanceAbsoluteOverrideGpuHeader) * empty.size());
    return buffer;
}

static BufferWrapper createMaterialAbsoluteOverrideRecordBuf(
    VulkanManageCore &vkcore) {
    auto buffer = vkcore.allocBuf(
        sizeof(MaterialInstanceAbsoluteOverrideGpuData) *
            maxMaterialInstanceAbsoluteOverrideRecords,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc |
            vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferDevice,
        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    const std::vector<MaterialInstanceAbsoluteOverrideGpuData> empty(
        maxMaterialInstanceAbsoluteOverrideRecords);
    vkcore.writeBuf(buffer, empty.data(), 0,
                    sizeof(MaterialInstanceAbsoluteOverrideGpuData) * empty.size());
    return buffer;
}

static vk::UniqueDescriptorSetLayout createDeformationLayout(vk::Device device,
                                                              bool skinned,
                                                              bool material) {
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    bindings.reserve((skinned ? 7 : 5) + (material ? 3 : 0));
    if (skinned) {
        bindings.push_back({PELICAN_SKIN_PALETTE_BINDING,
                            vk::DescriptorType::eStorageBuffer, 1,
                            vk::ShaderStageFlagBits::eVertex});
        bindings.push_back({PELICAN_PREVIOUS_SKIN_PALETTE_BINDING,
                            vk::DescriptorType::eStorageBuffer, 1,
                            vk::ShaderStageFlagBits::eVertex});
    }
    for (const auto binding : {
             PELICAN_MORPH_INSTANCE_BINDING,
             PELICAN_MORPH_WEIGHT_BINDING,
             PELICAN_PREVIOUS_MORPH_WEIGHT_BINDING,
             PELICAN_MORPH_METADATA_BINDING,
             PELICAN_MORPH_DELTA_BINDING,
         }) {
        bindings.push_back({binding, vk::DescriptorType::eStorageBuffer, 1,
                            vk::ShaderStageFlagBits::eVertex});
    }
    if (material) {
        bindings.push_back({PELICAN_MATERIAL_INSTANCE_OVERRIDE_BINDING,
                            vk::DescriptorType::eStorageBuffer, 1,
                            vk::ShaderStageFlagBits::eFragment});
        bindings.push_back({PELICAN_MATERIAL_INSTANCE_ABSOLUTE_HEADER_BINDING,
                            vk::DescriptorType::eStorageBuffer, 1,
                            vk::ShaderStageFlagBits::eVertex |
                                vk::ShaderStageFlagBits::eFragment});
        bindings.push_back({PELICAN_MATERIAL_INSTANCE_ABSOLUTE_RECORD_BINDING,
                            vk::DescriptorType::eStorageBuffer, 1,
                            vk::ShaderStageFlagBits::eVertex |
                                vk::ShaderStageFlagBits::eFragment});
    }
    vk::DescriptorSetLayoutCreateInfo info;
    info.setBindings(bindings);
    return device.createDescriptorSetLayoutUnique(info);
}

static vk::UniqueDescriptorPool createDeformationPool(vk::Device device) {
    vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, 30};
    vk::DescriptorPoolCreateInfo info;
    info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    info.maxSets = 4;
    info.setPoolSizes(size);
    return device.createDescriptorPoolUnique(info);
}

PolygonInstanceContainer::PolygonInstanceContainer()
    : indirect_buf{
          createIndirectBuf(GET_MODULE(VulkanManageCore),
                            maxRenderCommands * maxDrawSortViews),
      },
      view_family_indirect_buf{
          createIndirectBuf(
              GET_MODULE(VulkanManageCore),
              maxRenderCommands *
                  maxPreparedSecondaryViewFamilyViews),
      },
      model_data_buffer{
          createModelInstanceDataBuf(GET_MODULE(VulkanManageCore), maxModelInstances),
      }, previous_model_data_buffer{
          createModelInstanceDataBuf(GET_MODULE(VulkanManageCore), maxModelInstances),
      }, device{GET_MODULE(VulkanManageCore).getDevice()},
      skin_palette_buffer{createSkinPaletteBuf(GET_MODULE(VulkanManageCore), maxModelInstances)},
      previous_skin_palette_buffer{createSkinPaletteBuf(GET_MODULE(VulkanManageCore), maxModelInstances)},
      morph_instance_buffer{createMorphInstanceBuf(GET_MODULE(VulkanManageCore), maxModelInstances)},
      morph_weight_buffer{createMorphWeightBuf(GET_MODULE(VulkanManageCore), maxModelInstances)},
      previous_morph_weight_buffer{createMorphWeightBuf(GET_MODULE(VulkanManageCore), maxModelInstances)},
      material_override_buffer{createMaterialOverrideBuf(GET_MODULE(VulkanManageCore),
                                                         maxModelInstances)},
      material_absolute_override_header_buffer{
          createMaterialAbsoluteOverrideHeaderBuf(GET_MODULE(VulkanManageCore),
                                                  maxModelInstances)},
      material_absolute_override_record_buffer{
          createMaterialAbsoluteOverrideRecordBuf(GET_MODULE(VulkanManageCore))},
      static_deformation_descriptor_layout{createDeformationLayout(device, false, false)},
      skinned_deformation_descriptor_layout{createDeformationLayout(device, true, false)},
      static_material_descriptor_layout{createDeformationLayout(device, false, true)},
      skinned_material_descriptor_layout{createDeformationLayout(device, true, true)},
      deformation_descriptor_pool{createDeformationPool(device)} {
    vk::DescriptorSetAllocateInfo allocate;
    allocate.descriptorPool = deformation_descriptor_pool.get();
    const std::array layouts{static_deformation_descriptor_layout.get(),
                             skinned_deformation_descriptor_layout.get(),
                             static_material_descriptor_layout.get(),
                             skinned_material_descriptor_layout.get()};
    allocate.setSetLayouts(layouts);
    auto sets = device.allocateDescriptorSetsUnique(allocate);
    static_deformation_descriptor_set = std::move(sets[0]);
    skinned_deformation_descriptor_set = std::move(sets[1]);
    static_material_descriptor_set = std::move(sets[2]);
    skinned_material_descriptor_set = std::move(sets[3]);

    auto &geometry = GET_MODULE(VertBufContainer);
    const auto update = [&](vk::DescriptorSet set, bool skinned, bool material) {
        std::vector<vk::DescriptorBufferInfo> infos;
        std::vector<vk::WriteDescriptorSet> writes;
        infos.reserve((skinned ? 7 : 5) + (material ? 3 : 0));
        writes.reserve((skinned ? 7 : 5) + (material ? 3 : 0));
        const auto append = [&](uint32_t binding, const BufferWrapper &buffer) {
            infos.push_back({buffer.buffer.get(), 0, vk::WholeSize});
            writes.push_back({set, binding, 0, 1,
                              vk::DescriptorType::eStorageBuffer});
            writes.back().setBufferInfo(infos.back());
        };
        if (skinned) {
            append(PELICAN_SKIN_PALETTE_BINDING, skin_palette_buffer);
            append(PELICAN_PREVIOUS_SKIN_PALETTE_BINDING,
                   previous_skin_palette_buffer);
        }
        append(PELICAN_MORPH_INSTANCE_BINDING, morph_instance_buffer);
        append(PELICAN_MORPH_WEIGHT_BINDING, morph_weight_buffer);
        append(PELICAN_PREVIOUS_MORPH_WEIGHT_BINDING,
               previous_morph_weight_buffer);
        append(PELICAN_MORPH_METADATA_BINDING,
               geometry.morphMetadataBuffer(skinned));
        append(PELICAN_MORPH_DELTA_BINDING, geometry.morphDeltaBuffer());
        if (material) {
            append(PELICAN_MATERIAL_INSTANCE_OVERRIDE_BINDING,
                   material_override_buffer);
            append(PELICAN_MATERIAL_INSTANCE_ABSOLUTE_HEADER_BINDING,
                   material_absolute_override_header_buffer);
            append(PELICAN_MATERIAL_INSTANCE_ABSOLUTE_RECORD_BINDING,
                   material_absolute_override_record_buffer);
        }
        device.updateDescriptorSets(writes, {});
    };
    update(static_deformation_descriptor_set.get(), false, false);
    update(skinned_deformation_descriptor_set.get(), true, false);
    update(static_material_descriptor_set.get(), false, true);
    update(skinned_material_descriptor_set.get(), true, true);
}

namespace {
size_t primitiveCount(const ModelTemplate &model) {
    size_t result = 0;
    for (const auto &material : model.material_primitives) result += material.primitives.size();
    return result;
}

DrawItemSnapshot makeDrawItemSnapshot(
    ModelInstanceId instance,
    const ModelTemplate::MaterialPrimitives &material,
    const ModelTemplate::PrimitiveRefInfo &primitive,
    std::string_view material_asset_reference,
    std::string_view material_document_reference,
    MaterialRouteClass route,
    std::uint64_t declaration_ordinal = 0) {
    if (!primitive.bounds_source) {
        throw std::runtime_error(
            "model primitive is missing the draw bounds source");
    }
    const auto &materials = GET_MODULE(MaterialContainer);
    const auto material_generation =
        materials.rayMaterialGeneration(material.material);
    const auto material_lease =
        materials.rayMaterialLease(material.material);
    return DrawItemSnapshot{
        .stable_identity =
            DrawStableIdentity{
                .instance = instance,
                .mesh_index = primitive.mesh_index,
                .primitive_index = primitive.primitive_index,
                .node_index = primitive.node_index,
                .occurrence = primitive.occurrence,
            },
        .declaration_ordinal = declaration_ordinal,
        .indexed =
            DrawIndexedArguments{
                .index_count = primitive.index_count,
                .instance_count = 1,
                .first_index = primitive.index_offset,
                .vertex_offset = primitive.vert_offset,
                .first_instance = instance.index,
            },
        .pipeline_material_key =
            DrawPipelineMaterialKey{
                .material = material.material,
                .source_material_index = material.source_material_index.value_or(
                    noSourceMaterialIndex),
                .skinned = primitive.skinned,
            },
        .material_asset_reference =
            std::string{material_asset_reference},
        .material_document_reference =
            std::string{material_document_reference},
        .material_name = materials.materialName(material.material),
        .ray_material_generation = material_generation,
        .ray_material_lease = material_lease,
        .route = route,
        .phase = drawPhaseForMaterialRoute(route),
        .bounds_source = primitive.bounds_source,
        .world_bounds = std::nullopt,
        .view_mask = drawViewMask(primitive.view_visibility),
        .vertex_count = primitive.vertex_count,
        .morph_deformed = primitive.morph_deformed,
        .vat_deformed = primitive.vat_deformed,
        .geometry_allocation_id =
            primitive.geometry_allocation_id,
        .ray_attribute_presence =
            primitive.ray_attribute_presence,
    };
}
} // namespace

struct StagedModelInstance::Impl {
    ModelInstanceId id{};
    // Everything the slot table takes on publication, already allocated.
    ModelInstanceSlotSeed seed;
    std::vector<DrawItemSnapshot> draw_items;
};

StagedModelInstance::StagedModelInstance() noexcept = default;
StagedModelInstance::StagedModelInstance(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}
StagedModelInstance::~StagedModelInstance() = default;
StagedModelInstance::StagedModelInstance(StagedModelInstance &&) noexcept = default;
StagedModelInstance &StagedModelInstance::operator=(StagedModelInstance &&) noexcept = default;

ModelInstanceId StagedModelInstance::id() const noexcept {
    return impl_ ? impl_->id : ModelInstanceId{};
}

std::array<std::size_t, ModelInstanceSlotTable::arrayCount>
ModelInstanceSlotTable::slotCounts() const {
    return {
        model_instances_data.size(),
        previous_model_instances_data.size(),
        model_history_valid.size(),
        skin_palettes.size(),
        animation_node_matrices.size(),
        previous_skin_palettes.size(),
        animation_revisions.size(),
        previous_animation_revisions.size(),
        animation_generations.size(),
        instance_slots.slotCount(),
        model_asset_ids.size(),
        ray_authoring_object_ids.size(),
        material_initial_value_tables.size(),
        vrm_semantics.size(),
        morph_layouts.size(),
        morph_weight_frames.size(),
        morph_history_valid.size(),
    };
}

void ModelInstanceSlotTable::reserveSlots(std::size_t capacity) {
    model_instances_data.reserve(capacity);
    previous_model_instances_data.reserve(capacity);
    model_history_valid.reserve(capacity);
    skin_palettes.reserve(capacity);
    animation_node_matrices.reserve(capacity);
    previous_skin_palettes.reserve(capacity);
    animation_revisions.reserve(capacity);
    previous_animation_revisions.reserve(capacity);
    animation_generations.reserve(capacity);
    instance_slots.reserve(capacity);
    model_asset_ids.reserve(capacity);
    ray_authoring_object_ids.reserve(capacity);
    material_initial_value_tables.reserve(capacity);
    vrm_semantics.reserve(capacity);
    morph_layouts.reserve(capacity);
    morph_weight_frames.reserve(capacity);
    morph_history_valid.reserve(capacity);
}

void ModelInstanceSlotTable::placeSlot(std::uint32_t index,
                                       ModelInstanceSlotSeed &&seed) {
    if (index == model_instances_data.size()) {
        model_instances_data.push_back(seed.model_matrix);
        previous_model_instances_data.push_back(seed.model_matrix);
        model_history_valid.push_back(false);
        skin_palettes.push_back(std::move(seed.skin_palette));
        animation_node_matrices.push_back(
            std::move(seed.animation_node_matrices));
        previous_skin_palettes.emplace_back();
        animation_revisions.push_back(0);
        previous_animation_revisions.push_back(0);
        animation_generations.push_back(1);
        model_asset_ids.push_back(seed.asset_id);
        ray_authoring_object_ids.push_back(0);
        material_initial_value_tables.push_back(
            std::move(seed.material_initial_values));
        vrm_semantics.push_back(std::move(seed.vrm_semantic));
        morph_layouts.push_back(std::move(seed.morph_layout));
        morph_weight_frames.push_back(std::move(seed.morph_weight_frame));
        morph_history_valid.push_back(false);
    } else {
        // A reused slot keeps animation_generations[index]: clearSlot() bumped
        // it when the previous occupant died, and the staged morph frame was
        // built for that bumped value.
        model_instances_data[index] = seed.model_matrix;
        previous_model_instances_data[index] = seed.model_matrix;
        model_history_valid[index] = false;
        skin_palettes[index] = std::move(seed.skin_palette);
        animation_node_matrices[index] =
            std::move(seed.animation_node_matrices);
        previous_skin_palettes[index].clear();
        animation_revisions[index] = 0;
        previous_animation_revisions[index] = 0;
        model_asset_ids[index] = seed.asset_id;
        ray_authoring_object_ids[index] = 0;
        material_initial_value_tables[index] =
            std::move(seed.material_initial_values);
        vrm_semantics[index] = std::move(seed.vrm_semantic);
        morph_layouts[index] = std::move(seed.morph_layout);
        morph_weight_frames[index] = std::move(seed.morph_weight_frame);
        morph_history_valid[index] = false;
    }
    ray_authoring_object_ids[index] = seed.authoring_object_id;
}

void ModelInstanceSlotTable::clearSlot(std::uint32_t index) {
    model_instances_data[index] = glm::identity<glm::mat4>();
    previous_model_instances_data[index] = glm::identity<glm::mat4>();
    model_history_valid[index] = false;
    skin_palettes[index].clear();
    animation_node_matrices[index].clear();
    previous_skin_palettes[index].clear();
    animation_revisions[index] = 0;
    previous_animation_revisions[index] = 0;
    model_asset_ids[index] = {};
    ray_authoring_object_ids[index] = 0;
    material_initial_value_tables[index].reset();
    vrm_semantics[index].reset();
    if (++animation_generations[index] == 0) ++animation_generations[index];
    morph_layouts[index].reset();
    morph_weight_frames[index] = MorphWeightFrame{
        .instance_identity = static_cast<std::uint64_t>(index) + 1,
        .instance_generation = animation_generations[index],
    };
    morph_history_valid[index] = false;
}

void ModelInstanceSlotTable::swap(ModelInstanceSlotTable &other) noexcept {
    model_instances_data.swap(other.model_instances_data);
    previous_model_instances_data.swap(other.previous_model_instances_data);
    model_history_valid.swap(other.model_history_valid);
    skin_palettes.swap(other.skin_palettes);
    animation_node_matrices.swap(other.animation_node_matrices);
    previous_skin_palettes.swap(other.previous_skin_palettes);
    animation_revisions.swap(other.animation_revisions);
    previous_animation_revisions.swap(other.previous_animation_revisions);
    animation_generations.swap(other.animation_generations);
    std::swap(instance_slots, other.instance_slots);
    model_asset_ids.swap(other.model_asset_ids);
    ray_authoring_object_ids.swap(other.ray_authoring_object_ids);
    material_initial_value_tables.swap(other.material_initial_value_tables);
    vrm_semantics.swap(other.vrm_semantics);
    morph_layouts.swap(other.morph_layouts);
    morph_weight_frames.swap(other.morph_weight_frames);
    morph_history_valid.swap(other.morph_history_valid);
}

bool PolygonInstanceContainer::isLive(ModelInstanceId id) const noexcept {
    return slots.instance_slots.isLive(id);
}

PolygonInstanceContainer::PreparedSceneState
PolygonInstanceContainer::snapshotPreparedScene() const {
    return PreparedSceneState{
        .slots = slots,
        .draw_inventory = draw_inventory,
        .compiled_draw_queue = compiled_draw_queue,
        .draw_queue_material_filters = draw_queue_material_filters,
        .next_draw_declaration_ordinal = next_draw_declaration_ordinal,
        .ray_query_geometry_generation = ray_query_geometry_generation,
        .prepared_view_family_draws = prepared_view_family_draws,
        .ray_scene_declarations = ray_scene_declarations,
        .ray_scene_plan = ray_scene_plan,
        .temporal_history_advance_count = temporal_history_advance_count,
        .material_override_frames = material_override_frames,
        .material_absolute_override_frames =
            material_absolute_override_frames,
    };
}

PolygonInstanceContainer::PreparedSceneState
PolygonInstanceContainer::prepareSceneReplacement(
    std::vector<std::uint32_t> &retire_indices) {
    auto candidate = snapshotPreparedScene();
    candidate.slots.instance_slots =
        slots.instance_slots.prepareSceneReplacement(retire_indices);
    candidate.draw_inventory.clear();
    candidate.compiled_draw_queue = {};
    candidate.draw_queue_material_filters.clear();
    candidate.next_draw_declaration_ordinal = 0;
    candidate.prepared_view_family_draws.clear();
    candidate.material_override_frames.clear();
    candidate.material_absolute_override_frames.clear();
    candidate.temporal_history_advance_count = 0;

    // retire_old_state only moves this already allocated list into the queue.
    scene_retirement_queue.reserve(scene_retirement_queue.size() + 1U);
    return candidate;
}

void PolygonInstanceContainer::publishPreparedScene(
    PreparedSceneState &&prepared) noexcept {
    using std::swap;
    // One swap moves every per-slot array, so no slot index can be left one
    // publication behind the others.
    slots.swap(prepared.slots);
    draw_inventory.swap(prepared.draw_inventory);
    swap(compiled_draw_queue, prepared.compiled_draw_queue);
    draw_queue_material_filters.swap(prepared.draw_queue_material_filters);
    swap(next_draw_declaration_ordinal,
         prepared.next_draw_declaration_ordinal);
    swap(ray_query_geometry_generation,
         prepared.ray_query_geometry_generation);
    prepared_view_family_draws.swap(prepared.prepared_view_family_draws);
    ray_scene_declarations.swap(prepared.ray_scene_declarations);
    ray_scene_plan.swap(prepared.ray_scene_plan);
    swap(temporal_history_advance_count,
         prepared.temporal_history_advance_count);
    material_override_frames.swap(prepared.material_override_frames);
    material_absolute_override_frames.swap(
        prepared.material_absolute_override_frames);
    // Publication stays noexcept. The next draw synchronizes the final CPU
    // state, including a rollback or an animation update after publication.
    published_scene_gpu_dirty = true;
}

void PolygonInstanceContainer::publishPreparedScene(
    PreparedSceneState &&prepared,
    SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::early_publish);
    publishPreparedScene(std::move(prepared));
}

StagedModelInstance PolygonInstanceContainer::stagePreparedSceneModel(
    PreparedSceneState &prepared, const ModelTemplate &model) {
    const auto primitive_count = primitiveCount(model);
    if (!prepared.slots.instance_slots.hasReusableSlot() &&
        prepared.slots.slotCount() >= maxModelInstances) {
        throw std::runtime_error("Model instance capacity exceeded");
    }
    if (prepared.draw_inventory.size() > maxRenderCommands ||
        primitive_count > maxRenderCommands - prepared.draw_inventory.size()) {
        throw std::runtime_error("Render command capacity exceeded");
    }
    if (primitive_count >
        std::numeric_limits<std::uint64_t>::max() -
            prepared.next_draw_declaration_ordinal) {
        throw std::runtime_error("Draw declaration ordinal exhausted");
    }

    auto staged = std::make_unique<StagedModelInstance::Impl>();
    staged->id = prepared.slots.instance_slots.nextId(
        static_cast<std::uint32_t>(prepared.slots.slotCount()));
    const auto index = staged->id.index;
    const auto reuse = index != prepared.slots.slotCount();
    staged->seed.asset_id = model.asset_id;
    staged->seed.material_initial_values = model.material_initial_values;
    staged->seed.vrm_semantic = model.vrm_semantic;
    staged->seed.morph_layout = model.morph_targets;
    if (model.skeletal) {
        staged->seed.skin_palette =
            evaluateSkinPalette(*model.skeletal, nullptr, 0.0, 1.0, false,
                                0.0, &staged->seed.animation_node_matrices);
        if (staged->seed.skin_palette.size() > maxSkinJoints) {
            throw std::runtime_error("Skin palette exceeds 128 joints");
        }
    }
    const auto defaults = model.morph_targets
                              ? model.morph_targets->default_weights
                              : std::vector<float>{};
    staged->seed.morph_weight_frame = MorphWeightFrame{
        .instance_identity = static_cast<std::uint64_t>(index) + 1,
        .instance_generation =
            reuse ? prepared.slots.animation_generations[index] : 1u,
        .layout_generation =
            model.morph_targets ? model.morph_targets->generation : 0,
        .current_revision = 0,
        .previous_revision = 0,
        .current = defaults,
        .previous = defaults,
    };
    staged->draw_items.reserve(primitive_count);
    if (primitive_count != 0) {
        const auto &materials = GET_MODULE(MaterialContainer);
        for (const auto &material : model.material_primitives) {
            if (material.primitives.empty()) continue;
            const auto route = materials.routeForMaterial(material.material);
            const auto &document =
                materials.materialDocumentReference(material.material);
            for (const auto &primitive : material.primitives) {
                staged->draw_items.push_back(makeDrawItemSnapshot(
                    staged->id, material, primitive,
                    model.authored_asset_reference, document, route));
            }
        }
    }

    const auto capacity = prepared.slots.slotCount() + (reuse ? 0U : 1U);
    prepared.slots.reserveSlots(capacity);
    prepared.draw_inventory.reserve(prepared.draw_inventory.size() +
                                    staged->draw_items.size());
    if (!staged->seed.skin_palette.empty()) {
        GET_MODULE(VulkanManageCore)
            .writeBuf(skin_palette_buffer, staged->seed.skin_palette.data(),
                      sizeof(glm::mat4) * maxSkinJoints * index,
                      sizeof(glm::mat4) * staged->seed.skin_palette.size());
    }
    return StagedModelInstance{std::move(staged)};
}

void PolygonInstanceContainer::setPreparedSceneModelMatrix(
    StagedModelInstance &staged, const glm::mat4 &matrix) noexcept {
    setStagedModelMatrix(staged, matrix);
}

namespace {
// The single publication step, shared by the scene candidate and the live
// container: the slot takes the staged state, the id goes live, and the draw
// items take their declaration ordinals. Both callers reserved everything this
// touches while staging, so it cannot fail.
void publishStagedCandidate(ModelInstanceSlotTable &slots,
                            std::vector<DrawItemSnapshot> &draw_inventory,
                            std::uint64_t &next_draw_declaration_ordinal,
                            ModelInstanceId id, ModelInstanceSlotSeed &&seed,
                            std::vector<DrawItemSnapshot> &draw_items) noexcept {
    slots.placeSlot(id.index, std::move(seed));
    slots.instance_slots.publish(id);
    for (auto &item : draw_items) {
        item.declaration_ordinal = next_draw_declaration_ordinal++;
    }
    draw_inventory.insert(draw_inventory.end(),
                          std::make_move_iterator(draw_items.begin()),
                          std::make_move_iterator(draw_items.end()));
}
} // namespace

void PolygonInstanceContainer::publishPreparedSceneModel(
    PreparedSceneState &prepared, StagedModelInstance staged) noexcept {
    assert(staged.impl_ != nullptr);
    auto &candidate = *staged.impl_;
    publishStagedCandidate(prepared.slots, prepared.draw_inventory,
                           prepared.next_draw_declaration_ordinal, candidate.id,
                           std::move(candidate.seed), candidate.draw_items);
}

void PolygonInstanceContainer::enqueueSceneRetirement(
    std::vector<std::uint32_t> retire_indices) noexcept {
    assert(scene_retirement_queue.size() < scene_retirement_queue.capacity());
    scene_retirement_queue.push_back(std::move(retire_indices));
}

void PolygonInstanceContainer::enqueueSceneRetirement(
    std::vector<std::uint32_t> retire_indices,
    SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::retire);
    enqueueSceneRetirement(std::move(retire_indices));
}

void PolygonInstanceContainer::drainSceneRetirements() {
    if (scene_retirement_queue.empty()) return;
    std::vector<std::vector<std::uint32_t>> pending;
    pending.swap(scene_retirement_queue);
    bool changed = false;
    for (const auto &batch : pending) {
        for (const auto index : batch) {
            if (index >= slots.instance_slots.slotCount() ||
                slots.instance_slots.alive(index)) {
                continue;
            }
            resetSlot(index);
            slots.instance_slots.releaseWithheld(index);
            const MaterialInstanceOverrideGpuData empty{};
            GET_MODULE(VulkanManageCore).writeBuf(
                material_override_buffer, &empty,
                sizeof(MaterialInstanceOverrideGpuData) * index,
                sizeof(empty));
            changed = true;
        }
    }
    if (changed) uploadMaterialAbsoluteOverrides();
}

std::uint32_t PolygonInstanceContainer::requireLive(ModelInstanceId id,
                                                    const char *api_name) const {
    return slots.instance_slots.requireLive(id, api_name);
}

void PolygonInstanceContainer::preflightModelInstance(const ModelTemplate &model) const {
    if (!slots.instance_slots.hasReusableSlot() &&
        slots.slotCount() >= maxModelInstances) {
        throw std::runtime_error("Model instance capacity exceeded");
    }

    size_t primitive_count = 0;
    for (const auto &material : model.material_primitives) {
        primitive_count += material.primitives.size();
    }
    if (draw_inventory.size() > maxRenderCommands ||
        primitive_count > maxRenderCommands - draw_inventory.size()) {
        throw std::runtime_error("Render command capacity exceeded");
    }
    if (primitive_count >
        std::numeric_limits<std::uint64_t>::max() -
            next_draw_declaration_ordinal) {
        throw std::runtime_error("Draw declaration ordinal exhausted");
    }
}

StagedModelInstance PolygonInstanceContainer::stageModelInstance(const ModelTemplate &model) {
    preflightModelInstance(model);

    auto staged = std::make_unique<StagedModelInstance::Impl>();
    staged->id = slots.instance_slots.nextId(
        static_cast<std::uint32_t>(slots.slotCount()));
    const auto index = staged->id.index;
    const auto reuse = index != slots.slotCount();
    staged->seed.asset_id = model.asset_id;
    staged->seed.material_initial_values = model.material_initial_values;
    staged->seed.vrm_semantic = model.vrm_semantic;
    staged->seed.morph_layout = model.morph_targets;

    if (model.skeletal) {
        staged->seed.skin_palette =
            evaluateSkinPalette(*model.skeletal, nullptr, 0.0, 1.0, false, 0.0,
                                &staged->seed.animation_node_matrices);
        if (staged->seed.skin_palette.size() > maxSkinJoints)
            throw std::runtime_error("Skin palette exceeds 128 joints");
    }

    const auto defaults = model.morph_targets
                              ? model.morph_targets->default_weights
                              : std::vector<float>{};
    staged->seed.morph_weight_frame = MorphWeightFrame{
        .instance_identity = static_cast<std::uint64_t>(staged->id.index) + 1,
        .instance_generation = reuse ? slots.animation_generations[index] : 1u,
        .layout_generation = model.morph_targets ? model.morph_targets->generation : 0,
        .current_revision = 0,
        .previous_revision = 0,
        .current = defaults,
        .previous = defaults,
    };

    const auto draw_item_count = primitiveCount(model);
    staged->draw_items.reserve(draw_item_count);
    if (draw_item_count != 0) {
        const auto &materials = GET_MODULE(MaterialContainer);
        for (const auto &material : model.material_primitives) {
            if (material.primitives.empty()) continue;
            const auto route = materials.routeForMaterial(material.material);
            const auto &document =
                materials.materialDocumentReference(material.material);
            for (const auto &primitive : material.primitives) {
                staged->draw_items.push_back(makeDrawItemSnapshot(
                    staged->id, material, primitive,
                    model.authored_asset_reference, document, route));
            }
        }
    }

    // Reserve every live container before publication. Once this succeeds,
    // publishModelInstance() only moves/copies into already allocated storage.
    const auto instance_capacity = slots.slotCount() + (reuse ? 0u : 1u);
    slots.reserveSlots(instance_capacity);
    draw_inventory.reserve(draw_inventory.size() + staged->draw_items.size());

    // The future slot is not addressable until publication, so a failed write
    // cannot change the live inventory or IDs.
    if (!staged->seed.skin_palette.empty()) {
        GET_MODULE(VulkanManageCore)
            .writeBuf(skin_palette_buffer, staged->seed.skin_palette.data(),
                      sizeof(glm::mat4) * maxSkinJoints * staged->id.index,
                      sizeof(glm::mat4) * staged->seed.skin_palette.size());
    }

    return StagedModelInstance{std::move(staged)};
}

void PolygonInstanceContainer::setStagedModelMatrix(
    StagedModelInstance &staged, const glm::mat4 &matrix) noexcept {
    assert(staged.impl_ != nullptr);
    staged.impl_->seed.model_matrix = matrix;
}

void PolygonInstanceContainer::publishModelInstance(StagedModelInstance staged) noexcept {
    assert(staged.impl_ != nullptr);
    assert(draw_inventory.size() + staged.impl_->draw_items.size() <=
           draw_inventory.capacity());

    auto &candidate = *staged.impl_;
    const auto index = candidate.id.index;
    assert(candidate.id.scene_epoch == slots.instance_slots.sceneEpoch());
    if (index == slots.slotCount()) {
        assert(slots.slotCount() < slots.model_instances_data.capacity());
    } else {
        assert(!slots.instance_slots.alive(index));
    }
    publishStagedCandidate(slots, draw_inventory, next_draw_declaration_ordinal,
                           candidate.id, std::move(candidate.seed),
                           candidate.draw_items);
}

ModelInstanceId PolygonInstanceContainer::placeModelInstance(const ModelTemplate &model) {
    auto staged = stageModelInstance(model);
    const auto id = staged.id();
    publishModelInstance(std::move(staged));
    return id;
}
void PolygonInstanceContainer::resetSlot(std::uint32_t index) {
    slots.clearSlot(index);
    material_override_frames.erase(index);
    const auto identity = static_cast<std::uint64_t>(index) + 1;
    std::erase_if(material_absolute_override_frames,
                  [identity](const auto &entry) {
                      return entry.first.instance_identity == identity;
                  });
}

bool PolygonInstanceContainer::removeModelInstance(ModelInstanceId id) {
    if (!isLive(id)) return false;

    const auto index = id.index;
    std::erase_if(draw_inventory, [index](const DrawItemSnapshot &item) {
        return item.stable_identity.instance.index == index;
    });
    resetSlot(index);
    slots.instance_slots.retire(id);
    const MaterialInstanceOverrideGpuData empty{};
    GET_MODULE(VulkanManageCore).writeBuf(
        material_override_buffer, &empty,
        sizeof(MaterialInstanceOverrideGpuData) * index, sizeof(empty));
    uploadMaterialAbsoluteOverrides();
    return true;
}

void PolygonInstanceContainer::clear() {
    auto empty_declarations = std::make_shared<const RaySceneDeclarationCatalog>();
    slots.instance_slots.prepareClear();
    draw_inventory.clear();
    compiled_draw_queue = {};
    next_draw_declaration_ordinal = 0;
    for (std::uint32_t index = 0; index < slots.instance_slots.slotCount(); ++index) {
        if (slots.instance_slots.alive(index)) {
            resetSlot(index);
        }
    }
    slots.instance_slots.clearPrepared();
    ray_scene_plan.reset();
    ray_scene_declarations = std::move(empty_declarations);
    material_override_frames.clear();
    material_absolute_override_frames.clear();
    const std::vector<MaterialInstanceOverrideGpuData> empty(maxModelInstances);
    GET_MODULE(VulkanManageCore).writeBuf(material_override_buffer, empty.data(), 0,
                                          sizeof(MaterialInstanceOverrideGpuData) *
                                              empty.size());
    uploadMaterialAbsoluteOverrides();
}

void PolygonInstanceContainer::triggerUpdate() {
    drainSceneRetirements();
    constexpr std::array default_views{
        DrawQueueSortView{},
    };
    triggerUpdate(DrawQueueFramePlan{.sort_views = default_views});
}

void PolygonInstanceContainer::triggerUpdate(
    const DrawQueueFramePlan &frame_plan) {
    drainSceneRetirements();
    uploadPublishedSceneState();
    // Clear the published view before compiling the next immutable queue.
    compiled_draw_queue = {};
    prepared_view_family_draws.clear();
    draw_queue_material_filters.assign(
        frame_plan.material_filters.begin(),
        frame_plan.material_filters.end());

    if (frame_plan.opaque_provider.empty() ||
        frame_plan.transparent_provider.empty()) {
        throw std::invalid_argument(
            "draw queue frame plan requires non-empty provider names");
    }
    if (frame_plan.sort_views.empty() ||
        frame_plan.sort_views.size() > maxDrawSortViews) {
        throw std::invalid_argument(
            "draw queue frame plan requires one or two sort views");
    }
    if (draw_inventory.empty()) {
        compiled_draw_queue =
            CompiledDrawQueueSet::makeEmpty(
                frame_plan.material_filters,
                static_cast<std::uint32_t>(
                    frame_plan.sort_views.size()));
        return;
    }

    for (size_t i = 0; i < slots.model_instances_data.size(); ++i) {
        if (!slots.model_history_valid[i]) {
            slots.previous_model_instances_data[i] = slots.model_instances_data[i];
            slots.previous_skin_palettes[i] = slots.skin_palettes[i];
        }
        if (!slots.morph_history_valid[i]) {
            slots.morph_weight_frames[i].previous = slots.morph_weight_frames[i].current;
            slots.morph_weight_frames[i].previous_revision =
                slots.morph_weight_frames[i].current_revision;
        }
        if (!slots.previous_skin_palettes[i].empty()) {
            GET_MODULE(VulkanManageCore)
                .writeBuf(previous_skin_palette_buffer, slots.previous_skin_palettes[i].data(),
                          sizeof(glm::mat4) * maxSkinJoints * i,
                          sizeof(glm::mat4) * slots.previous_skin_palettes[i].size());
        }
    }

    std::vector<MorphInstanceGpuData> morph_instances(slots.model_instances_data.size());
    for (size_t i = 0; i < slots.morph_weight_frames.size(); ++i) {
        const auto &frame = slots.morph_weight_frames[i];
        const auto generation = frame.layout_generation;
        morph_instances[i] = {
            static_cast<std::uint32_t>(frame.current.size()),
            static_cast<std::uint32_t>(generation),
            static_cast<std::uint32_t>(generation >> 32u),
            0,
        };
        if (!frame.current.empty()) {
            GET_MODULE(VulkanManageCore).writeBuf(
                morph_weight_buffer, frame.current.data(),
                sizeof(float) * maxMorphWeightsPerInstance * i,
                sizeof(float) * frame.current.size());
            GET_MODULE(VulkanManageCore).writeBuf(
                previous_morph_weight_buffer, frame.previous.data(),
                sizeof(float) * maxMorphWeightsPerInstance * i,
                sizeof(float) * frame.previous.size());
        }
    }
    if (!morph_instances.empty()) {
        GET_MODULE(VulkanManageCore).writeBuf(
            morph_instance_buffer, morph_instances.data(), 0,
            sizeof(MorphInstanceGpuData) * morph_instances.size());
    }

    // Resolve the frame-varying world bounds into a temporary immutable
    // snapshot. The declaration inventory and its object-space sources stay
    // unchanged across builds.
    auto frame_items = draw_inventory;
    const auto &materials = GET_MODULE(MaterialContainer);
    for (auto &item : frame_items) {
        const auto instance = item.stable_identity.instance.index;
        if (!slots.instance_slots.alive(instance) ||
            instance >= slots.model_instances_data.size() || !item.bounds_source) {
            throw std::runtime_error(
                "draw inventory contains a stale instance or missing bounds source");
        }
        const auto weights = instance < slots.morph_weight_frames.size()
                                 ? std::span<const float>{
                                       slots.morph_weight_frames[instance].current}
                                 : std::span<const float>{};
        std::span<const glm::mat4> palette;
        if (item.pipeline_material_key.skinned) {
            if (instance >= slots.skin_palettes.size() ||
                slots.skin_palettes[instance].empty()) {
                throw std::runtime_error(
                    "skinned draw inventory item is missing its current skin palette");
            }
            palette = slots.skin_palettes[instance];
        }
        item.world_bounds = resolveDrawWorldBounds(
            *item.bounds_source, slots.model_instances_data[instance], palette,
            weights);
        item.material_tags =
            materials.tagsForMaterial(
                item.pipeline_material_key.material);
    }

    // Compile phase/view variants without changing the live inventory. The
    // set flattens them back into one indirect buffer after rebasing ranges.
    const auto max_draw_indirect_count = GET_MODULE(VulkanManageCore)
                                             .getPhysDevice()
                                             .getProperties()
                                             .limits.maxDrawIndirectCount;
    const auto opaque_provider =
        renderPolicyRegistry().resolveDrawSortProvider(
            frame_plan.opaque_provider);
    const auto transparent_provider =
        renderPolicyRegistry().resolveDrawSortProvider(
            frame_plan.transparent_provider);
    std::vector<CompiledDrawQueueVariant> variants;
    variants.reserve(frame_plan.sort_views.size() * 2);
    for (std::uint32_t view_index = 0;
         view_index < frame_plan.sort_views.size(); ++view_index) {
        const auto &view = frame_plan.sort_views[view_index];
        const auto request = [&](DrawQueuePhase phase) {
            return DrawQueueBuildRequest{
                .items = frame_items,
                .material_filters =
                    frame_plan.material_filters,
                .max_draw_indirect_count = max_draw_indirect_count,
                .target_phase = phase,
                .logical_view = view.logical_view,
                .logical_view_origin = view.origin,
                .logical_view_forward = view.forward,
            };
        };
        variants.push_back(CompiledDrawQueueVariant{
            .phase = DrawQueuePhase::opaque,
            .sort_view_index = view_index,
            .queue = DrawQueueBuilder::build(
                request(DrawQueuePhase::opaque), opaque_provider),
        });
        variants.push_back(CompiledDrawQueueVariant{
            .phase = DrawQueuePhase::transparent,
            .sort_view_index = view_index,
            .queue = DrawQueueBuilder::build(
                request(DrawQueuePhase::transparent), transparent_provider),
        });
    }
    auto next_draw_queue =
        CompiledDrawQueueSet::combine(std::move(variants));
    const auto &indirect_records = next_draw_queue.indirectRecords();
    if (indirect_records.size() > maxRenderCommands * maxDrawSortViews) {
        throw std::runtime_error(
            "compiled draw queue exceeds indirect buffer capacity");
    }
    GET_MODULE(VulkanManageCore)
        .writeBuf(indirect_buf, indirect_records.data(), 0,
                  sizeof(RenderCommand) * indirect_records.size());
    compiled_draw_queue = std::move(next_draw_queue);

    GET_MODULE(VulkanManageCore)
        .writeBuf(model_data_buffer, slots.model_instances_data.data(), 0, sizeof(glm::mat4) * slots.model_instances_data.size());
    GET_MODULE(VulkanManageCore)
        .writeBuf(previous_model_data_buffer, slots.previous_model_instances_data.data(), 0,
                  sizeof(glm::mat4) * slots.previous_model_instances_data.size());
}

void PolygonInstanceContainer::commitFrameHistory() {
    advanceTemporalHistoryAfterRender();
}

void PolygonInstanceContainer::advanceMorphHistoryAfterRender() {
    for (auto &frame : slots.morph_weight_frames) {
        frame.previous = frame.current;
        frame.previous_revision = frame.current_revision;
    }
    std::fill(slots.morph_history_valid.begin(), slots.morph_history_valid.end(), true);
}

void PolygonInstanceContainer::advanceMaterialOverrideHistoryAfterRender() {
    for (auto &[instance, frame] : material_override_frames) {
        (void)instance;
        frame.previous = frame.current;
        frame.previous_revision = frame.current_revision;
    }
    for (auto &[key, frame] : material_absolute_override_frames) {
        (void)key;
        frame.previous = frame.current;
        frame.previous_revision = frame.current_revision;
    }
}

void PolygonInstanceContainer::advanceTemporalHistoryAfterRender() {
    slots.previous_model_instances_data = slots.model_instances_data;
    slots.previous_skin_palettes = slots.skin_palettes;
    slots.previous_animation_revisions = slots.animation_revisions;
    advanceMorphHistoryAfterRender();
    advanceMaterialOverrideHistoryAfterRender();
    std::fill(slots.model_history_valid.begin(), slots.model_history_valid.end(), true);
    ++temporal_history_advance_count;
}

void PolygonInstanceContainer::resetTemporalHistory() {
    slots.previous_model_instances_data = slots.model_instances_data;
    slots.previous_skin_palettes = slots.skin_palettes;
    slots.previous_animation_revisions = slots.animation_revisions;
    for (auto &frame : slots.morph_weight_frames) {
        frame.previous = frame.current;
        frame.previous_revision = frame.current_revision;
    }
    for (auto &[instance, frame] : material_override_frames) {
        (void)instance;
        frame.previous = frame.current;
        frame.previous_revision = frame.current_revision;
    }
    for (auto &[key, frame] : material_absolute_override_frames) {
        (void)key;
        frame.previous = frame.current;
        frame.previous_revision = frame.current_revision;
    }
    std::fill(slots.model_history_valid.begin(), slots.model_history_valid.end(), false);
    std::fill(slots.morph_history_valid.begin(), slots.morph_history_valid.end(), false);
}

bool PolygonInstanceContainer::canRebuildModelInstances(
    std::span<const ModelInstanceRebuild> replacements) const {
    std::unordered_map<std::uint64_t, const ModelTemplate *> by_asset;
    for (const auto &replacement : replacements) {
        if (!isValidModelAssetId(replacement.asset_id) || replacement.replacement == nullptr ||
            !by_asset.emplace(replacement.asset_id.value, replacement.replacement).second)
            return false;
    }

    size_t command_count = draw_inventory.size();
    for (const auto &item : draw_inventory) {
        const auto instance = item.stable_identity.instance.index;
        if (instance < slots.model_asset_ids.size() &&
            slots.instance_slots.alive(instance) &&
            by_asset.contains(slots.model_asset_ids[instance].value)) {
            --command_count;
        }
    }
    for (const auto &[asset, replacement] : by_asset) {
        size_t instances = 0;
        for (std::size_t index = 0; index < slots.model_asset_ids.size(); ++index)
            if (slots.instance_slots.alive(static_cast<std::uint32_t>(index)) &&
                slots.model_asset_ids[index].value == asset)
                ++instances;
        const auto primitives = primitiveCount(*replacement);
        if (instances != 0 && primitives > (maxRenderCommands - command_count) / instances)
            return false;
        command_count += instances * primitives;
    }
    return command_count <= maxRenderCommands;
}

PolygonInstanceContainer::PreparedSceneState
PolygonInstanceContainer::prepareModelInstanceRebuild(
    std::span<const ModelInstanceRebuild> replacements) {
    if (!replacements.empty() &&
        ray_query_geometry_generation ==
            std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "ray-query model rebuild generation exhausted");
    }
    if (!canRebuildModelInstances(replacements))
        throw std::runtime_error("Render command capacity exceeded by model reload batch");
    // Everything below edits the candidate, never `this`: `next` is the
    // candidate's own slot table, so a rebuilt slot is written once, in place,
    // instead of through a shadow copy per array.
    auto prepared = snapshotPreparedScene();
    auto &next = prepared.slots;
    std::unordered_map<std::uint64_t, const ModelTemplate *> by_asset;
    std::unordered_map<std::uint64_t, std::vector<glm::mat4>> rest_palettes;
    std::unordered_map<std::uint64_t, std::vector<glm::mat4>> rest_node_matrices;
    for (const auto &replacement : replacements) {
        by_asset.emplace(replacement.asset_id.value, replacement.replacement);
        auto &palette = rest_palettes[replacement.asset_id.value];
        auto &node_matrices = rest_node_matrices[replacement.asset_id.value];
        if (replacement.replacement->skeletal) {
            palette = evaluateSkinPalette(*replacement.replacement->skeletal, nullptr,
                                          0.0, 1.0, false, 0.0, &node_matrices);
        }
    }

    std::vector<DrawItemSnapshot> rebuilt;
    rebuilt.reserve(maxRenderCommands);
    for (const auto &item : prepared.draw_inventory) {
        const auto instance = item.stable_identity.instance.index;
        if (instance < next.model_asset_ids.size() &&
            next.instance_slots.alive(instance) &&
            by_asset.contains(next.model_asset_ids[instance].value))
            continue;
        rebuilt.push_back(item);
    }

    std::size_t replacement_item_count = 0;
    for (std::uint32_t instance = 0; instance < next.model_asset_ids.size();
         ++instance) {
        if (!next.instance_slots.alive(instance)) continue;
        const auto found = by_asset.find(next.model_asset_ids[instance].value);
        if (found != by_asset.end()) {
            replacement_item_count += primitiveCount(*found->second);
        }
    }
    if (replacement_item_count >
        std::numeric_limits<std::uint64_t>::max() -
            prepared.next_draw_declaration_ordinal) {
        throw std::runtime_error("Draw declaration ordinal exhausted");
    }
    auto next_declaration_ordinal = prepared.next_draw_declaration_ordinal;
    const auto *materials = replacement_item_count == 0
                                ? nullptr
                                : &GET_MODULE(MaterialContainer);

    for (uint32_t instance = 0; instance < next.model_asset_ids.size();
         ++instance) {
        if (!next.instance_slots.alive(instance)) continue;
        const auto found = by_asset.find(next.model_asset_ids[instance].value);
        if (found == by_asset.end()) continue;
        const auto &replacement = *found->second;
        const auto instance_id = next.instance_slots.idAt(instance);
        for (const auto &material : replacement.material_primitives) {
            if (material.primitives.empty()) continue;
            const auto route = materials->routeForMaterial(material.material);
            const auto &document =
                materials->materialDocumentReference(material.material);
            for (const auto &primitive : material.primitives) {
                rebuilt.push_back(makeDrawItemSnapshot(
                    instance_id, material, primitive,
                    replacement.authored_asset_reference, document, route,
                    next_declaration_ordinal++));
            }
        }

        const auto asset = next.model_asset_ids[instance].value;
        const auto &rest_palette = rest_palettes.at(asset);
        next.skin_palettes[instance] = rest_palette;
        next.animation_node_matrices[instance] = rest_node_matrices.at(asset);
        next.previous_skin_palettes[instance] = rest_palette;
        next.animation_revisions[instance] = 0;
        next.previous_animation_revisions[instance] = 0;
        if (++next.animation_generations[instance] == 0)
            ++next.animation_generations[instance];
        next.material_initial_value_tables[instance] =
            replacement.material_initial_values;
        next.vrm_semantics[instance] = replacement.vrm_semantic;
        next.morph_layouts[instance] = replacement.morph_targets;
        const auto defaults = replacement.morph_targets
                                  ? replacement.morph_targets->default_weights
                                  : std::vector<float>{};
        next.morph_weight_frames[instance] = MorphWeightFrame{
            .instance_identity = static_cast<std::uint64_t>(instance) + 1,
            .instance_generation = next.animation_generations[instance],
            .layout_generation = replacement.morph_targets
                                     ? replacement.morph_targets->generation
                                     : 0,
            .current_revision = 0,
            .previous_revision = 0,
            .current = defaults,
            .previous = defaults,
        };
        next.morph_history_valid[instance] = false;
        prepared.material_override_frames.erase(instance);
        const auto identity = static_cast<std::uint64_t>(instance) + 1;
        std::erase_if(prepared.material_absolute_override_frames,
                      [identity](const auto &entry) {
                          return entry.first.instance_identity == identity;
                      });
        next.previous_model_instances_data[instance] =
            next.model_instances_data[instance];
        next.model_history_valid[instance] = false;
    }
    prepared.draw_inventory = std::move(rebuilt);
    prepared.next_draw_declaration_ordinal = next_declaration_ordinal;
    if (!replacements.empty()) {
        ++prepared.ray_query_geometry_generation;
    }
    std::vector<RayQueryGeometryInstanceSnapshot> procedurals;
    if (prepared.ray_scene_plan) {
        for (const auto &instance : prepared.ray_scene_plan->inventory->instances)
            if (instance.geometry_kind == RayGeometryKind::aabbs)
                procedurals.push_back(instance);
    }
    prepareRayScenePlan(prepared,
        prepared.ray_scene_plan ? prepared.ray_scene_plan->project : nullptr,
        nullptr, procedurals);
    return prepared;
}

void PolygonInstanceContainer::setStagedAuthoringObject(
    StagedModelInstance &staged, std::uint64_t object) noexcept {
    staged.impl_->seed.authoring_object_id = object;
}

void PolygonInstanceContainer::rebuildModelInstances(
    std::span<const ModelInstanceRebuild> replacements) {
    auto prepared = prepareModelInstanceRebuild(replacements);
    publishPreparedScene(std::move(prepared));
}

std::shared_ptr<const RayScenePlanState>
PolygonInstanceContainer::prepareFrameRayScenePlan(
    std::shared_ptr<const RayTracingProjectConfig> project,
    std::shared_ptr<const RaySceneInventorySnapshot> inventory,
    std::shared_ptr<const RaySceneDeclarationCatalog> declarations) const {
    if (!declarations) declarations = ray_scene_declarations;
    if (ray_scene_plan && ray_scene_plan->project == project &&
        *ray_scene_plan->declarations == *declarations &&
        ray_scene_plan->inventory->instances == inventory->instances)
        return ray_scene_plan;
    auto candidate = std::make_shared<RayScenePlanState>();
    candidate->project = std::move(project);
    candidate->inventory = std::move(inventory);
    candidate->declarations = std::move(declarations);
    if (candidate->project) {
        auto resolved = std::make_shared<RaySceneRowSnapshot>(
            resolveRayTracingSceneRows(candidate->inventory->instances,
                                      *candidate->project, *candidate->declarations));
        resolved->generation = ray_scene_plan && ray_scene_plan->resolved_rows
            ? ray_scene_plan->resolved_rows->generation + 1 : 1;
        candidate->resolved_rows = std::move(resolved);
    }
    return candidate;
}

nlohmann::ordered_json PolygonInstanceContainer::rayComponentWitness(
    std::uint64_t object, const PreparedSceneState *prepared) const {
    const auto &declarations = *(prepared ? prepared->ray_scene_declarations : ray_scene_declarations);
    const auto found = declarations.find(object);
    return found == declarations.end() ? nlohmann::ordered_json::object() : found->second.component_witness;
}

void PolygonInstanceContainer::prepareRayScenePlan(
    PreparedSceneState &prepared,
    std::shared_ptr<const RayTracingProjectConfig> project,
    const ResolvedSceneView *scene,
    std::span<const RayQueryGeometryInstanceSnapshot> procedurals,
    SceneLoadPhaseTrace *phase_trace) {
    if (phase_trace) phase_trace->record(SceneLoadPhase::prepare);
    if (scene) {
        const auto transforms = resolvedSceneWorldTransforms(*scene);
        auto candidate_declarations = std::make_shared<RaySceneDeclarationCatalog>();
        for (const auto &object : scene->objects) {
            for (const auto &component : object.components) {
                if (component.name == "pelican_ray_instance") {
                    candidate_declarations->emplace(object.authoring_object_id.value,
                        RaySceneDeclaration{std::any_cast<const RayInstanceComponent &>(
                            component.requireRuntimeValue()), resolvedComponentWitness(component)});
                }
            }
        }
        prepared.ray_scene_declarations = std::move(candidate_declarations);
        for (std::size_t index = 0; index < prepared.slots.ray_authoring_object_ids.size(); ++index) {
            if (!prepared.slots.instance_slots.alive(static_cast<std::uint32_t>(index))) continue;
            const auto object = prepared.slots.ray_authoring_object_ids[index];
            if (const auto found = transforms.find(object); found != transforms.end()) {
                const auto &world = found->second;
                prepared.slots.model_instances_data[index] =
                    glm::translate(glm::mat4{1.0F}, world.pos) *
                    glm::toMat4(world.rotation) * glm::scale(glm::mat4{1.0F}, world.scale);
            }
        }
    }
    auto inventory = std::make_shared<RaySceneInventorySnapshot>(
        makeRaySceneInventorySnapshot(rayQueryGeometryInstances(&prepared), procedurals,
                                     prepared.ray_query_geometry_generation));
    for (std::size_t index = 0; index < inventory->instances.size(); ++index) {
        auto &instance = inventory->instances[index];
        instance.ray_instance_component_witness = rayComponentWitness(instance.authoring_object_id, &prepared);
        std::visit([&](auto &row) { row.instance = instance; }, inventory->rows[index]);
    }
    prepared.ray_scene_plan = prepareFrameRayScenePlan(std::move(project), std::move(inventory),
                                                      prepared.ray_scene_declarations);
}

std::vector<RayQueryGeometryInstanceSnapshot>
PolygonInstanceContainer::rayQueryGeometryInstances(
    const PreparedSceneState *prepared) const {
    // Scene and model reload prepare the same inventory shape as the live
    // renderer without temporarily publishing their candidate.
    const auto &draw_inventory =
        prepared ? prepared->draw_inventory : this->draw_inventory;
    const auto &table = prepared ? prepared->slots : this->slots;
    const auto &declarations =
        *(prepared ? prepared->ray_scene_declarations : ray_scene_declarations);
    const auto &material_absolute_override_frames = prepared
        ? prepared->material_absolute_override_frames : this->material_absolute_override_frames;
    const auto &material_override_frames =
        prepared ? prepared->material_override_frames : this->material_override_frames;
    std::vector<RayQueryGeometryInstanceSnapshot> result;
    result.reserve(draw_inventory.size());
    for (const auto &item : draw_inventory) {
        const auto instance =
            item.stable_identity.instance.index;
        if (instance >= table.model_instances_data.size() ||
            instance >= table.model_asset_ids.size() ||
            !table.instance_slots.alive(instance)) {
            continue;
        }
        std::uint32_t alpha_override_mask = 0;
        std::array<float, 4> alpha_override_base_color_factor{
            1.0F, 1.0F, 1.0F, 1.0F};
        std::array<float, 4> alpha_override_uv_offset_scale{
            0.0F, 0.0F, 1.0F, 1.0F};
        float alpha_override_uv_rotation = 0.0F;
        bool has_absolute_uv_override = false;
        const MaterialInstanceAbsoluteOverrideKey override_key{
            static_cast<std::uint64_t>(instance) + 1,
            item.pipeline_material_key.source_material_index};
        if (const auto override =
                material_absolute_override_frames.find(override_key);
            override != material_absolute_override_frames.end()) {
            const auto &values = override->second.current;
            if ((values.mask & materialOverrideBaseColor) != 0) {
                alpha_override_mask |= rayAlphaOverrideBaseFactor;
                alpha_override_base_color_factor = {
                    values.base_color_factor.x,
                    values.base_color_factor.y,
                    values.base_color_factor.z,
                    values.base_color_factor.w,
                };
            }
            if ((values.mask & materialOverrideUvTransform) != 0) {
                has_absolute_uv_override = true;
                alpha_override_mask |= rayAlphaOverrideUvTransform;
                alpha_override_uv_offset_scale = {
                    values.uv_offset.x,
                    values.uv_offset.y,
                    values.uv_scale.x,
                    values.uv_scale.y,
                };
                alpha_override_uv_rotation =
                    values.uv_rotation;
            }
        }
        // Raster resolves UV transforms before sampling: a source-material
        // absolute override wins, otherwise the per-instance relative UV
        // transform is used. Relative base-color multiplication happens only
        // after raster discard and therefore remains intentionally absent.
        if (!has_absolute_uv_override) {
            if (const auto relative =
                    material_override_frames.find(instance);
                relative != material_override_frames.end() &&
                (relative->second.current.mask &
                 materialOverrideUvTransform) != 0) {
                const auto &values = relative->second.current;
                alpha_override_mask |= rayAlphaOverrideUvTransform;
                alpha_override_uv_offset_scale = {
                    values.uv_offset.x,
                    values.uv_offset.y,
                    values.uv_scale.x,
                    values.uv_scale.y,
                };
                alpha_override_uv_rotation =
                    values.uv_rotation;
            }
        }
        const auto authoring_object = table.ray_authoring_object_ids.at(instance);
        const auto declaration = declarations.find(authoring_object);
        result.push_back(
            RayQueryGeometryInstanceSnapshot{
                .asset_identity =
                    table.model_asset_ids[instance].value,
                .authoring_object_id =
                    authoring_object,
                .ray_instance = declaration == declarations.end()
                    ? std::nullopt : std::optional{declaration->second.value},
                .ray_instance_component_witness = declaration == declarations.end()
                    ? nlohmann::ordered_json::object() : declaration->second.component_witness,
                .instance_index =
                    item.stable_identity.instance.index,
                .instance_generation =
                    item.stable_identity.instance.generation,
                .scene_epoch =
                    item.stable_identity.instance.scene_epoch,
                .material_asset_reference =
                    item.material_asset_reference,
                .material_document_reference =
                    item.material_document_reference,
                .material_name = item.material_name,
                .resolved_material_handle =
                    item.pipeline_material_key.material.value,
                .resolved_material_generation =
                    item.ray_material_generation,
                .resolved_material_lease =
                    item.ray_material_lease,
                .source_material_index =
                    item.pipeline_material_key.source_material_index ==
                            noSourceMaterialIndex
                        ? std::optional<std::uint32_t>{}
                        : std::optional<std::uint32_t>{
                              item.pipeline_material_key.source_material_index},
                .alpha_override_mask = alpha_override_mask,
                .alpha_override_base_color_factor =
                    alpha_override_base_color_factor,
                .alpha_override_uv_offset_scale =
                    alpha_override_uv_offset_scale,
                .alpha_override_uv_rotation =
                    alpha_override_uv_rotation,
                .geometry_allocation_id =
                    item.geometry_allocation_id,
                .index_count = item.indexed.index_count,
                .index_offset = item.indexed.first_index,
                .vertex_offset = item.indexed.vertex_offset,
                .vertex_count = item.vertex_count,
                .attribute_presence =
                    item.ray_attribute_presence,
                .skinned =
                    item.pipeline_material_key.skinned,
                .morph_deformed = item.morph_deformed,
                .vat_deformed = item.vat_deformed,
                .mesh_index =
                    item.stable_identity.mesh_index,
                .primitive_index =
                    item.stable_identity.primitive_index,
                .node_index =
                    item.stable_identity.node_index,
                .occurrence_id =
                    item.stable_identity.occurrence
                        ? item.stable_identity.occurrence->occurrence_id
                        : std::string{},
                .occurrence_asset =
                    item.stable_identity.occurrence &&
                            item.stable_identity.occurrence->asset
                        ? item.stable_identity.occurrence->asset->logical_name
                        : std::string{},
                .occurrence_node_path =
                    item.stable_identity.occurrence
                        ? item.stable_identity.occurrence->node_name_path
                        : std::vector<std::string>{},
                .occurrence_variant =
                    item.stable_identity.occurrence
                        ? importerVariantV1Name(
                              item.stable_identity.occurrence->variant)
                        : std::string{},
                .occurrence_error =
                    item.stable_identity.occurrence &&
                            item.stable_identity.occurrence->error
                        ? item.stable_identity.occurrence->error->reason
                        : std::string{},
                .occurrence_ambiguous =
                    item.stable_identity.occurrence &&
                    item.stable_identity.occurrence->ambiguous,
                .world_transform =
                    table.model_instances_data[instance],
            });
    }
    return result;
}

std::vector<RayOccurrenceCatalogEntry>
PolygonInstanceContainer::rayOccurrences(
    std::uint64_t authoring_object_id) const {
    const auto plan = rayAsPlan();
    const auto inventory = rayAsInventory();
    const auto fallback = inventory ? std::vector<RayQueryGeometryInstanceSnapshot>{}
                                    : rayQueryGeometryInstances();
    const auto &snapshots = inventory ? inventory->instances : fallback;
    std::vector<const RaySceneRow *> resolved_rows(snapshots.size(), nullptr);
    if (plan) {
        for (const auto &row : plan->rows)
            resolved_rows.at(row.source_index) = &row;
    }
    std::vector<RayOccurrenceCatalogEntry> result;
    result.reserve(snapshots.size());
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        const auto &snapshot = snapshots[index];
        if (snapshot.geometry_kind != RayGeometryKind::triangles) continue;
        const bool ray_eligible = snapshot.geometry_allocation_id != 0 &&
                                  snapshot.vertex_offset >= 0 &&
                                  snapshot.vertex_count != 0 &&
                                  snapshot.index_count != 0 &&
                                  snapshot.index_count % 3u == 0;
        if (authoring_object_id != 0 &&
            snapshot.authoring_object_id != authoring_object_id)
            continue;
        RayOccurrenceCatalogEntry entry{
            .occurrence_id = snapshot.occurrence_id,
            .display_name =
                snapshot.occurrence_asset + ":" +
                (snapshot.occurrence_node_path.empty()
                     ? std::string{"<unnamed>"}
                     : snapshot.occurrence_node_path.back()) +
                "/" + std::to_string(snapshot.primitive_index),
            .asset = snapshot.occurrence_asset,
            .node_path = snapshot.occurrence_node_path,
            .mesh = snapshot.mesh_index,
            .primitive = snapshot.primitive_index,
            .material = snapshot.material_name,
            .variant = snapshot.occurrence_variant,
            .authoring_object_id = snapshot.authoring_object_id,
        };
        if (const auto *row = resolved_rows[index]) {
            entry.q = row->q;
            entry.effective = RayOccurrenceEffectiveValues{
                .mask = row->instance_mask,
                .instance_flags = row->instance_flags,
                .custom_index = row->instance_custom_index,
            };
        }
        if (snapshot.occurrence_ambiguous) {
            entry.eligibility = RayOccurrenceEligibilityKind::ambiguous;
            entry.reason = "collision";
        } else if (snapshot.occurrence_id.empty()) {
            entry.eligibility = RayOccurrenceEligibilityKind::unrepresentable;
            entry.reason = snapshot.occurrence_error.empty()
                               ? "asset_key_missing"
                               : snapshot.occurrence_error;
        } else if (!ray_eligible || !entry.q) {
            entry.eligibility = RayOccurrenceEligibilityKind::not_ray_traced;
            if (!ray_eligible) entry.reason = "blas_ineligible";
            else entry.reason = plan ? "not_in_as_plan" : "as_plan_unavailable";
        }
        result.push_back(std::move(entry));
    }
    return result;
}

RayOccurrenceResolution PolygonInstanceContainer::resolveRayOccurrence(
    std::string_view occurrence_id) const {
    RayOccurrenceResolution result;
    if (occurrence_id.empty()) return result;
    for (auto entry : rayOccurrences()) {
        if (entry.occurrence_id == occurrence_id)
            result.candidates.push_back(std::move(entry));
    }
    if (result.candidates.empty()) return result;
    result.state = result.candidates.size() == 1 &&
                           result.candidates.front().eligibility !=
                               RayOccurrenceEligibilityKind::ambiguous
                       ? RayOccurrenceResolveState::found
                       : RayOccurrenceResolveState::ambiguous;
    return result;
}

void PolygonInstanceContainer::setRayInstanceDeclaration(
    ModelInstanceId id, std::uint64_t authoring_object_id,
    std::optional<RayInstanceComponent> declaration) {
    const auto index = requireLive(id, "setRayInstanceDeclaration");
    const auto current = ray_scene_declarations->find(authoring_object_id);
    const bool unchanged = declaration
        ? current != ray_scene_declarations->end() && current->second.value == *declaration
        : current == ray_scene_declarations->end();
    if (slots.ray_authoring_object_ids[index] == authoring_object_id && unchanged) {
        return;
    }
    auto declarations = std::make_shared<RaySceneDeclarationCatalog>(*ray_scene_declarations);
    if (declaration) {
        auto &owner = (*declarations)[authoring_object_id];
        owner.value = *declaration;
    } else declarations->erase(authoring_object_id);
    ray_scene_declarations = std::move(declarations);
    slots.ray_authoring_object_ids[index] = authoring_object_id;
}

void PolygonInstanceContainer::rebuildModelInstances(
    ModelAssetId asset_id, const ModelTemplate &replacement) {
    const ModelInstanceRebuild request{asset_id, &replacement};
    rebuildModelInstances(std::span{&request, std::size_t{1}});
}

void PolygonInstanceContainer::setSkinningPalette(ModelInstanceId id,
                                                   std::span<const glm::mat4> palette) {
    const auto index = requireLive(id, "setSkinningPalette");
    if (palette.size() > maxSkinJoints) throw std::runtime_error("Skin palette exceeds 128 joints");
    slots.skin_palettes[index].assign(palette.begin(), palette.end());
    GET_MODULE(VulkanManageCore).writeBuf(skin_palette_buffer, palette.data(),
                                         sizeof(glm::mat4) * maxSkinJoints * index,
                                         sizeof(glm::mat4) * palette.size());
    // A palette-only override does not provide the evaluated node hierarchy.
    slots.animation_node_matrices[index].clear();
}

Animation::InstanceHandle PolygonInstanceContainer::animationInstance(ModelInstanceId id) const {
    if (!isLive(id)) return Animation::invalidHandle<Animation::InstanceHandle>();
    return {static_cast<std::uint64_t>(id.index) + 1,
            slots.animation_generations[id.index], 0};
}

std::string_view animationFrameWriterName(AnimationFrameWriterKind kind) noexcept {
    switch (kind) {
    case AnimationFrameWriterKind::animation_component_clip: return "animation component";
    case AnimationFrameWriterKind::animation_source: return "animation source";
    case AnimationFrameWriterKind::none: break;
    }
    return "nobody";
}

AnimationFrameWriter PolygonInstanceContainer::animationFrameWriter(ModelInstanceId id) const {
    if (!isLive(id)) return {};
    const auto found = animation_frame_writers.find(id.index);
    if (found == animation_frame_writers.end() ||
        found->second.generation != slots.animation_generations[id.index])
        return {};
    return found->second.writer;
}

Animation::Status PolygonInstanceContainer::publishAnimationFrame(
    ModelInstanceId id, AnimationFrameWriter writer,
    const Animation::PublishAnimationFrameDescV1 &frame,
    std::span<const Animation::Matrix4fV1> model_matrices,
    std::span<const std::uint32_t> original_to_layout) {
    constexpr auto minimum = offsetof(Animation::PublishAnimationFrameDescV1, root_delta) +
                             sizeof(Animation::RootDeltaV1);
    if (frame.struct_size < minimum) return Animation::Status::invalid_argument;
    if (frame.version != Animation::descriptorVersionV1) return Animation::Status::unsupported_version;
    if (frame.reserved0 != 0 || frame.reserved1 != 0) return Animation::Status::reserved_not_zero;
    if (!isLive(id)) return Animation::Status::invalid_handle;
    const auto index = id.index;
    const auto expected = animationInstance(id);
    if (!Animation::isValid(frame.instance) || frame.instance.identity != expected.identity)
        return Animation::Status::invalid_handle;
    if (frame.instance.generation != expected.generation) return Animation::Status::stale_generation;
    // A second, different writer inside one frame revision is refused as soon
    // as the instance is known -- before anything about the frame it brought is
    // looked at, and before any of this instance's state is touched -- so the
    // two poses cannot race and the later one cannot silently replace the
    // earlier one. The same writer publishing twice is still
    // duplicate_revision, which is a different mistake and keeps its own name.
    const auto owner = animation_frame_writers.find(index);
    if (owner != animation_frame_writers.end() &&
        owner->second.generation == expected.generation &&
        owner->second.writer.kind != AnimationFrameWriterKind::none &&
        owner->second.writer.frame_revision == frame.frame_revision &&
        (owner->second.writer.kind != writer.kind ||
         owner->second.writer.identity != writer.identity))
        return Animation::Status::authority_conflict;
    if (!Animation::isValid(frame.local_pose) || !Animation::isValid(frame.model_pose) ||
        frame.frame_revision == 0 || (frame.palette_count != 0 && frame.palette == nullptr) ||
        frame.palette_count > maxSkinJoints ||
        (frame.flags & ~(Animation::commit_reset_history | Animation::commit_discontinuity)) != 0)
        return Animation::Status::invalid_argument;
    if (slots.animation_revisions[index] != 0 &&
        frame.frame_revision <= slots.animation_revisions[index])
        return Animation::Status::duplicate_revision;

    if (model_matrices.size() != original_to_layout.size())
        return Animation::Status::invalid_argument;
    std::vector<glm::mat4> node_matrices(original_to_layout.size());
    for (std::size_t source = 0; source < original_to_layout.size(); ++source) {
        const auto layout = original_to_layout[source];
        if (layout >= model_matrices.size()) return Animation::Status::invalid_argument;
        std::memcpy(&node_matrices[source], &model_matrices[layout], sizeof(glm::mat4));
    }
    std::vector<glm::mat4> palette(frame.palette_count);
    if (!palette.empty()) std::memcpy(palette.data(), frame.palette, palette.size() * sizeof(glm::mat4));
    if (!palette.empty()) {
        GET_MODULE(VulkanManageCore).writeBuf(skin_palette_buffer, palette.data(),
                                             sizeof(glm::mat4) * maxSkinJoints * index,
                                             sizeof(glm::mat4) * palette.size());
    }
    slots.skin_palettes[index] = std::move(palette);
    slots.animation_node_matrices[index] = std::move(node_matrices);
    slots.animation_revisions[index] = frame.frame_revision;
    animation_frame_writers[index] = AnimationFrameWriterRecord{expected.generation,
        AnimationFrameWriter{writer.kind, writer.identity, frame.frame_revision}};
    if (slots.previous_animation_revisions[index] == 0 ||
        (frame.flags & (Animation::commit_reset_history | Animation::commit_discontinuity)) != 0) {
        slots.previous_skin_palettes[index] = slots.skin_palettes[index];
        slots.previous_animation_revisions[index] = frame.frame_revision;
    }
    return Animation::Status::ok;
}

std::optional<glm::mat4> PolygonInstanceContainer::animationNodeMatrix(
    ModelInstanceId id, std::uint32_t source_node) const {
    if (!isLive(id)) return std::nullopt;
    const auto &matrices = slots.animation_node_matrices[id.index];
    if (source_node >= matrices.size()) return std::nullopt;
    return matrices[source_node];
}

std::optional<float> PolygonInstanceContainer::morphWeight(
    ModelInstanceId id, std::uint32_t node, std::uint32_t target) const {
    if (!isLive(id)) return std::nullopt;
    const auto &layout = slots.morph_layouts[id.index];
    if (!layout) return std::nullopt;
    for (const auto &primitive : layout->primitives) {
        if (primitive.node_index != node) continue;
        for (const auto &delta : primitive.delta_ranges)
            if (delta.target_index == target)
                return slots.morph_weight_frames[id.index].current.at(primitive.weight_offset + target);
    }
    return std::nullopt;
}

Animation::Status PolygonInstanceContainer::publishMorphWeightFrame(
    ModelInstanceId id, const PublishMorphWeightFrameDescV1 &frame) {
    constexpr auto minimum =
        offsetof(PublishMorphWeightFrameDescV1, reserved1) + sizeof(std::uint32_t);
    if (frame.struct_size < minimum) return Animation::Status::invalid_argument;
    if (frame.version != morphWeightDescriptorVersionV1)
        return Animation::Status::unsupported_version;
    if (frame.reserved0 != 0 || frame.reserved1 != 0)
        return Animation::Status::reserved_not_zero;
    if (!isLive(id)) return Animation::Status::invalid_handle;
    const auto index = id.index;
    const auto &layout = slots.morph_layouts[index];
    if (!layout || frame.layout_generation != layout->generation)
        return Animation::Status::stale_generation;
    if (frame.frame_revision == 0 || frame.weight_count != layout->default_weights.size() ||
        (frame.weight_count != 0 && frame.weights == nullptr) ||
        frame.weight_count > maxMorphWeightsPerInstance ||
        (frame.flags & ~(morphCommitResetHistory | morphCommitDiscontinuity)) != 0)
        return Animation::Status::invalid_argument;
    const auto &current = slots.morph_weight_frames[index];
    if (current.current_revision != 0 &&
        frame.frame_revision <= current.current_revision)
        return Animation::Status::duplicate_revision;
    std::vector<float> weights;
    if (frame.weight_count != 0)
        weights.assign(frame.weights, frame.weights + frame.weight_count);
    if (std::any_of(weights.begin(), weights.end(),
                    [](float value) { return !std::isfinite(value); }))
        return Animation::Status::invalid_argument;

    auto &destination = slots.morph_weight_frames[index];
    destination.current = std::move(weights);
    destination.current_revision = frame.frame_revision;
    if (!slots.morph_history_valid[index] ||
        (frame.flags & (morphCommitResetHistory | morphCommitDiscontinuity)) != 0) {
        destination.previous = destination.current;
        destination.previous_revision = destination.current_revision;
        slots.morph_history_valid[index] = false;
    }
    return Animation::Status::ok;
}

Animation::Status PolygonInstanceContainer::publishMaterialInstanceOverride(
    ModelInstanceId id, const PublishMaterialInstanceOverrideDescV1 &frame) {
    constexpr auto minimum =
        offsetof(PublishMaterialInstanceOverrideDescV1, reserved1) +
        sizeof(std::uint32_t);
    if (frame.struct_size < minimum) return Animation::Status::invalid_argument;
    if (frame.version != materialInstanceOverrideDescriptorVersionV1)
        return Animation::Status::unsupported_version;
    if (frame.reserved0 != 0 || frame.reserved1 != 0)
        return Animation::Status::reserved_not_zero;
    if (!isLive(id)) return Animation::Status::invalid_handle;
    const auto index = id.index;

    const auto expected = animationInstance(id);
    if (!Animation::isValid(frame.instance) ||
        frame.instance.identity != expected.identity)
        return Animation::Status::invalid_handle;
    if (frame.instance.generation != expected.generation)
        return Animation::Status::stale_generation;
    if (frame.frame_revision == 0 ||
        (frame.values.mask & ~materialOverrideAll) != 0 ||
        (frame.flags & ~(materialOverrideCommitResetHistory |
                         materialOverrideCommitDiscontinuity)) != 0)
        return Animation::Status::invalid_argument;

    const auto finite = [](const auto &value) {
        for (glm::length_t i = 0; i < value.length(); ++i) {
            if (!std::isfinite(value[i])) return false;
        }
        return true;
    };
    if (!finite(frame.values.base_color_factor) ||
        !finite(frame.values.emissive_factor) ||
        !finite(frame.values.uv_offset) || !finite(frame.values.uv_scale) ||
        !std::isfinite(frame.values.uv_rotation))
        return Animation::Status::invalid_argument;

    const auto found = material_override_frames.find(index);
    if (found != material_override_frames.end() &&
        frame.frame_revision <= found->second.current_revision)
        return Animation::Status::duplicate_revision;

    MaterialInstanceOverrideFrame next =
        found == material_override_frames.end()
            ? MaterialInstanceOverrideFrame{
                  .instance_identity = expected.identity,
                  .instance_generation = expected.generation,
              }
            : found->second;
    next.current = frame.values;
    next.current_revision = frame.frame_revision;
    if (next.previous_revision == 0 ||
        (frame.flags & (materialOverrideCommitResetHistory |
                        materialOverrideCommitDiscontinuity)) != 0) {
        next.previous = next.current;
        next.previous_revision = next.current_revision;
    }

    const auto gpu = materialOverrideGpuData(next);
    GET_MODULE(VulkanManageCore).writeBuf(
        material_override_buffer, &gpu,
        sizeof(MaterialInstanceOverrideGpuData) * index, sizeof(gpu));
    material_override_frames.insert_or_assign(index, std::move(next));
    return Animation::Status::ok;
}

void PolygonInstanceContainer::uploadPublishedSceneState() {
    if (!published_scene_gpu_dirty) return;
    auto &vkcore = GET_MODULE(VulkanManageCore);
    for (std::size_t i = 0; i < slots.skin_palettes.size(); ++i) {
        const auto &palette = slots.skin_palettes[i];
        if (!palette.empty()) {
            vkcore.writeBuf(skin_palette_buffer, palette.data(),
                            sizeof(glm::mat4) * maxSkinJoints * i,
                            sizeof(glm::mat4) * palette.size());
        }
    }
    // Cleared overrides must also overwrite their former GPU slots.
    std::vector<MaterialInstanceOverrideGpuData> overrides(maxModelInstances);
    for (const auto &[index, frame] : material_override_frames) {
        overrides.at(index) = materialOverrideGpuData(frame);
    }
    vkcore.writeBuf(material_override_buffer, overrides.data(), 0,
                    sizeof(MaterialInstanceOverrideGpuData) * overrides.size());
    uploadMaterialAbsoluteOverrides();
    published_scene_gpu_dirty = false;
}

void PolygonInstanceContainer::uploadMaterialAbsoluteOverrides() {
    std::vector<const MaterialInstanceAbsoluteOverrideFrame *> sorted;
    sorted.reserve(material_absolute_override_frames.size());
    for (const auto &[key, frame] : material_absolute_override_frames) {
        (void)key;
        sorted.push_back(&frame);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto *left, const auto *right) {
        return std::tie(left->instance_identity, left->source_material_index) <
               std::tie(right->instance_identity, right->source_material_index);
    });

    std::vector<MaterialInstanceAbsoluteOverrideGpuHeader> headers(
        maxModelInstances);
    std::vector<MaterialInstanceAbsoluteOverrideGpuData> records;
    records.reserve(sorted.size());
    for (const auto *frame : sorted) {
        if (frame->instance_identity == 0 ||
            frame->instance_identity > maxModelInstances) {
            throw std::runtime_error(
                "Material absolute override instance identity is out of range");
        }
        auto &header = headers[frame->instance_identity - 1];
        if (header.record_count == 0) {
            header.record_offset = static_cast<std::uint32_t>(records.size());
            header.instance_generation = frame->instance_generation;
        }
        ++header.record_count;
        records.push_back(MaterialInstanceAbsoluteOverrideGpuData{
            .base_color_factor = frame->current.base_color_factor,
            .emissive_factor = frame->current.emissive_factor,
            .uv_offset_scale =
                glm::vec4{frame->current.uv_offset, frame->current.uv_scale},
            .uv_rotation_reserved =
                glm::vec4{frame->current.uv_rotation, 0.0f, 0.0f, 0.0f},
            .metadata = glm::uvec4{
                frame->current.mask, frame->source_material_index,
                static_cast<std::uint32_t>(frame->current_revision),
                static_cast<std::uint32_t>(frame->current_revision >> 32u)},
        });
    }

    auto &vkcore = GET_MODULE(VulkanManageCore);
    vkcore.writeBuf(material_absolute_override_header_buffer, headers.data(), 0,
                    sizeof(MaterialInstanceAbsoluteOverrideGpuHeader) *
                        headers.size());
    if (!records.empty()) {
        vkcore.writeBuf(material_absolute_override_record_buffer, records.data(), 0,
                        sizeof(MaterialInstanceAbsoluteOverrideGpuData) *
                            records.size());
    }
}

Animation::Status
PolygonInstanceContainer::publishMaterialInstanceAbsoluteOverride(
    ModelInstanceId id,
    const PublishMaterialInstanceAbsoluteOverrideDescV2 &frame) {
    constexpr auto minimum =
        offsetof(PublishMaterialInstanceAbsoluteOverrideDescV2, reserved1) +
        sizeof(std::uint32_t);
    if (frame.struct_size < minimum) return Animation::Status::invalid_argument;
    if (frame.version != materialInstanceAbsoluteOverrideDescriptorVersionV2)
        return Animation::Status::unsupported_version;
    if (frame.reserved0 != 0 || frame.reserved1 != 0)
        return Animation::Status::reserved_not_zero;
    if (!isLive(id)) return Animation::Status::invalid_handle;
    const auto index = id.index;

    const auto expected = animationInstance(id);
    if (!Animation::isValid(frame.instance) ||
        frame.instance.identity != expected.identity)
        return Animation::Status::invalid_handle;
    if (frame.instance.generation != expected.generation)
        return Animation::Status::stale_generation;
    if (frame.frame_revision == 0 ||
        frame.source_material_index == noSourceMaterialIndex ||
        (frame.values.mask & ~materialOverrideAll) != 0 ||
        (frame.flags & ~(materialOverrideCommitResetHistory |
                         materialOverrideCommitDiscontinuity)) != 0)
        return Animation::Status::invalid_argument;

    const auto &initial_values = slots.material_initial_value_tables[index];
    if (!initial_values ||
        frame.source_material_index >= initial_values->values.size() ||
        initial_values->values[frame.source_material_index]
                .source_material_index != frame.source_material_index)
        return Animation::Status::invalid_argument;

    const auto finite = [](const auto &value) {
        for (glm::length_t i = 0; i < value.length(); ++i) {
            if (!std::isfinite(value[i])) return false;
        }
        return true;
    };
    if (!finite(frame.values.base_color_factor) ||
        !finite(frame.values.emissive_factor) ||
        !finite(frame.values.uv_offset) || !finite(frame.values.uv_scale) ||
        !std::isfinite(frame.values.uv_rotation))
        return Animation::Status::invalid_argument;

    const MaterialInstanceAbsoluteOverrideKey key{
        expected.identity, frame.source_material_index};
    const auto found = material_absolute_override_frames.find(key);
    if (found != material_absolute_override_frames.end() &&
        frame.frame_revision <= found->second.current_revision)
        return Animation::Status::duplicate_revision;
    if (found == material_absolute_override_frames.end() &&
        material_absolute_override_frames.size() >=
            maxMaterialInstanceAbsoluteOverrideRecords)
        return Animation::Status::out_of_memory;

    MaterialInstanceAbsoluteOverrideFrame next =
        found == material_absolute_override_frames.end()
            ? MaterialInstanceAbsoluteOverrideFrame{
                  .instance_identity = expected.identity,
                  .instance_generation = expected.generation,
                  .source_material_index = frame.source_material_index,
              }
            : found->second;
    next.current = frame.values;
    next.current_revision = frame.frame_revision;
    if (next.previous_revision == 0 ||
        (frame.flags & (materialOverrideCommitResetHistory |
                        materialOverrideCommitDiscontinuity)) != 0) {
        next.previous = next.current;
        next.previous_revision = next.current_revision;
    }

    material_absolute_override_frames.insert_or_assign(key, std::move(next));
    uploadMaterialAbsoluteOverrides();
    return Animation::Status::ok;
}

Animation::Status PolygonInstanceContainer::publishVrmApplicationTransaction(
    ModelInstanceId id,
    const PublishVrmApplicationTransactionDescV1 &transaction) {
    if (!isLive(id)) return Animation::Status::invalid_handle;
    const auto index = id.index;

    std::vector<std::uint32_t> material_indices;
    material_indices.reserve(transaction.material_overrides.size());
    for (const auto &material : transaction.material_overrides) {
        material_indices.push_back(material.source_material_index);
    }
    std::sort(material_indices.begin(), material_indices.end());
    if (std::adjacent_find(material_indices.begin(), material_indices.end()) !=
        material_indices.end())
        return Animation::Status::invalid_argument;

    MorphWeightFrame previous_morph;
    bool previous_morph_history_valid = false;
    decltype(material_absolute_override_frames) previous_materials;
    try {
        previous_morph = slots.morph_weight_frames.at(index);
        previous_morph_history_valid = slots.morph_history_valid.at(index);
        previous_materials = material_absolute_override_frames;
    } catch (...) {
        return Animation::Status::out_of_memory;
    }

    const auto rollback = [&]() noexcept {
        try {
            slots.morph_weight_frames[index] = previous_morph;
            slots.morph_history_valid[index] = previous_morph_history_valid;
            material_absolute_override_frames = previous_materials;
            uploadMaterialAbsoluteOverrides();
        } catch (...) {
        }
    };

    try {
        if (transaction.publish_morph) {
            const auto status = publishMorphWeightFrame(id, transaction.morph);
            if (status != Animation::Status::ok) {
                rollback();
                return status;
            }
        }
        for (const auto &material : transaction.material_overrides) {
            const auto status =
                publishMaterialInstanceAbsoluteOverride(id, material);
            if (status != Animation::Status::ok) {
                rollback();
                return status;
            }
        }
    } catch (...) {
        rollback();
        return Animation::Status::out_of_memory;
    }
    return Animation::Status::ok;
}

std::optional<VrmApplicationModelView>
PolygonInstanceContainer::vrmApplicationModel(
    Animation::InstanceHandle instance) const {
    if (!Animation::isValid(instance) || instance.identity == 0 ||
        instance.identity > slots.animation_generations.size())
        return std::nullopt;
    const auto index = static_cast<std::uint32_t>(instance.identity - 1);
    if (slots.animation_generations[index] != instance.generation ||
        index >= slots.vrm_semantics.size() || !slots.instance_slots.alive(index) ||
        !slots.vrm_semantics[index])
        return std::nullopt;
    return VrmApplicationModelView{
        .model_instance = slots.instance_slots.idAt(index),
        .instance = instance,
        .semantic = slots.vrm_semantics[index],
        .morph_layout = slots.morph_layouts[index],
        .material_initial_values = slots.material_initial_value_tables[index],
    };
}

void PolygonInstanceContainer::bindDeformation(
    vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout,
    bool skinned) const {
    cmd_buf.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics, pipeline_layout, PELICAN_SET_FREE,
        skinned ? skinned_deformation_descriptor_set.get()
                : static_deformation_descriptor_set.get(),
        {});
}

void PolygonInstanceContainer::bindMaterialInstanceResources(
    vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout,
    bool skinned) const {
    cmd_buf.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics, pipeline_layout, PELICAN_SET_FREE,
        skinned ? skinned_material_descriptor_set.get()
                : static_material_descriptor_set.get(),
        {});
}

void PolygonInstanceContainer::bindSkinning(vk::CommandBuffer cmd_buf,
                                            vk::PipelineLayout pipeline_layout) const {
    bindDeformation(cmd_buf, pipeline_layout, true);
}

PolygonInstanceContainer::PreparedModelTrs
PolygonInstanceContainer::prepareTrs(ModelInstanceId id, glm::vec3 pos,
                                     glm::quat rotation,
                                     glm::vec3 scale) const {
    const auto index = requireLive(id, "setTrs");
    const auto next = glm::translate(glm::identity<glm::mat4>(), pos) *
                      glm::toMat4(rotation) *
                      glm::scale(glm::identity<glm::mat4>(), scale);
    return PreparedModelTrs{
        .index = index,
        .old_value = slots.model_instances_data[index],
        .next_value = next,
    };
}

void PolygonInstanceContainer::publishPreparedTrs(
    PreparedModelTrs &prepared) noexcept {
    slots.model_instances_data[prepared.index] = prepared.next_value;
    prepared.published = true;
}

void PolygonInstanceContainer::rollbackPreparedTrs(
    PreparedModelTrs &prepared) noexcept {
    if (!prepared.published) return;
    slots.model_instances_data[prepared.index] = prepared.old_value;
    prepared.published = false;
}

void PolygonInstanceContainer::setTrs(ModelInstanceId id, glm::vec3 pos,
                                      glm::quat rotation, glm::vec3 scale) {
    auto prepared = prepareTrs(id, pos, rotation, scale);
    publishPreparedTrs(prepared);
}

std::optional<ModelInstanceId>
PolygonInstanceContainer::modelInstanceIdAt(std::uint32_t index) const {
    if (!slots.instance_slots.alive(index)) return std::nullopt;
    return slots.instance_slots.idAt(index);
}

const BufferWrapper &PolygonInstanceContainer::getIndirectBuf() const { return indirect_buf; }

void PolygonInstanceContainer::prepareViewFamilyDraws(
    const RenderViewFamilies &view_families,
    std::span<const std::string>
        locally_sorted_families) {
    prepared_view_family_draws.clear();
    const auto &canonical =
        compiled_draw_queue.indirectRecords();
    if (canonical.empty()) {
        return;
    }
    if (compiled_draw_queue.sortViewCount() ==
        0) {
        throw std::runtime_error(
            "view-family culling received an empty canonical sort view");
    }
    const auto canonical_command_count =
        compiled_draw_queue
            .queue(
                DrawQueuePhase::opaque, 0)
            .indirectRecords().size() +
        compiled_draw_queue
            .queue(
                DrawQueuePhase::transparent, 0)
            .indirectRecords().size();
    if (canonical_command_count >
            maxRenderCommands ||
        canonical_command_count >
            canonical.size()) {
        throw std::runtime_error(
            "view-family culling exceeds its canonical view region");
    }

    std::unordered_set<std::string_view>
        locally_sorted;
    locally_sorted.reserve(
        locally_sorted_families.size());
    for (const auto &family_id :
         locally_sorted_families) {
        if (family_id.empty() ||
            family_id ==
                mainRenderViewFamilyId) {
            throw std::invalid_argument(
                "secondary local draw sorting requires a non-main family id");
        }
        locally_sorted.insert(family_id);
        const auto found =
            std::find_if(
                view_families.families.begin(),
                view_families.families.end(),
                [&](const auto &family) {
                    return family.family_id ==
                           family_id;
                });
        if (found ==
            view_families.families.end()) {
            throw std::invalid_argument(
                "secondary local draw sorting references an unavailable "
                "view family: " +
                family_id);
        }
    }

    std::vector<DrawItemSnapshot>
        local_sort_items;
    DrawSortProviderLease opaque_provider;
    DrawSortProviderLease transparent_provider;
    std::uint32_t max_draw_indirect_count =
        0;
    if (!locally_sorted.empty()) {
        const auto &opaque_queue =
            compiled_draw_queue.queue(
                DrawQueuePhase::opaque, 0);
        const auto &transparent_queue =
            compiled_draw_queue.queue(
                DrawQueuePhase::transparent, 0);
        local_sort_items.reserve(
            opaque_queue.orderedItems().size() +
            transparent_queue.orderedItems()
                .size());
        local_sort_items.insert(
            local_sort_items.end(),
            opaque_queue.orderedItems().begin(),
            opaque_queue.orderedItems().end());
        local_sort_items.insert(
            local_sort_items.end(),
            transparent_queue.orderedItems()
                .begin(),
            transparent_queue.orderedItems()
                .end());
        opaque_provider =
            renderPolicyRegistry()
                .resolveDrawSortProvider(
                    opaque_queue.provider().name);
        transparent_provider =
            renderPolicyRegistry()
                .resolveDrawSortProvider(
                    transparent_queue
                        .provider()
                        .name);
        max_draw_indirect_count =
            GET_MODULE(VulkanManageCore)
                .getPhysDevice()
                .getProperties()
                .limits
                .maxDrawIndirectCount;
    }

    const auto build_local_queue =
        [&](const RenderViewParameters &view) {
            const auto sort_view =
                drawQueueSortView(view);
            const auto request =
                [&](DrawQueuePhase phase) {
                    return DrawQueueBuildRequest{
                        .items =
                            local_sort_items,
                        .material_filters =
                            draw_queue_material_filters,
                        .max_draw_indirect_count =
                            max_draw_indirect_count,
                        .target_phase = phase,
                        .logical_view =
                            sort_view.logical_view,
                        .logical_view_origin =
                            sort_view.origin,
                        .logical_view_forward =
                            sort_view.forward,
                    };
                };
            std::vector<CompiledDrawQueueVariant>
                variants;
            variants.reserve(2);
            variants.push_back({
                .phase =
                    DrawQueuePhase::opaque,
                .sort_view_index = 0,
                .queue =
                    DrawQueueBuilder::build(
                        request(
                            DrawQueuePhase::
                                opaque),
                        opaque_provider),
            });
            variants.push_back({
                .phase =
                    DrawQueuePhase::
                        transparent,
                .sort_view_index = 0,
                .queue =
                    DrawQueueBuilder::build(
                        request(
                            DrawQueuePhase::
                                transparent),
                        transparent_provider),
            });
            return CompiledDrawQueueSet::
                combine(std::move(variants));
        };

    const auto intersects_clip_plane =
        [](const DrawWorldBounds &bounds,
           const RenderViewClipPlane &plane) {
            float maximum_distance =
                plane.offset;
            for (std::size_t axis = 0;
                 axis < 3; ++axis) {
                maximum_distance +=
                    plane.normal[
                        static_cast<
                            glm::length_t>(axis)] >=
                            0.0f
                        ? plane.normal[
                              static_cast<
                                  glm::length_t>(axis)] *
                              bounds.maximum[axis]
                        : plane.normal[
                              static_cast<
                                  glm::length_t>(axis)] *
                              bounds.minimum[axis];
            }
            return maximum_distance >= 0.0f;
        };

    std::size_t next_slot = 0;
    for (const auto &family :
         view_families.families) {
        if (family.family_id ==
            mainRenderViewFamilyId) {
            continue;
        }
        if (family.views.size() >
            maxPreparedSecondaryViewFamilyViews -
                next_slot) {
            // Culling is an optimization. An unusually large user family
            // remains correct through the canonical indirect queue.
            continue;
        }
        PreparedViewFamilyDraws prepared{
            .slot_base = next_slot,
            .visible_draw_counts =
                std::vector<std::size_t>(
                    family.views.size(), 0),
        };
        const auto local_sort =
            locally_sorted.contains(
                family.family_id);
        if (local_sort) {
            prepared.view_queues.reserve(
                family.views.size());
        }
        for (std::size_t view_index = 0;
             view_index < family.views.size();
             ++view_index) {
            const auto &view =
                family.views[view_index];
            const auto view_projection =
                view.projection * view.view;
            for (glm::length_t column = 0;
                 column < 4; ++column) {
                for (glm::length_t row = 0;
                     row < 4; ++row) {
                    if (!std::isfinite(
                            view_projection[column]
                                           [row])) {
                        throw std::invalid_argument(
                            "view-family culling requires finite view "
                            "projection matrices");
                    }
                }
            }

            const CompiledDrawQueueSet
                *view_queue =
                    &compiled_draw_queue;
            std::uint32_t sort_view_index =
                0;
            if (local_sort) {
                prepared.view_queues.push_back(
                    build_local_queue(view));
                view_queue =
                    &prepared.view_queues.back();
            }
            const auto &source_commands =
                view_queue->indirectRecords();
            const auto command_count =
                view_queue
                    ->queue(
                        DrawQueuePhase::opaque,
                        sort_view_index)
                    .indirectRecords()
                    .size() +
                view_queue
                    ->queue(
                        DrawQueuePhase::
                            transparent,
                        sort_view_index)
                    .indirectRecords()
                    .size();
            if (command_count >
                    maxRenderCommands ||
                command_count >
                    source_commands.size()) {
                throw std::runtime_error(
                    "secondary sorted draw queue exceeds its prepared "
                    "view slot");
            }
            std::vector<RenderCommand>
                commands{
                    source_commands.begin(),
                    source_commands.begin() +
                        static_cast<
                            std::ptrdiff_t>(
                            command_count)};
            std::size_t command = 0;
            for (const auto phase :
                 {DrawQueuePhase::opaque,
                  DrawQueuePhase::
                      transparent}) {
                for (const auto &item :
                     view_queue
                         ->queue(
                             phase,
                             sort_view_index)
                         .orderedItems()) {
                    if (command >=
                        commands.size()) {
                        throw std::runtime_error(
                            "secondary draw queue items exceed command "
                            "records");
                    }
                    bool visible =
                        !item.world_bounds
                             .has_value();
                    if (!visible) {
                        visible =
                            intersectsZeroToOneClipFrustum(
                                *item.world_bounds,
                                view_projection);
                        if (visible &&
                            view.clip_plane) {
                            visible =
                                intersects_clip_plane(
                                    *item.world_bounds,
                                    *view.clip_plane);
                        }
                    }
                    if (visible) {
                        ++prepared
                              .visible_draw_counts
                                  [view_index];
                    } else {
                        commands[command]
                            .command
                            .instanceCount = 0;
                    }
                    ++command;
                }
            }
            if (command != commands.size()) {
                throw std::runtime_error(
                    "secondary draw queue command and item counts differ");
            }
            if (!commands.empty()) {
                const auto slot =
                    next_slot + view_index;
                GET_MODULE(VulkanManageCore)
                    .writeBuf(
                        view_family_indirect_buf,
                        commands.data(),
                        static_cast<vk::DeviceSize>(
                            slot *
                            maxRenderCommands *
                            sizeof(RenderCommand)),
                        commands.size() *
                            sizeof(RenderCommand));
            }
        }
        next_slot += family.views.size();
        prepared_view_family_draws.emplace(
            family.family_id,
            std::move(prepared));
    }
}

const BufferWrapper &
PolygonInstanceContainer::
    viewFamilyIndirectBuffer() const {
    return view_family_indirect_buf;
}

std::optional<vk::DeviceSize>
PolygonInstanceContainer::viewFamilyDrawOffset(
    std::string_view family_id,
    std::uint32_t view_index) const {
    const auto found =
        prepared_view_family_draws.find(
            family_id);
    if (found ==
        prepared_view_family_draws.end()) {
        return std::nullopt;
    }
    if (view_index >=
        found->second
            .visible_draw_counts.size()) {
        throw std::out_of_range(
            "prepared view-family draw index is out of range");
    }
    return static_cast<vk::DeviceSize>(
               found->second.slot_base +
               view_index) *
           maxRenderCommands *
           sizeof(RenderCommand);
}

const std::vector<DrawIndirectInfo> &
PolygonInstanceContainer::
    getViewFamilyDrawCalls(
        std::string_view family_id,
        std::uint32_t view_index,
        bool first_person_view,
        std::optional<MaterialPhase> phase,
        std::optional<
            MaterialDrawTagFilterId>
            material_filter) const {
    const auto found =
        prepared_view_family_draws.find(
            family_id);
    if (found ==
        prepared_view_family_draws.end()) {
        throw std::out_of_range(
            "prepared view-family draw queue is unavailable");
    }
    if (view_index >=
        found->second
            .visible_draw_counts.size()) {
        throw std::out_of_range(
            "prepared view-family draw queue index is out of range");
    }
    if (found->second.view_queues.empty()) {
        return getDrawCalls(
            first_person_view, phase, 0,
            material_filter);
    }
    if (found->second.view_queues.size() !=
        found->second
            .visible_draw_counts.size()) {
        throw std::runtime_error(
            "prepared view-family local draw queues are incomplete");
    }
    const auto visibility =
        first_person_view
            ? DrawQueueView::first_person
            : DrawQueueView::third_person;
    const auto &queue =
        found->second
            .view_queues[view_index];
    if (!phase) {
        return queue.allDrawRanges(
            0, visibility,
            material_filter);
    }
    return queue.drawRanges(
        *phase == MaterialPhase::opaque
            ? DrawQueuePhase::opaque
            : DrawQueuePhase::
                  transparent,
        0, visibility,
        material_filter);
}

SceneDrawCandidatesV1
PolygonInstanceContainer::
    sceneDrawCandidatesForFrameGraph() const {
    SceneDrawCandidatesV1 result;
    const auto &records =
        compiled_draw_queue.indirectRecords();
    result.commands.reserve(records.size());
    result.bounds.reserve(records.size());
    if (records.empty()) {
        return result;
    }
    result.segments =
        compiled_draw_queue.sceneDrawSegments();
    for (const auto &record : records) {
        result.commands.push_back(record.command);
    }
    for (std::uint32_t view = 0;
         view < compiled_draw_queue.sortViewCount();
         ++view) {
        for (const auto phase :
             {DrawQueuePhase::opaque,
              DrawQueuePhase::transparent}) {
            for (const auto &item :
                 compiled_draw_queue
                     .queue(phase, view)
                     .orderedItems()) {
                SceneDrawBoundsV1 packed;
                if (item.world_bounds) {
                    packed.minimum = {
                        item.world_bounds->minimum[0],
                        item.world_bounds->minimum[1],
                        item.world_bounds->minimum[2],
                        1.0F,
                    };
                    packed.maximum = {
                        item.world_bounds->maximum[0],
                        item.world_bounds->maximum[1],
                        item.world_bounds->maximum[2],
                        0.0F,
                    };
                }
                result.bounds.push_back(packed);
            }
        }
    }
    if (result.commands.size() !=
        result.bounds.size()) {
        throw std::runtime_error(
            "scene draw command and bounds publications are misaligned");
    }
    for (std::size_t index = 0;
         index < result.segments.size();
         ++index) {
        const auto &segment =
            result.segments[index];
        if (segment.output_count_index !=
                index ||
            segment.source_first_command >
                result.commands.size() ||
            segment.command_capacity >
                result.commands.size() -
                    segment.source_first_command) {
            throw std::runtime_error(
                "scene draw segment publication is invalid");
        }
    }
    return result;
}
const BufferWrapper &PolygonInstanceContainer::getObjectBuf() const { return model_data_buffer; }
const BufferWrapper &PolygonInstanceContainer::getPreviousObjectBuf() const { return previous_model_data_buffer; }
const std::vector<DrawIndirectInfo> &
PolygonInstanceContainer::getDrawCalls(
    bool first_person_view, std::optional<MaterialPhase> phase,
    std::uint32_t sort_view_index,
    std::optional<MaterialDrawTagFilterId>
        material_filter) const {
    const auto visibility = first_person_view
                                ? DrawQueueView::first_person
                                : DrawQueueView::third_person;
    if (!phase) {
        return compiled_draw_queue.allDrawRanges(
            sort_view_index, visibility,
            material_filter);
    }
    return compiled_draw_queue.drawRanges(
        *phase == MaterialPhase::opaque ? DrawQueuePhase::opaque
                                        : DrawQueuePhase::transparent,
        sort_view_index, visibility,
        material_filter);
}

} // namespace Pelican
