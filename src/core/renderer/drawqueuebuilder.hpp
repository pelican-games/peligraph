#pragma once

#include "../material/material.hpp"
#include "../model/modeltemplate.hpp"
#include "modelinstance.hpp"
#include "renderpolicyregistry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

struct DrawIndirectInfo {
    GlobalMaterialId material;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    vk::DeviceSize offset = 0;
    std::uint32_t draw_count = 0;
    std::uint32_t stride = 0;
    bool skinned = false;

    bool operator==(const DrawIndirectInfo &) const = default;
};

enum class DrawQueueView : std::uint8_t {
    third_person,
    first_person,
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

// Immutable input to queue compilation. RPE3 records bounds as optional
// because the current ModelPrimitiveRefInfo does not retain accessor bounds;
// transparent sorting can require them when RPE5 enriches the inventory.
struct DrawItemSnapshot {
    DrawStableIdentity stable_identity{};
    std::uint64_t declaration_ordinal = 0;
    DrawIndexedArguments indexed{};
    DrawPipelineMaterialKey pipeline_material_key{};
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    MaterialPhase phase = MaterialPhase::opaque;
    std::optional<DrawWorldBounds> world_bounds;
    DrawViewMask view_mask = DrawViewMask::both;

    bool operator==(const DrawItemSnapshot &) const = default;
};

DrawViewMask drawViewMask(PrimitiveViewVisibility visibility) noexcept;
PrimitiveViewVisibility primitiveViewVisibility(DrawViewMask mask);
MaterialPhase drawPhaseForMaterialRoute(MaterialRouteClass route) noexcept;

struct DrawQueueBuildRequest {
    std::span<const DrawItemSnapshot> items;
    std::uint32_t max_draw_indirect_count = 0;
};

class CompiledDrawQueue {
    friend class DrawQueueBuilder;

    DrawSortProviderInfo provider_;
    std::vector<DrawItemSnapshot> ordered_items_;
    std::vector<RenderCommand> indirect_records_;
    std::array<std::vector<DrawIndirectInfo>, 2> draw_ranges_;

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
    drawRanges(DrawQueueView view) const noexcept;
};

class DrawQueueBuilder {
  public:
    // Pure CPU-only compilation. It neither mutates request.items nor resolves
    // modules, devices, material containers, or render-graph state.
    static CompiledDrawQueue build(const DrawQueueBuildRequest &request,
                                   const DrawSortProviderLease &provider);
};

} // namespace Pelican
