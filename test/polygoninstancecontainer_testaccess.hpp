#pragma once

// The reads and pokes only a test needs on PolygonInstanceContainer. They used
// to be ~25 public `*ForTesting` members on the container itself, which made a
// test-only surface indistinguishable from the engine API. They live here now,
// behind the one friend the container declares, so the container's public
// header states only what production calls.
//
// The five counters `editorruntimefactory.cpp` publishes in its preview-state
// JSON stay public on the container; they are production readers, not tests.

#include "../src/core/renderer/polygoninstancecontainer.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::test_support {

struct PolygonInstanceContainerTestAccess {
    using Container = PolygonInstanceContainer;

    static std::size_t
    liveModelInstanceCount(const Container &instances) noexcept {
        std::size_t result = 0;
        for (std::uint32_t index = 0;
             index < instances.slots.instance_slots.slotCount(); ++index) {
            if (instances.slots.instance_slots.alive(index)) ++result;
        }
        return result;
    }

    static std::uint64_t
    nextDrawDeclarationOrdinal(const Container &instances) noexcept {
        return instances.next_draw_declaration_ordinal;
    }

    // The whole published per-slot table, so a test can compare it against the
    // candidate a publication swapped in.
    static const ModelInstanceSlotTable &slotTable(const Container &instances) {
        return instances.slots;
    }

    static std::size_t
    directionalShadowDrawViewCount(const Container &instances) {
        const auto found = instances.prepared_view_family_draws.find(
            directionalShadowRenderViewFamilyId);
        return found == instances.prepared_view_family_draws.end()
                   ? 0
                   : found->second.visible_draw_counts.size();
    }

    static std::size_t
    directionalShadowVisibleDrawCount(const Container &instances,
                                      std::uint32_t view_index) {
        return viewFamilyVisibleDrawCount(
            instances, directionalShadowRenderViewFamilyId, view_index);
    }

    static std::size_t viewFamilyDrawViewCount(const Container &instances,
                                               std::string_view family_id) {
        const auto found =
            instances.prepared_view_family_draws.find(family_id);
        return found == instances.prepared_view_family_draws.end()
                   ? 0
                   : found->second.visible_draw_counts.size();
    }

    static std::size_t viewFamilyVisibleDrawCount(const Container &instances,
                                                  std::string_view family_id,
                                                  std::uint32_t view_index) {
        return instances.prepared_view_family_draws
            .at(std::string{family_id})
            .visible_draw_counts.at(view_index);
    }

    static std::vector<std::uint32_t>
    drawOrder(const Container &instances, MaterialPhase phase,
              std::uint32_t sort_view_index = 0) {
        const auto &items =
            instances.compiled_draw_queue
                .queue(phase == MaterialPhase::opaque
                           ? DrawQueuePhase::opaque
                           : DrawQueuePhase::transparent,
                       sort_view_index)
                .orderedItems();
        std::vector<std::uint32_t> result;
        result.reserve(items.size());
        for (const auto &item : items) {
            result.push_back(item.stable_identity.instance.index);
        }
        return result;
    }

    static std::vector<std::uint32_t>
    viewFamilyDrawOrder(const Container &instances, std::string_view family_id,
                        std::uint32_t view_index, MaterialPhase phase) {
        const auto &prepared =
            instances.prepared_view_family_draws.at(std::string{family_id});
        if (view_index >= prepared.visible_draw_counts.size()) {
            throw std::out_of_range(
                "prepared view-family draw order index is out of range");
        }
        if (prepared.view_queues.empty()) {
            return drawOrder(instances, phase, 0);
        }
        const auto &items =
            prepared.view_queues.at(view_index)
                .queue(phase == MaterialPhase::opaque
                           ? DrawQueuePhase::opaque
                           : DrawQueuePhase::transparent,
                       0)
                .orderedItems();
        std::vector<std::uint32_t> result;
        result.reserve(items.size());
        for (const auto &item : items) {
            result.push_back(item.stable_identity.instance.index);
        }
        return result;
    }

    static ModelInstanceId modelInstanceId(const Container &instances,
                                           std::uint32_t index) {
        return instances.slots.instance_slots.idAt(index);
    }

    static ModelInstanceId forceGeneration(Container &instances,
                                           ModelInstanceId id,
                                           std::uint32_t generation) {
        return instances.slots.instance_slots.forceGeneration(id, generation);
    }

    static void forceSceneEpoch(Container &instances, std::uint64_t epoch) {
        instances.slots.instance_slots.forceSceneEpoch(epoch);
    }

    static glm::mat4 currentModelMatrix(const Container &instances,
                                        ModelInstanceId id) {
        return instances.slots.model_instances_data.at(
            instances.requireLive(id, "currentModelMatrixForTesting"));
    }

    static glm::mat4 previousModelMatrix(const Container &instances,
                                         ModelInstanceId id) {
        return instances.slots.previous_model_instances_data.at(
            instances.requireLive(id, "previousModelMatrixForTesting"));
    }

    static std::size_t animationNodeCount(const Container &instances, ModelInstanceId id) {
        return instances.slots.animation_node_matrices.at(
            instances.requireLive(id, "animationNodeCountForTesting")).size();
    }

    static std::uint64_t currentAnimationRevision(const Container &instances,
                                                  ModelInstanceId id) {
        return instances.slots.animation_revisions.at(
            instances.requireLive(id, "currentAnimationRevisionForTesting"));
    }

    static std::uint64_t previousAnimationRevision(const Container &instances,
                                                   ModelInstanceId id) {
        return instances.slots.previous_animation_revisions.at(
            instances.requireLive(id, "previousAnimationRevisionForTesting"));
    }

    static std::uint32_t animationGeneration(const Container &instances,
                                             ModelInstanceId id) {
        return instances.slots.animation_generations.at(
            instances.requireLive(id, "animationGenerationForTesting"));
    }

    static const MorphWeightFrame &morphWeightFrame(const Container &instances,
                                                    ModelInstanceId id) {
        return instances.slots.morph_weight_frames.at(
            instances.requireLive(id, "morphWeightFrameForTesting"));
    }

    static std::uint64_t morphLayoutGeneration(const Container &instances,
                                               ModelInstanceId id) {
        const auto &layout = instances.slots.morph_layouts.at(
            instances.requireLive(id, "morphLayoutGenerationForTesting"));
        return layout ? layout->generation : 0;
    }

    static const MaterialInstanceOverrideFrame *
    materialOverrideFrame(const Container &instances, ModelInstanceId id) {
        const auto index =
            instances.requireLive(id, "materialOverrideFrameForTesting");
        const auto found = instances.material_override_frames.find(index);
        return found == instances.material_override_frames.end()
                   ? nullptr
                   : &found->second;
    }

    static const MaterialInstanceAbsoluteOverrideFrame *
    materialAbsoluteOverrideFrame(const Container &instances,
                                  ModelInstanceId id,
                                  std::uint32_t source_material_index) {
        const auto index = instances.requireLive(
            id, "materialAbsoluteOverrideFrameForTesting");
        const MaterialInstanceAbsoluteOverrideKey key{
            static_cast<std::uint64_t>(index) + 1, source_material_index};
        const auto found =
            instances.material_absolute_override_frames.find(key);
        return found == instances.material_absolute_override_frames.end()
                   ? nullptr
                   : &found->second;
    }

    static std::size_t instanceCountForAsset(const Container &instances,
                                             ModelAssetId asset_id) {
        std::size_t count = 0;
        for (std::size_t index = 0;
             index < instances.slots.model_asset_ids.size(); ++index) {
            if (instances.slots.instance_slots.alive(
                    static_cast<std::uint32_t>(index)) &&
                instances.slots.model_asset_ids[index] == asset_id)
                ++count;
        }
        return count;
    }

    static const std::vector<DrawItemSnapshot> &
    drawItems(const Container &instances) {
        return instances.draw_inventory;
    }

    static const CompiledDrawQueueSet &
    compiledDrawQueue(const Container &instances) {
        return instances.compiled_draw_queue;
    }

    static const std::vector<glm::mat4> &
    currentSkinPalette(const Container &instances, ModelInstanceId id) {
        return instances.slots.skin_palettes.at(
            instances.requireLive(id, "currentSkinPaletteForTesting"));
    }
};

} // namespace Pelican::test_support

// Short name for the call sites. This header is only ever included by tests,
// so the global-scope alias reaches no production translation unit.
using PolygonInstanceTestAccess =
    Pelican::test_support::PolygonInstanceContainerTestAccess;
