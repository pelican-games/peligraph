#pragma once

#include "../material/material.hpp"
#include "../model/modeltemplate.hpp"
#include "modelinstance.hpp"
#include "renderpolicyregistry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// GPU-consumed command record. The Vulkan command must remain the first member:
// drawIndexedIndirect reads it at each sizeof(RenderCommand) stride while the
// remaining fields are CPU-side range metadata.
struct RenderCommand {
    vk::DrawIndexedIndirectCommand command;
    GlobalMaterialId material;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    std::uint32_t node_index = noSourceNodeIndex;
    bool skinned = false;
    PrimitiveViewVisibility view_visibility = PrimitiveViewVisibility::both;
};

static_assert(offsetof(RenderCommand, command) == 0);

inline constexpr std::uint32_t noSceneDrawSegmentIndex =
    std::numeric_limits<std::uint32_t>::max();

struct DrawIndirectInfo {
    GlobalMaterialId material;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    vk::DeviceSize offset = 0;
    std::uint32_t draw_count = 0;
    std::uint32_t stride = 0;
    bool skinned = false;
    // Assigned when phase/view queues are combined into the canonical frame
    // publication. GPU segmented draw consumers use this to recover the
    // output command/count slots without deriving material state on the GPU.
    std::uint32_t scene_segment_index =
        noSceneDrawSegmentIndex;

    bool operator==(const DrawIndirectInfo &) const = default;
};

// GPU-visible fixed-state range. source_* addresses the packed
// scene_draw_commands_v1 array. output_* reserves a disjoint compacted range
// and one count slot so overlapping view/filter publications never race.
struct alignas(16) SceneDrawSegmentV1 {
    std::uint32_t source_first_command = 0;
    std::uint32_t command_capacity = 0;
    std::uint32_t output_first_command = 0;
    std::uint32_t output_count_index = 0;
    std::uint32_t sort_view_index = 0;
    std::uint32_t phase = 0;
    std::uint32_t visibility_view = 0;
    std::uint32_t material_filter_index =
        noSceneDrawSegmentIndex;

    bool operator==(const SceneDrawSegmentV1 &) const = default;
};

static_assert(sizeof(SceneDrawSegmentV1) == sizeof(std::uint32_t) * 8);

enum class DrawQueueView : std::uint8_t {
    third_person,
    first_person,
};

enum class DrawQueuePhase : std::uint8_t {
    mixed,
    opaque,
    transparent,
};

enum class DrawViewMask : std::uint8_t {
    none = 0,
    third_person = 1u << 0u,
    first_person = 1u << 1u,
    both = (1u << 0u) | (1u << 1u),
};

struct DrawIndexedArguments {
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0;

    bool operator==(const DrawIndexedArguments &) const = default;
};

// This is the state key used by the established renderer. A later policy may
// produce different sort keys, but it must not need to query a live material
// or pipeline container to recover these inputs.
struct DrawPipelineMaterialKey {
    GlobalMaterialId material;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    bool skinned = false;

    bool operator==(const DrawPipelineMaterialKey &) const = default;
};

struct DrawStableIdentity {
    ModelInstanceId instance{};
    std::uint32_t mesh_index = 0;
    std::uint32_t primitive_index = 0;
    std::uint32_t node_index = noSourceNodeIndex;

    bool operator==(const DrawStableIdentity &) const = default;
};

struct DrawWorldBounds {
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};

    bool operator==(const DrawWorldBounds &) const = default;
};

// Immutable input to queue compilation. RPE5 retains both the reference-space
// source and the current world-space envelope. Bounds remain optional for
// legacy or custom geometry that has not declared a safe envelope; policies
// that require them reject those items instead of guessing.
struct DrawItemSnapshot {
    DrawStableIdentity stable_identity{};
    std::uint64_t declaration_ordinal = 0;
    DrawIndexedArguments indexed{};
    DrawPipelineMaterialKey pipeline_material_key{};
    // Canonical material-side authoring tags. They are copied into this
    // immutable snapshot before queue compilation and never reach the Vulkan
    // execution loop.
    std::vector<std::string> material_tags;
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    MaterialPhase phase = MaterialPhase::opaque;
    std::shared_ptr<const ModelPrimitiveBoundsSource> bounds_source;
    std::optional<DrawWorldBounds> world_bounds;
    DrawViewMask view_mask = DrawViewMask::both;
    std::uint32_t vertex_count = 0;
    bool morph_deformed = false;
    bool vat_deformed = false;
    std::uint64_t geometry_allocation_id = 0;

    bool operator==(const DrawItemSnapshot &) const = default;
};

// Resolves the current deformation envelope without querying live modules.
// Morph deltas expand the indexed base AABB, skin palettes conservatively
// enclose the weighted joint results, and the instance matrix produces the
// final provider-facing world-space bounds.
DrawWorldBounds resolveDrawWorldBounds(
    const ModelPrimitiveBoundsSource &source, const glm::mat4 &model_matrix,
    std::span<const glm::mat4> skin_palette = {},
    std::span<const float> morph_weights = {});

// Conservative AABB/frustum test for Vulkan's zero-to-one clip volume.
// Intersecting boxes remain visible; only boxes wholly outside one clip plane
// are rejected.
bool intersectsZeroToOneClipFrustum(
    const DrawWorldBounds &bounds,
    const glm::mat4 &view_projection);

DrawViewMask drawViewMask(PrimitiveViewVisibility visibility) noexcept;
PrimitiveViewVisibility primitiveViewVisibility(DrawViewMask mask);
MaterialPhase drawPhaseForMaterialRoute(MaterialRouteClass route) noexcept;

struct DrawQueueBuildRequest {
    std::span<const DrawItemSnapshot> items;
    std::span<const MaterialDrawTagFilter> material_filters;
    std::uint32_t max_draw_indirect_count = 0;
    DrawQueuePhase target_phase = DrawQueuePhase::mixed;
    RenderPolicy::DrawSortLogicalViewV1 logical_view =
        RenderPolicy::DrawSortLogicalViewV1::shared;
    std::array<float, 3> logical_view_origin{};
    std::array<float, 3> logical_view_forward{0.0F, 0.0F, -1.0F};
};

struct MaterialDrawFilterResolution {
    MaterialDrawTagFilterId filter_id{};
    std::size_t resolved_draw_count = 0;
    std::vector<std::string> unmatched_include;
    std::vector<std::string> unmatched_exclude;

    bool operator==(const MaterialDrawFilterResolution &) const = default;
};

class CompiledDrawQueue {
    friend class DrawQueueBuilder;
    friend class CompiledDrawQueueSet;

    struct FilterPublication {
        MaterialDrawTagFilter filter;
        MaterialDrawFilterResolution resolution;
        std::array<std::vector<DrawIndirectInfo>, 2> draw_ranges;
    };

    DrawSortProviderInfo provider_;
    std::vector<DrawItemSnapshot> ordered_items_;
    std::vector<RenderCommand> indirect_records_;
    std::array<std::vector<DrawIndirectInfo>, 2> draw_ranges_;
    std::vector<FilterPublication> material_filters_;

  public:
    const DrawSortProviderInfo &provider() const noexcept { return provider_; }
    bool empty() const noexcept { return indirect_records_.empty(); }
    const std::vector<DrawItemSnapshot> &orderedItems() const noexcept {
        return ordered_items_;
    }
    const std::vector<RenderCommand> &indirectRecords() const noexcept {
        return indirect_records_;
    }
    std::span<const std::byte> indirectBytes() const noexcept;
    const std::vector<DrawIndirectInfo> &
    drawRanges(
        DrawQueueView view,
        std::optional<MaterialDrawTagFilterId> filter_id =
            std::nullopt) const;
    const MaterialDrawFilterResolution *
    materialFilterResolution(
        MaterialDrawTagFilterId filter_id) const noexcept;
};

class DrawQueueBuilder {
  public:
    // Pure CPU-only compilation. It neither mutates request.items nor resolves
    // modules, devices, material containers, or render-graph state.
    static CompiledDrawQueue build(const DrawQueueBuildRequest &request,
                                   const DrawSortProviderLease &provider);
};

struct CompiledDrawQueueVariant {
    DrawQueuePhase phase = DrawQueuePhase::opaque;
    std::uint32_t sort_view_index = 0;
    CompiledDrawQueue queue;
};

// Canonical frame publication: view-major, then opaque/transparent. The GPU
// still sees one indirect buffer; CPU consumers select a phase/view range
// whose offsets have already been rebased into that flattened storage.
class CompiledDrawQueueSet {
    struct Variant {
        struct FilterRanges {
            MaterialDrawTagFilterId filter_id{};
            std::array<std::vector<DrawIndirectInfo>, 2> draw_ranges;
        };

        DrawQueuePhase phase = DrawQueuePhase::opaque;
        std::uint32_t sort_view_index = 0;
        CompiledDrawQueue queue;
        std::array<std::vector<DrawIndirectInfo>, 2> draw_ranges;
        std::vector<FilterRanges> material_filters;
    };

    struct FilterPublication {
        MaterialDrawTagFilter filter;
        MaterialDrawFilterResolution resolution;
        std::vector<std::array<std::vector<DrawIndirectInfo>, 2>>
            all_ranges;
    };

    std::vector<Variant> variants_;
    std::vector<RenderCommand> indirect_records_;
    std::vector<std::array<std::vector<DrawIndirectInfo>, 2>> all_ranges_;
    std::vector<FilterPublication> material_filters_;
    std::vector<SceneDrawSegmentV1> scene_draw_segments_;
    std::uint32_t scene_draw_output_command_capacity_ = 0;

    const Variant &variant(DrawQueuePhase phase,
                           std::uint32_t sort_view_index) const;

  public:
    static CompiledDrawQueueSet
    makeEmpty(
        std::span<const MaterialDrawTagFilter> material_filters,
        std::uint32_t sort_view_count);
    static CompiledDrawQueueSet
    combine(std::vector<CompiledDrawQueueVariant> variants);

    bool empty() const noexcept { return indirect_records_.empty(); }
    std::uint32_t sortViewCount() const noexcept {
        return static_cast<std::uint32_t>(all_ranges_.size());
    }
    std::size_t queueCount() const noexcept { return variants_.size(); }
    const std::vector<RenderCommand> &indirectRecords() const noexcept {
        return indirect_records_;
    }
    const std::vector<SceneDrawSegmentV1> &
    sceneDrawSegments() const noexcept {
        return scene_draw_segments_;
    }
    std::uint32_t
    sceneDrawOutputCommandCapacity() const noexcept {
        return scene_draw_output_command_capacity_;
    }
    const SceneDrawSegmentV1 &
    sceneDrawSegment(std::uint32_t index) const {
        return scene_draw_segments_.at(index);
    }
    std::span<const std::byte> indirectBytes() const noexcept;
    const std::vector<DrawIndirectInfo> &
    drawRanges(DrawQueuePhase phase, std::uint32_t sort_view_index,
               DrawQueueView visibility_view,
               std::optional<MaterialDrawTagFilterId> filter_id =
                   std::nullopt) const;
    const std::vector<DrawIndirectInfo> &
    allDrawRanges(std::uint32_t sort_view_index,
                  DrawQueueView visibility_view,
                  std::optional<MaterialDrawTagFilterId> filter_id =
                      std::nullopt) const;
    const MaterialDrawFilterResolution *
    materialFilterResolution(
        MaterialDrawTagFilterId filter_id) const noexcept;
    const MaterialDrawFilterResolution *
    materialFilterResolution(
        DrawQueuePhase phase,
        std::uint32_t sort_view_index,
        MaterialDrawTagFilterId filter_id) const noexcept;
    const CompiledDrawQueue &
    queue(DrawQueuePhase phase, std::uint32_t sort_view_index) const {
        return variant(phase, sort_view_index).queue;
    }
};

} // namespace Pelican
