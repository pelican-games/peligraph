#include "drawqueuebuilder.hpp"

#include "indirectdrawlimits.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace Pelican {

namespace {

constexpr std::size_t viewIndex(DrawQueueView view) noexcept {
    return view == DrawQueueView::first_person ? 1u : 0u;
}

constexpr bool isValidViewMask(DrawViewMask mask) noexcept {
    return mask == DrawViewMask::third_person ||
           mask == DrawViewMask::first_person || mask == DrawViewMask::both;
}

constexpr bool isVisible(DrawViewMask mask, DrawQueueView view) noexcept {
    const auto bits = static_cast<std::uint8_t>(mask);
    const auto required = view == DrawQueueView::first_person
                              ? static_cast<std::uint8_t>(
                                    DrawViewMask::first_person)
                              : static_cast<std::uint8_t>(
                                    DrawViewMask::third_person);
    return (bits & required) != 0;
}

bool isValidRoute(MaterialRouteClass route) noexcept {
    switch (route) {
    case MaterialRouteClass::deferred_geometry:
    case MaterialRouteClass::forward_opaque:
    case MaterialRouteClass::forward_transparent:
        return true;
    }
    return false;
}

void validateSnapshot(const DrawItemSnapshot &item) {
    if (!isValidMaterialId(item.pipeline_material_key.material)) {
        throw std::invalid_argument(
            "DrawItemSnapshot requires a valid material id");
    }
    if (!isValidViewMask(item.view_mask)) {
        throw std::invalid_argument(
            "DrawItemSnapshot view mask must select at least one known view");
    }
    if (!isValidRoute(item.route) ||
        item.phase != drawPhaseForMaterialRoute(item.route)) {
        throw std::invalid_argument(
            "DrawItemSnapshot route and phase are inconsistent");
    }
    if (item.stable_identity.instance.index ==
            std::numeric_limits<std::uint32_t>::max() ||
        item.stable_identity.instance.generation == 0 ||
        item.stable_identity.instance.scene_epoch == 0) {
        throw std::invalid_argument(
            "DrawItemSnapshot requires a valid stable model instance identity");
    }
    if (item.indexed.first_instance != item.stable_identity.instance.index) {
        throw std::invalid_argument(
            "DrawItemSnapshot first instance does not match its stable identity");
    }
    if (item.world_bounds) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(item.world_bounds->minimum[axis]) ||
                !std::isfinite(item.world_bounds->maximum[axis]) ||
                item.world_bounds->minimum[axis] >
                    item.world_bounds->maximum[axis]) {
                throw std::invalid_argument(
                    "DrawItemSnapshot world bounds must be finite and ordered");
            }
        }
    }
}

RenderPolicy::MaterialRouteV1 providerRoute(MaterialRouteClass route) {
    switch (route) {
    case MaterialRouteClass::deferred_geometry:
        return RenderPolicy::MaterialRouteV1::deferred_geometry;
    case MaterialRouteClass::forward_opaque:
        return RenderPolicy::MaterialRouteV1::forward_opaque;
    case MaterialRouteClass::forward_transparent:
        return RenderPolicy::MaterialRouteV1::forward_transparent;
    }
    throw std::invalid_argument("Unknown draw item material route");
}

RenderPolicy::MaterialPhaseV1 providerPhase(MaterialPhase phase) {
    switch (phase) {
    case MaterialPhase::opaque:
        return RenderPolicy::MaterialPhaseV1::opaque;
    case MaterialPhase::transparent:
        return RenderPolicy::MaterialPhaseV1::transparent;
    }
    throw std::invalid_argument("Unknown draw item material phase");
}

std::uint32_t providerViewMask(DrawViewMask mask) {
    switch (mask) {
    case DrawViewMask::third_person:
        return RenderPolicy::view_third_person;
    case DrawViewMask::first_person:
        return RenderPolicy::view_first_person;
    case DrawViewMask::both:
        return RenderPolicy::view_both;
    case DrawViewMask::none:
        break;
    }
    throw std::invalid_argument("Unknown draw item view mask");
}

RenderPolicy::DrawSortItemV1 providerItem(const DrawItemSnapshot &item) {
    RenderPolicy::DrawSortItemV1 result{
        .instance_index = item.stable_identity.instance.index,
        .instance_generation = item.stable_identity.instance.generation,
        .scene_epoch = item.stable_identity.instance.scene_epoch,
        .mesh_index = item.stable_identity.mesh_index,
        .primitive_index = item.stable_identity.primitive_index,
        .node_index = item.stable_identity.node_index,
        .material_id = item.pipeline_material_key.material.value,
        .source_material_index =
            item.pipeline_material_key.source_material_index,
        .declaration_ordinal = item.declaration_ordinal,
        .flags = item.pipeline_material_key.skinned
                     ? RenderPolicy::item_skinned
                     : 0U,
        .route = providerRoute(item.route),
        .phase = providerPhase(item.phase),
        .view_mask = providerViewMask(item.view_mask),
    };
    if (item.world_bounds) {
        result.has_world_bounds = 1;
        result.world_bounds_minimum = {
            item.world_bounds->minimum[0], item.world_bounds->minimum[1],
            item.world_bounds->minimum[2]};
        result.world_bounds_maximum = {
            item.world_bounds->maximum[0], item.world_bounds->maximum[1],
            item.world_bounds->maximum[2]};
    }
    return result;
}

const char *statusName(RenderPolicy::Status status) noexcept {
    switch (status) {
    case RenderPolicy::Status::ok: return "ok";
    case RenderPolicy::Status::buffer_too_small: return "buffer_too_small";
    case RenderPolicy::Status::invalid_argument: return "invalid_argument";
    case RenderPolicy::Status::unsupported_version:
        return "unsupported_version";
    case RenderPolicy::Status::reserved_not_zero: return "reserved_not_zero";
    case RenderPolicy::Status::duplicate_provider: return "duplicate_provider";
    case RenderPolicy::Status::stale_provider: return "stale_provider";
    case RenderPolicy::Status::wrong_owner: return "wrong_owner";
    case RenderPolicy::Status::stale_owner: return "stale_owner";
    case RenderPolicy::Status::provider_error: return "provider_error";
    case RenderPolicy::Status::out_of_memory: return "out_of_memory";
    case RenderPolicy::Status::unavailable: return "unavailable";
    }
    return "unknown_status";
}

auto stableOrderKey(const DrawItemSnapshot &item) noexcept {
    return std::tuple{
        item.stable_identity.instance.scene_epoch,
        item.stable_identity.instance.index,
        item.stable_identity.instance.generation,
        item.stable_identity.mesh_index,
        item.stable_identity.primitive_index,
        item.stable_identity.node_index,
        item.declaration_ordinal,
    };
}

RenderCommand materialize(const DrawItemSnapshot &item) {
    RenderCommand result{};
    result.command = vk::DrawIndexedIndirectCommand{
        item.indexed.index_count,
        item.indexed.instance_count,
        item.indexed.first_index,
        item.indexed.vertex_offset,
        item.indexed.first_instance,
    };
    result.material = item.pipeline_material_key.material;
    result.source_material_index =
        item.pipeline_material_key.source_material_index;
    result.node_index = item.stable_identity.node_index;
    result.skinned = item.pipeline_material_key.skinned;
    result.view_visibility = primitiveViewVisibility(item.view_mask);
    return result;
}

bool sameLegacyBatch(const DrawItemSnapshot &left,
                     const DrawItemSnapshot &right) noexcept {
    return left.pipeline_material_key == right.pipeline_material_key;
}

} // namespace

DrawViewMask drawViewMask(PrimitiveViewVisibility visibility) noexcept {
    switch (visibility) {
    case PrimitiveViewVisibility::third_person_only:
        return DrawViewMask::third_person;
    case PrimitiveViewVisibility::both:
        return DrawViewMask::both;
    case PrimitiveViewVisibility::first_person_only:
        return DrawViewMask::first_person;
    }
    return DrawViewMask::none;
}

PrimitiveViewVisibility primitiveViewVisibility(DrawViewMask mask) {
    switch (mask) {
    case DrawViewMask::third_person:
        return PrimitiveViewVisibility::third_person_only;
    case DrawViewMask::both:
        return PrimitiveViewVisibility::both;
    case DrawViewMask::first_person:
        return PrimitiveViewVisibility::first_person_only;
    case DrawViewMask::none:
        break;
    }
    throw std::invalid_argument("Unknown draw view mask");
}

MaterialPhase drawPhaseForMaterialRoute(MaterialRouteClass route) noexcept {
    return route == MaterialRouteClass::forward_transparent
               ? MaterialPhase::transparent
               : MaterialPhase::opaque;
}

std::span<const std::byte> CompiledDrawQueue::indirectBytes() const noexcept {
    return std::as_bytes(
        std::span<const RenderCommand>{indirect_records_.data(),
                                       indirect_records_.size()});
}

const std::vector<DrawIndirectInfo> &
CompiledDrawQueue::drawRanges(DrawQueueView view) const noexcept {
    return draw_ranges_[viewIndex(view)];
}

CompiledDrawQueue
DrawQueueBuilder::build(const DrawQueueBuildRequest &request,
                        const DrawSortProviderLease &provider) {
    if (!provider) {
        throw std::invalid_argument(
            "DrawQueueBuilder requires a resolved draw sort provider");
    }
    CompiledDrawQueue result;
    result.provider_ = provider.info();

    if (!request.items.empty() && request.max_draw_indirect_count == 0) {
        throw std::invalid_argument(
            "Vulkan maxDrawIndirectCount must be greater than zero");
    }
    if (request.items.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "DrawQueueBuilder item count exceeds the provider ABI limit");
    }

    std::unordered_set<std::uint64_t> declaration_ordinals;
    declaration_ordinals.reserve(request.items.size());
    for (const auto &item : request.items) {
        validateSnapshot(item);
        if (!declaration_ordinals.insert(item.declaration_ordinal).second) {
            throw std::invalid_argument(
                "DrawItemSnapshot declaration ordinals must be unique");
        }
    }

    std::vector<RenderPolicy::DrawSortItemV1> provider_items;
    provider_items.reserve(request.items.size());
    for (const auto &item : request.items) {
        provider_items.push_back(providerItem(item));
    }
    std::vector<RenderPolicy::DrawSortKeyV1> provider_keys(
        request.items.size());
    const auto provider_input =
        RenderPolicy::DrawSortInputV1{
            .target_phase = RenderPolicy::DrawSortPhaseV1::mixed,
            .logical_view = RenderPolicy::DrawSortLogicalViewV1::shared,
            .items = provider_items.data(),
            .item_count = static_cast<std::uint32_t>(provider_items.size()),
        };
    std::uint32_t output_count = 0;
    const auto status = provider.sort(provider_input, provider_keys,
                                      output_count);
    if (status != RenderPolicy::Status::ok) {
        throw std::runtime_error(
            "draw sort provider '" + provider.info().name +
            "' failed with status " + statusName(status));
    }
    if (output_count != provider_keys.size()) {
        throw std::runtime_error(
            "draw sort provider '" + provider.info().name + "' returned " +
            std::to_string(output_count) + " keys for " +
            std::to_string(provider_keys.size()) + " draw items");
    }

    std::vector<std::size_t> order(request.items.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t left,
                                              std::size_t right) {
        const auto &left_key = provider_keys[left];
        const auto &right_key = provider_keys[right];
        if (left_key.primary != right_key.primary) {
            return left_key.primary < right_key.primary;
        }
        if (left_key.secondary != right_key.secondary) {
            return left_key.secondary < right_key.secondary;
        }
        return stableOrderKey(request.items[left]) <
               stableOrderKey(request.items[right]);
    });

    result.ordered_items_.reserve(order.size());
    for (const auto index : order) {
        result.ordered_items_.push_back(request.items[index]);
    }

    result.indirect_records_.reserve(result.ordered_items_.size());
    for (const auto &item : result.ordered_items_) {
        result.indirect_records_.push_back(materialize(item));
    }

    const auto build_ranges = [&](DrawQueueView view) {
        auto &output = result.draw_ranges_[viewIndex(view)];
        std::optional<std::size_t> first;
        const auto flush = [&](std::size_t end) {
            if (!first) return;
            for (const auto &segment : renderer_detail::splitIndirectDrawRange(
                     *first, end, request.max_draw_indirect_count)) {
                const auto &item = result.ordered_items_[segment.first_command];
                output.push_back(DrawIndirectInfo{
                    .material = item.pipeline_material_key.material,
                    .source_material_index =
                        item.pipeline_material_key.source_material_index,
                    .offset = segment.first_command * sizeof(RenderCommand),
                    .draw_count = segment.draw_count,
                    .stride = sizeof(RenderCommand),
                    .skinned = item.pipeline_material_key.skinned,
                });
            }
            first.reset();
        };

        for (std::size_t index = 0; index < result.ordered_items_.size();
             ++index) {
            const auto &item = result.ordered_items_[index];
            if (!isVisible(item.view_mask, view)) {
                flush(index);
                continue;
            }
            if (first &&
                !sameLegacyBatch(result.ordered_items_[*first], item)) {
                flush(index);
            }
            if (!first) first = index;
        }
        flush(result.ordered_items_.size());
    };

    build_ranges(DrawQueueView::third_person);
    build_ranges(DrawQueueView::first_person);
    return result;
}

} // namespace Pelican
