#include "drawqueuebuilder.hpp"

#include "indirectdrawlimits.hpp"

#include <algorithm>
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

constexpr std::uint8_t legacyVisibilityOrder(DrawViewMask mask) noexcept {
    switch (mask) {
    case DrawViewMask::third_person:
        return static_cast<std::uint8_t>(
            PrimitiveViewVisibility::third_person_only);
    case DrawViewMask::both:
        return static_cast<std::uint8_t>(PrimitiveViewVisibility::both);
    case DrawViewMask::first_person:
        return static_cast<std::uint8_t>(
            PrimitiveViewVisibility::first_person_only);
    case DrawViewMask::none:
        break;
    }
    return 0;
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
    if (item.indexed.first_instance != item.stable_identity.instance.index) {
        throw std::invalid_argument(
            "DrawItemSnapshot first instance does not match its stable identity");
    }
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
DrawQueueBuilder::build(const DrawQueueBuildRequest &request) {
    CompiledDrawQueue result;
    result.policy_ = request.policy;

    switch (request.policy) {
    case DrawQueuePolicy::state_batched_v1:
        break;
    default:
        throw std::invalid_argument("Unknown draw queue policy");
    }

    if (request.items.empty()) return result;
    if (request.max_draw_indirect_count == 0) {
        throw std::invalid_argument(
            "Vulkan maxDrawIndirectCount must be greater than zero");
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

    result.ordered_items_.assign(request.items.begin(), request.items.end());
    std::sort(result.ordered_items_.begin(), result.ordered_items_.end(),
              [](const DrawItemSnapshot &left,
                 const DrawItemSnapshot &right) {
                  const auto &left_key = left.pipeline_material_key;
                  const auto &right_key = right.pipeline_material_key;
                  return std::tuple{
                             left_key.material.value,
                             left_key.source_material_index,
                             left_key.skinned,
                             legacyVisibilityOrder(left.view_mask),
                         } <
                         std::tuple{
                             right_key.material.value,
                             right_key.source_material_index,
                             right_key.skinned,
                             legacyVisibilityOrder(right.view_mask),
                         };
              });

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
