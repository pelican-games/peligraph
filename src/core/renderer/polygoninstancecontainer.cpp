#include "polygoninstancecontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../vkcore/core.hpp"
#include <algorithm>
#include <array>
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

uint32_t checkedDrawCount(size_t count) {
    if (count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Draw call count exceeds uint32_t range");
    }
    return static_cast<uint32_t>(count);
}

} // namespace

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
          createIndirectBuf(GET_MODULE(VulkanManageCore), maxRenderCommands),
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
} // namespace

ModelInstanceId PolygonInstanceContainer::placeModelInstance(const ModelTemplate &model) {
    if (model_instances_data.size() >= maxModelInstances) {
        throw std::runtime_error("Model instance capacity exceeded");
    }

    size_t primitive_count = 0;
    for (const auto &material : model.material_primitives) {
        primitive_count += material.primitives.size();
    }
    if (render_commands.size() > maxRenderCommands ||
        primitive_count > maxRenderCommands - render_commands.size()) {
        throw std::runtime_error("Render command capacity exceeded");
    }

    ModelInstanceId id{static_cast<uint32_t>(model_instances_data.size())};
    model_instances_data.push_back(glm::identity<glm::mat4>());
    previous_model_instances_data.push_back(glm::identity<glm::mat4>());
    model_history_valid.push_back(false);
    skin_palettes.emplace_back();
    previous_skin_palettes.emplace_back();
    animation_revisions.push_back(0);
    previous_animation_revisions.push_back(0);
    animation_generations.push_back(1);
    model_asset_ids.push_back(model.asset_id);
    material_initial_value_tables.push_back(model.material_initial_values);
    vrm_semantics.push_back(model.vrm_semantic);
    morph_layouts.push_back(model.morph_targets);
    const auto defaults = model.morph_targets
                              ? model.morph_targets->default_weights
                              : std::vector<float>{};
    morph_weight_frames.push_back(MorphWeightFrame{
        .instance_identity = static_cast<std::uint64_t>(id.value) + 1,
        .instance_generation = animation_generations.back(),
        .layout_generation = model.morph_targets ? model.morph_targets->generation : 0,
        .current_revision = 0,
        .previous_revision = 0,
        .current = defaults,
        .previous = defaults,
    });
    morph_history_valid.push_back(false);

    for (const auto &material : model.material_primitives) {
        for (const auto &primitive : material.primitives) {
            RenderCommand instance{
                .command =
                    vk::DrawIndexedIndirectCommand{
                        primitive.index_count,
                        1,
                        primitive.index_offset,
                        primitive.vert_offset,
                        id.value,
                },
                .material = material.material,
                .source_material_index = material.source_material_index,
                .skinned = primitive.skinned,
            };
            render_commands.push_back(instance);
        }
    }
    if (model.skeletal) {
        setSkinningPalette(id, evaluateSkinPalette(*model.skeletal, nullptr, 0.0, 1.0, false, 0.0));
    }
    return id;
}
void PolygonInstanceContainer::removeModelInstance(ModelInstanceId id) {
    if (id.value >= model_instances_data.size()) {
        return;
    }

    const auto first_instance = id.value;
    std::erase_if(render_commands, [first_instance](const RenderCommand &command) {
        return command.command.firstInstance == first_instance;
    });
    model_instances_data[id.value] = glm::identity<glm::mat4>();
    previous_model_instances_data[id.value] = glm::identity<glm::mat4>();
    model_history_valid[id.value] = false;
    skin_palettes[id.value].clear();
    previous_skin_palettes[id.value].clear();
    animation_revisions[id.value] = 0;
    previous_animation_revisions[id.value] = 0;
    model_asset_ids[id.value] = {};
    material_initial_value_tables[id.value].reset();
    vrm_semantics[id.value].reset();
    if (++animation_generations[id.value] == 0) ++animation_generations[id.value];
    morph_layouts[id.value].reset();
    morph_weight_frames[id.value] = MorphWeightFrame{
        .instance_identity = static_cast<std::uint64_t>(id.value) + 1,
        .instance_generation = animation_generations[id.value],
    };
    morph_history_valid[id.value] = false;
    material_override_frames.erase(id.value);
    const MaterialInstanceOverrideGpuData empty{};
    GET_MODULE(VulkanManageCore).writeBuf(
        material_override_buffer, &empty,
        sizeof(MaterialInstanceOverrideGpuData) * id.value, sizeof(empty));
    const auto identity = static_cast<std::uint64_t>(id.value) + 1;
    std::erase_if(material_absolute_override_frames,
                  [identity](const auto &entry) {
                      return entry.first.instance_identity == identity;
                  });
    uploadMaterialAbsoluteOverrides();
}

void PolygonInstanceContainer::clear() {
    render_commands.clear();
    draw_calls.clear();
    model_instances_data.clear();
    previous_model_instances_data.clear();
    model_history_valid.clear();
    skin_palettes.clear();
    previous_skin_palettes.clear();
    animation_revisions.clear();
    previous_animation_revisions.clear();
    animation_generations.clear();
    model_asset_ids.clear();
    material_initial_value_tables.clear();
    vrm_semantics.clear();
    morph_layouts.clear();
    morph_weight_frames.clear();
    morph_history_valid.clear();
    material_override_frames.clear();
    material_absolute_override_frames.clear();
    const std::vector<MaterialInstanceOverrideGpuData> empty(maxModelInstances);
    GET_MODULE(VulkanManageCore).writeBuf(material_override_buffer, empty.data(), 0,
                                          sizeof(MaterialInstanceOverrideGpuData) *
                                              empty.size());
    uploadMaterialAbsoluteOverrides();
}

void PolygonInstanceContainer::triggerUpdate() {
    // clear previous frame
    draw_calls.clear();

    if (render_commands.empty())
        return;

    for (size_t i = 0; i < model_instances_data.size(); ++i) {
        if (!model_history_valid[i]) {
            previous_model_instances_data[i] = model_instances_data[i];
            previous_skin_palettes[i] = skin_palettes[i];
        }
        if (!morph_history_valid[i]) {
            morph_weight_frames[i].previous = morph_weight_frames[i].current;
            morph_weight_frames[i].previous_revision =
                morph_weight_frames[i].current_revision;
        }
        if (!previous_skin_palettes[i].empty()) {
            GET_MODULE(VulkanManageCore)
                .writeBuf(previous_skin_palette_buffer, previous_skin_palettes[i].data(),
                          sizeof(glm::mat4) * maxSkinJoints * i,
                          sizeof(glm::mat4) * previous_skin_palettes[i].size());
        }
    }

    std::vector<MorphInstanceGpuData> morph_instances(model_instances_data.size());
    for (size_t i = 0; i < morph_weight_frames.size(); ++i) {
        const auto &frame = morph_weight_frames[i];
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

    // prepare indirect buffer
    std::sort(render_commands.begin(), render_commands.end(),
              [](const RenderCommand &p, const RenderCommand &q) {
                  return std::tie(p.material.value, p.source_material_index,
                                  p.skinned) <
                         std::tie(q.material.value, q.source_material_index,
                                  q.skinned);
              });
    GET_MODULE(VulkanManageCore)
        .writeBuf(indirect_buf, render_commands.data(), 0, sizeof(RenderCommand) * render_commands.size());

    // prepare drawindirect information
    DrawIndirectInfo draw_call{.stride = sizeof(RenderCommand)};
    size_t prev_offset_index;

    prev_offset_index = 0;
    draw_call.material = render_commands[0].material;
    draw_call.source_material_index = render_commands[0].source_material_index;
    draw_call.skinned = render_commands[0].skinned;
    draw_call.offset = prev_offset_index * sizeof(RenderCommand);
    for (size_t i = 1; i < render_commands.size(); i++) {
        if (render_commands[i].material.value != render_commands[i - 1].material.value ||
            render_commands[i].source_material_index !=
                render_commands[i - 1].source_material_index ||
            render_commands[i].skinned != render_commands[i - 1].skinned) {
            draw_call.draw_count = checkedDrawCount(i - prev_offset_index);
            draw_calls.push_back(draw_call);

            prev_offset_index = i;
            draw_call.material = render_commands[i].material;
            draw_call.source_material_index =
                render_commands[i].source_material_index;
            draw_call.skinned = render_commands[i].skinned;
            draw_call.offset = prev_offset_index * sizeof(RenderCommand);
        }
    }
    draw_call.draw_count = checkedDrawCount(render_commands.size() - prev_offset_index);
    draw_calls.push_back(draw_call);

    GET_MODULE(VulkanManageCore)
        .writeBuf(model_data_buffer, model_instances_data.data(), 0, sizeof(glm::mat4) * model_instances_data.size());
    GET_MODULE(VulkanManageCore)
        .writeBuf(previous_model_data_buffer, previous_model_instances_data.data(), 0,
                  sizeof(glm::mat4) * previous_model_instances_data.size());
}

void PolygonInstanceContainer::commitFrameHistory() {
    advanceTemporalHistoryAfterRender();
}

void PolygonInstanceContainer::advanceMorphHistoryAfterRender() {
    for (auto &frame : morph_weight_frames) {
        frame.previous = frame.current;
        frame.previous_revision = frame.current_revision;
    }
    std::fill(morph_history_valid.begin(), morph_history_valid.end(), true);
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
    previous_model_instances_data = model_instances_data;
    previous_skin_palettes = skin_palettes;
    previous_animation_revisions = animation_revisions;
    advanceMorphHistoryAfterRender();
    advanceMaterialOverrideHistoryAfterRender();
    std::fill(model_history_valid.begin(), model_history_valid.end(), true);
    ++temporal_history_advance_count;
}

void PolygonInstanceContainer::resetTemporalHistory() {
    previous_model_instances_data = model_instances_data;
    previous_skin_palettes = skin_palettes;
    previous_animation_revisions = animation_revisions;
    for (auto &frame : morph_weight_frames) {
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
    std::fill(model_history_valid.begin(), model_history_valid.end(), false);
    std::fill(morph_history_valid.begin(), morph_history_valid.end(), false);
}

bool PolygonInstanceContainer::canRebuildModelInstances(
    std::span<const ModelInstanceRebuild> replacements) const {
    std::unordered_map<std::uint64_t, const ModelTemplate *> by_asset;
    for (const auto &replacement : replacements) {
        if (!isValidModelAssetId(replacement.asset_id) || replacement.replacement == nullptr ||
            !by_asset.emplace(replacement.asset_id.value, replacement.replacement).second)
            return false;
    }

    size_t command_count = render_commands.size();
    for (const auto &command : render_commands) {
        const auto instance = command.command.firstInstance;
        if (instance < model_asset_ids.size() &&
            by_asset.contains(model_asset_ids[instance].value)) {
            --command_count;
        }
    }
    for (const auto &[asset, replacement] : by_asset) {
        size_t instances = 0;
        for (const auto current : model_asset_ids)
            if (current.value == asset) ++instances;
        const auto primitives = primitiveCount(*replacement);
        if (instances != 0 && primitives > (maxRenderCommands - command_count) / instances)
            return false;
        command_count += instances * primitives;
    }
    return command_count <= maxRenderCommands;
}

void PolygonInstanceContainer::rebuildModelInstances(
    std::span<const ModelInstanceRebuild> replacements) {
    if (!canRebuildModelInstances(replacements))
        throw std::runtime_error("Render command capacity exceeded by model reload batch");
    std::unordered_map<std::uint64_t, const ModelTemplate *> by_asset;
    std::unordered_map<std::uint64_t, std::vector<glm::mat4>> rest_palettes;
    for (const auto &replacement : replacements) {
        by_asset.emplace(replacement.asset_id.value, replacement.replacement);
        auto &palette = rest_palettes[replacement.asset_id.value];
        if (replacement.replacement->skeletal) {
            palette = evaluateSkinPalette(*replacement.replacement->skeletal, nullptr,
                                          0.0, 1.0, false, 0.0);
        }
    }

    std::vector<RenderCommand> rebuilt;
    rebuilt.reserve(maxRenderCommands);
    for (const auto &command : render_commands) {
        const auto instance = command.command.firstInstance;
        if (instance < model_asset_ids.size() &&
            by_asset.contains(model_asset_ids[instance].value))
            continue;
        rebuilt.push_back(command);
    }

    auto next_skin_palettes = skin_palettes;
    auto next_previous_skin_palettes = previous_skin_palettes;
    auto next_animation_revisions = animation_revisions;
    auto next_previous_animation_revisions = previous_animation_revisions;
    auto next_animation_generations = animation_generations;
    auto next_material_initial_value_tables = material_initial_value_tables;
    auto next_vrm_semantics = vrm_semantics;
    auto next_morph_layouts = morph_layouts;
    auto next_morph_weight_frames = morph_weight_frames;
    auto next_morph_history_valid = morph_history_valid;
    auto next_material_override_frames = material_override_frames;
    auto next_material_absolute_override_frames =
        material_absolute_override_frames;
    auto next_previous_models = previous_model_instances_data;
    auto next_history_valid = model_history_valid;
    for (uint32_t instance = 0; instance < model_asset_ids.size(); ++instance) {
        const auto found = by_asset.find(model_asset_ids[instance].value);
        if (found == by_asset.end()) continue;
        const auto &replacement = *found->second;
        for (const auto &material : replacement.material_primitives) {
            for (const auto &primitive : material.primitives) {
                rebuilt.push_back(RenderCommand{
                    .command = vk::DrawIndexedIndirectCommand{
                        primitive.index_count, 1, primitive.index_offset,
                        primitive.vert_offset, instance},
                    .material = material.material,
                    .source_material_index = material.source_material_index,
                    .skinned = primitive.skinned,
                });
            }
        }

        const auto &rest_palette = rest_palettes.at(model_asset_ids[instance].value);
        next_skin_palettes[instance] = rest_palette;
        next_previous_skin_palettes[instance] = rest_palette;
        if (!rest_palette.empty()) {
            GET_MODULE(VulkanManageCore).writeBuf(
                skin_palette_buffer, rest_palette.data(),
                sizeof(glm::mat4) * maxSkinJoints * instance,
                sizeof(glm::mat4) * rest_palette.size());
        }
        next_animation_revisions[instance] = 0;
        next_previous_animation_revisions[instance] = 0;
        if (++next_animation_generations[instance] == 0)
            ++next_animation_generations[instance];
        next_material_initial_value_tables[instance] =
            replacement.material_initial_values;
        next_vrm_semantics[instance] = replacement.vrm_semantic;
        next_morph_layouts[instance] = replacement.morph_targets;
        const auto defaults = replacement.morph_targets
                                  ? replacement.morph_targets->default_weights
                                  : std::vector<float>{};
        next_morph_weight_frames[instance] = MorphWeightFrame{
            .instance_identity = static_cast<std::uint64_t>(instance) + 1,
            .instance_generation = next_animation_generations[instance],
            .layout_generation = replacement.morph_targets
                                     ? replacement.morph_targets->generation
                                     : 0,
            .current_revision = 0,
            .previous_revision = 0,
            .current = defaults,
            .previous = defaults,
        };
        next_morph_history_valid[instance] = false;
        next_material_override_frames.erase(instance);
        const auto identity = static_cast<std::uint64_t>(instance) + 1;
        std::erase_if(next_material_absolute_override_frames,
                      [identity](const auto &entry) {
                          return entry.first.instance_identity == identity;
                      });
        const MaterialInstanceOverrideGpuData empty{};
        GET_MODULE(VulkanManageCore).writeBuf(
            material_override_buffer, &empty,
            sizeof(MaterialInstanceOverrideGpuData) * instance, sizeof(empty));
        next_previous_models[instance] = model_instances_data[instance];
        next_history_valid[instance] = false;
    }
    render_commands = std::move(rebuilt);
    skin_palettes = std::move(next_skin_palettes);
    previous_skin_palettes = std::move(next_previous_skin_palettes);
    animation_revisions = std::move(next_animation_revisions);
    previous_animation_revisions = std::move(next_previous_animation_revisions);
    animation_generations = std::move(next_animation_generations);
    material_initial_value_tables =
        std::move(next_material_initial_value_tables);
    vrm_semantics = std::move(next_vrm_semantics);
    morph_layouts = std::move(next_morph_layouts);
    morph_weight_frames = std::move(next_morph_weight_frames);
    morph_history_valid = std::move(next_morph_history_valid);
    material_override_frames = std::move(next_material_override_frames);
    material_absolute_override_frames =
        std::move(next_material_absolute_override_frames);
    previous_model_instances_data = std::move(next_previous_models);
    model_history_valid = std::move(next_history_valid);
    uploadMaterialAbsoluteOverrides();
}

void PolygonInstanceContainer::rebuildModelInstances(
    ModelAssetId asset_id, const ModelTemplate &replacement) {
    const ModelInstanceRebuild request{asset_id, &replacement};
    rebuildModelInstances(std::span{&request, std::size_t{1}});
}

size_t PolygonInstanceContainer::instanceCountForAssetForTesting(ModelAssetId asset_id) const {
    return static_cast<size_t>(std::count(model_asset_ids.begin(), model_asset_ids.end(), asset_id));
}

void PolygonInstanceContainer::setSkinningPalette(ModelInstanceId id,
                                                  std::span<const glm::mat4> palette) {
    if (id.value >= model_instances_data.size()) throw std::runtime_error("Model instance not found");
    if (palette.size() > maxSkinJoints) throw std::runtime_error("Skin palette exceeds 128 joints");
    skin_palettes[id.value].assign(palette.begin(), palette.end());
    GET_MODULE(VulkanManageCore).writeBuf(skin_palette_buffer, palette.data(),
                                         sizeof(glm::mat4) * maxSkinJoints * id.value,
                                         sizeof(glm::mat4) * palette.size());
}

Animation::InstanceHandle PolygonInstanceContainer::animationInstance(ModelInstanceId id) const {
    if (id.value >= animation_generations.size()) return Animation::invalidHandle<Animation::InstanceHandle>();
    return {static_cast<std::uint64_t>(id.value) + 1, animation_generations[id.value], 0};
}

Animation::Status PolygonInstanceContainer::publishAnimationFrame(
    ModelInstanceId id, const Animation::PublishAnimationFrameDescV1 &frame) {
    constexpr auto minimum = offsetof(Animation::PublishAnimationFrameDescV1, root_delta) +
                             sizeof(Animation::RootDeltaV1);
    if (frame.struct_size < minimum) return Animation::Status::invalid_argument;
    if (frame.version != Animation::descriptorVersionV1) return Animation::Status::unsupported_version;
    if (frame.reserved0 != 0 || frame.reserved1 != 0) return Animation::Status::reserved_not_zero;
    if (id.value >= skin_palettes.size()) return Animation::Status::invalid_handle;
    const auto expected = animationInstance(id);
    if (!Animation::isValid(frame.instance) || frame.instance.identity != expected.identity)
        return Animation::Status::invalid_handle;
    if (frame.instance.generation != expected.generation) return Animation::Status::stale_generation;
    if (!Animation::isValid(frame.local_pose) || !Animation::isValid(frame.model_pose) ||
        frame.frame_revision == 0 || (frame.palette_count != 0 && frame.palette == nullptr) ||
        frame.palette_count > maxSkinJoints ||
        (frame.flags & ~(Animation::commit_reset_history | Animation::commit_discontinuity)) != 0)
        return Animation::Status::invalid_argument;
    if (animation_revisions[id.value] != 0 && frame.frame_revision <= animation_revisions[id.value])
        return Animation::Status::duplicate_revision;

    std::vector<glm::mat4> palette(frame.palette_count);
    if (!palette.empty()) std::memcpy(palette.data(), frame.palette, palette.size() * sizeof(glm::mat4));
    if (!palette.empty()) {
        GET_MODULE(VulkanManageCore).writeBuf(skin_palette_buffer, palette.data(),
                                             sizeof(glm::mat4) * maxSkinJoints * id.value,
                                             sizeof(glm::mat4) * palette.size());
    }
    skin_palettes[id.value] = std::move(palette);
    animation_revisions[id.value] = frame.frame_revision;
    if (previous_animation_revisions[id.value] == 0 ||
        (frame.flags & (Animation::commit_reset_history | Animation::commit_discontinuity)) != 0) {
        previous_skin_palettes[id.value] = skin_palettes[id.value];
        previous_animation_revisions[id.value] = frame.frame_revision;
    }
    return Animation::Status::ok;
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
    if (id.value >= morph_weight_frames.size()) return Animation::Status::invalid_handle;
    const auto &layout = morph_layouts[id.value];
    if (!layout || frame.layout_generation != layout->generation)
        return Animation::Status::stale_generation;
    if (frame.frame_revision == 0 || frame.weight_count != layout->default_weights.size() ||
        (frame.weight_count != 0 && frame.weights == nullptr) ||
        frame.weight_count > maxMorphWeightsPerInstance ||
        (frame.flags & ~(morphCommitResetHistory | morphCommitDiscontinuity)) != 0)
        return Animation::Status::invalid_argument;
    const auto &current = morph_weight_frames[id.value];
    if (current.current_revision != 0 &&
        frame.frame_revision <= current.current_revision)
        return Animation::Status::duplicate_revision;
    std::vector<float> weights;
    if (frame.weight_count != 0)
        weights.assign(frame.weights, frame.weights + frame.weight_count);
    if (std::any_of(weights.begin(), weights.end(),
                    [](float value) { return !std::isfinite(value); }))
        return Animation::Status::invalid_argument;

    auto &destination = morph_weight_frames[id.value];
    destination.current = std::move(weights);
    destination.current_revision = frame.frame_revision;
    if (!morph_history_valid[id.value] ||
        (frame.flags & (morphCommitResetHistory | morphCommitDiscontinuity)) != 0) {
        destination.previous = destination.current;
        destination.previous_revision = destination.current_revision;
        morph_history_valid[id.value] = false;
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
    if (id.value >= model_instances_data.size())
        return Animation::Status::invalid_handle;

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

    const auto found = material_override_frames.find(id.value);
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

    const MaterialInstanceOverrideGpuData gpu{
        .base_color_factor = next.current.base_color_factor,
        .emissive_factor = next.current.emissive_factor,
        .uv_offset_scale = glm::vec4{next.current.uv_offset,
                                     next.current.uv_scale},
        .uv_rotation_reserved = glm::vec4{next.current.uv_rotation, 0.0f,
                                          0.0f, 0.0f},
        .metadata = glm::uvec4{
            next.current.mask, next.instance_generation,
            static_cast<std::uint32_t>(next.current_revision),
            static_cast<std::uint32_t>(next.current_revision >> 32u)},
    };
    GET_MODULE(VulkanManageCore).writeBuf(
        material_override_buffer, &gpu,
        sizeof(MaterialInstanceOverrideGpuData) * id.value, sizeof(gpu));
    material_override_frames.insert_or_assign(id.value, std::move(next));
    return Animation::Status::ok;
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
    if (id.value >= model_instances_data.size())
        return Animation::Status::invalid_handle;

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

    const auto &initial_values = material_initial_value_tables[id.value];
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
    if (id.value >= model_instances_data.size())
        return Animation::Status::invalid_handle;

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
        previous_morph = morph_weight_frames.at(id.value);
        previous_morph_history_valid = morph_history_valid.at(id.value);
        previous_materials = material_absolute_override_frames;
    } catch (...) {
        return Animation::Status::out_of_memory;
    }

    const auto rollback = [&]() noexcept {
        try {
            morph_weight_frames[id.value] = previous_morph;
            morph_history_valid[id.value] = previous_morph_history_valid;
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
        instance.identity > animation_generations.size())
        return std::nullopt;
    const auto index = static_cast<std::uint32_t>(instance.identity - 1);
    if (animation_generations[index] != instance.generation ||
        index >= vrm_semantics.size() || !vrm_semantics[index])
        return std::nullopt;
    return VrmApplicationModelView{
        .model_instance = ModelInstanceId{index},
        .instance = instance,
        .semantic = vrm_semantics[index],
        .morph_layout = morph_layouts[index],
        .material_initial_values = material_initial_value_tables[index],
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

void PolygonInstanceContainer::setTrs(ModelInstanceId id, glm::vec3 pos, glm::quat rotation, glm::vec3 scale) {
    if (id.value >= model_instances_data.size()) {
        throw std::runtime_error("Model instance not found");
    }

    model_instances_data[id.value] = glm::translate(glm::identity<glm::mat4>(), pos) * glm::toMat4(rotation) *
                                     glm::scale(glm::identity<glm::mat4>(), scale);
}

const BufferWrapper &PolygonInstanceContainer::getIndirectBuf() const { return indirect_buf; }
const BufferWrapper &PolygonInstanceContainer::getObjectBuf() const { return model_data_buffer; }
const BufferWrapper &PolygonInstanceContainer::getPreviousObjectBuf() const { return previous_model_data_buffer; }
const std::vector<DrawIndirectInfo> &PolygonInstanceContainer::getDrawCalls() const { return draw_calls; }

} // namespace Pelican
