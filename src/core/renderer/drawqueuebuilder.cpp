#include "drawqueuebuilder.hpp"

#include "indirectdrawlimits.hpp"

#include <algorithm>
#include <array>
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

bool selectsPhase(DrawQueuePhase target, MaterialPhase item) noexcept {
    switch (target) {
    case DrawQueuePhase::mixed: return true;
    case DrawQueuePhase::opaque: return item == MaterialPhase::opaque;
    case DrawQueuePhase::transparent:
        return item == MaterialPhase::transparent;
    }
    return false;
}

RenderPolicy::DrawSortPhaseV1 providerTargetPhase(DrawQueuePhase phase) {
    switch (phase) {
    case DrawQueuePhase::mixed:
        return RenderPolicy::DrawSortPhaseV1::mixed;
    case DrawQueuePhase::opaque:
        return RenderPolicy::DrawSortPhaseV1::opaque;
    case DrawQueuePhase::transparent:
        return RenderPolicy::DrawSortPhaseV1::transparent;
    }
    throw std::invalid_argument("Unknown draw queue target phase");
}

bool validLogicalView(RenderPolicy::DrawSortLogicalViewV1 view) noexcept {
    switch (view) {
    case RenderPolicy::DrawSortLogicalViewV1::shared:
    case RenderPolicy::DrawSortLogicalViewV1::third_person:
    case RenderPolicy::DrawSortLogicalViewV1::first_person:
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
    for (const auto &tag : item.material_tags) {
        validateMaterialDrawTag(
            tag, "DrawItemSnapshot material");
    }
    if (!std::is_sorted(item.material_tags.begin(),
                        item.material_tags.end()) ||
        std::adjacent_find(item.material_tags.begin(),
                           item.material_tags.end()) !=
            item.material_tags.end()) {
        throw std::invalid_argument(
            "DrawItemSnapshot material tags must be canonical");
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

void validatePrimitiveBounds(const ModelPrimitiveBounds &bounds,
                             const char *description) {
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(bounds.minimum[axis]) ||
            !std::isfinite(bounds.maximum[axis]) ||
            bounds.minimum[axis] > bounds.maximum[axis]) {
            throw std::invalid_argument(description);
        }
    }
}

} // namespace

bool intersectsZeroToOneClipFrustum(
    const DrawWorldBounds &bounds,
    const glm::mat4 &view_projection) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(bounds.minimum[axis]) ||
            !std::isfinite(bounds.maximum[axis]) ||
            bounds.minimum[axis] > bounds.maximum[axis]) {
            throw std::invalid_argument(
                "draw frustum bounds must be finite and ordered");
        }
    }
    for (glm::length_t column = 0; column < 4; ++column) {
        for (glm::length_t row = 0; row < 4; ++row) {
            if (!std::isfinite(view_projection[column][row])) {
                throw std::invalid_argument(
                    "draw frustum matrix must be finite");
            }
        }
    }

    std::array<glm::vec4, 8> clip_corners;
    for (std::uint32_t corner = 0; corner < clip_corners.size(); ++corner) {
        const glm::vec3 world{
            (corner & 1U) != 0 ? bounds.maximum[0] : bounds.minimum[0],
            (corner & 2U) != 0 ? bounds.maximum[1] : bounds.minimum[1],
            (corner & 4U) != 0 ? bounds.maximum[2] : bounds.minimum[2],
        };
        clip_corners[corner] =
            view_projection * glm::vec4{world, 1.0F};
    }

    const auto allOutside = [&](const auto &outside) {
        return std::all_of(
            clip_corners.begin(), clip_corners.end(), outside);
    };
    return !allOutside([](const glm::vec4 &p) { return p.x < -p.w; }) &&
           !allOutside([](const glm::vec4 &p) { return p.x > p.w; }) &&
           !allOutside([](const glm::vec4 &p) { return p.y < -p.w; }) &&
           !allOutside([](const glm::vec4 &p) { return p.y > p.w; }) &&
           !allOutside([](const glm::vec4 &p) { return p.z < 0.0F; }) &&
           !allOutside([](const glm::vec4 &p) { return p.z > p.w; });
}

DrawWorldBounds resolveDrawWorldBounds(
    const ModelPrimitiveBoundsSource &source, const glm::mat4 &model_matrix,
    std::span<const glm::mat4> skin_palette,
    std::span<const float> morph_weights) {
    validatePrimitiveBounds(
        source.base,
        "draw bounds base envelope must be finite and ordered");
    for (const auto &delta : source.morph_position_deltas) {
        validatePrimitiveBounds(
            delta,
            "draw bounds morph envelope must be finite and ordered");
    }

    auto local = source.base;
    if (!source.morph_position_deltas.empty()) {
        if (source.morph_weight_offset > morph_weights.size() ||
            source.morph_position_deltas.size() >
                morph_weights.size() - source.morph_weight_offset) {
            throw std::invalid_argument(
                "draw bounds morph weights do not cover the primitive envelope");
        }
        for (std::size_t target = 0;
             target < source.morph_position_deltas.size(); ++target) {
            const auto weight =
                morph_weights[source.morph_weight_offset + target];
            if (!std::isfinite(weight)) {
                throw std::invalid_argument(
                    "draw bounds morph weight must be finite");
            }
            const auto &delta = source.morph_position_deltas[target];
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                const auto first = weight * delta.minimum[axis];
                const auto second = weight * delta.maximum[axis];
                local.minimum[axis] += std::min(first, second);
                local.maximum[axis] += std::max(first, second);
                if (!std::isfinite(local.minimum[axis]) ||
                    !std::isfinite(local.maximum[axis])) {
                    throw std::invalid_argument(
                        "draw bounds morph envelope is non-finite");
                }
            }
        }
    }

    const auto transformed = [](const ModelPrimitiveBounds &bounds,
                                const glm::mat4 &matrix) {
        glm::vec3 minimum{std::numeric_limits<float>::max()};
        glm::vec3 maximum{std::numeric_limits<float>::lowest()};
        for (std::uint32_t corner = 0; corner < 8; ++corner) {
            const glm::vec3 local_corner{
                (corner & 1U) != 0 ? bounds.maximum.x : bounds.minimum.x,
                (corner & 2U) != 0 ? bounds.maximum.y : bounds.minimum.y,
                (corner & 4U) != 0 ? bounds.maximum.z : bounds.minimum.z,
            };
            const auto world = matrix * glm::vec4{local_corner, 1.0F};
            if (!std::isfinite(world.x) || !std::isfinite(world.y) ||
                !std::isfinite(world.z) || !std::isfinite(world.w)) {
                throw std::invalid_argument(
                    "draw bounds transform produced a non-finite value");
            }
            minimum = glm::min(minimum, glm::vec3{world});
            maximum = glm::max(maximum, glm::vec3{world});
        }
        return ModelPrimitiveBounds{minimum, maximum};
    };

    ModelPrimitiveBounds world;
    if (skin_palette.empty()) {
        world = transformed(local, model_matrix);
    } else {
        world.minimum = glm::vec3{std::numeric_limits<float>::max()};
        world.maximum = glm::vec3{std::numeric_limits<float>::lowest()};
        for (const auto &joint : skin_palette) {
            const auto joint_bounds = transformed(local, model_matrix * joint);
            world.minimum = glm::min(world.minimum, joint_bounds.minimum);
            world.maximum = glm::max(world.maximum, joint_bounds.maximum);
        }
    }
    return DrawWorldBounds{
        .minimum = {world.minimum.x, world.minimum.y, world.minimum.z},
        .maximum = {world.maximum.x, world.maximum.y, world.maximum.z},
    };
}

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
CompiledDrawQueue::drawRanges(
    DrawQueueView view,
    std::optional<MaterialDrawTagFilterId> filter_id) const {
    if (filter_id) {
        const auto found = std::find_if(
            material_filters_.begin(), material_filters_.end(),
            [&](const auto &entry) {
                return entry.filter.id == *filter_id;
            });
        if (found == material_filters_.end()) {
            throw std::out_of_range(
                "compiled draw queue material filter is unavailable");
        }
        return found->draw_ranges[viewIndex(view)];
    }
    return draw_ranges_[viewIndex(view)];
}

const MaterialDrawFilterResolution *
CompiledDrawQueue::materialFilterResolution(
    MaterialDrawTagFilterId filter_id) const noexcept {
    const auto found = std::find_if(
        material_filters_.begin(), material_filters_.end(),
        [&](const auto &entry) {
            return entry.filter.id == filter_id;
        });
    return found == material_filters_.end()
               ? nullptr
               : &found->resolution;
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

    if (!validLogicalView(request.logical_view)) {
        throw std::invalid_argument("DrawQueueBuilder logical view is unknown");
    }
    glm::vec3 view_origin{
        request.logical_view_origin[0], request.logical_view_origin[1],
        request.logical_view_origin[2]};
    glm::vec3 view_forward{
        request.logical_view_forward[0], request.logical_view_forward[1],
        request.logical_view_forward[2]};
    const auto finiteVector = [](const glm::vec3 &value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    const auto forward_length = glm::length(view_forward);
    if (!finiteVector(view_origin) || !finiteVector(view_forward) ||
        !std::isfinite(forward_length) || forward_length <= 0.0F) {
        throw std::invalid_argument(
            "DrawQueueBuilder logical view snapshot must be finite with a non-zero forward vector");
    }
    view_forward /= forward_length;

    std::unordered_set<std::uint64_t> declaration_ordinals;
    declaration_ordinals.reserve(request.items.size());
    for (const auto &item : request.items) {
        validateSnapshot(item);
        if (!declaration_ordinals.insert(item.declaration_ordinal).second) {
            throw std::invalid_argument(
                "DrawItemSnapshot declaration ordinals must be unique");
        }
    }
    std::unordered_set<
        MaterialDrawTagFilterId,
        MaterialDrawTagFilterId::Hash>
        material_filter_ids;
    material_filter_ids.reserve(request.material_filters.size());
    for (const auto &filter : request.material_filters) {
        validateMaterialDrawTagFilter(
            filter, "DrawQueueBuilder material filter");
        if (!material_filter_ids.insert(filter.id).second) {
            throw std::invalid_argument(
                "DrawQueueBuilder material filter ids must be unique");
        }
    }

    std::vector<const DrawItemSnapshot *> selected_items;
    selected_items.reserve(request.items.size());
    for (const auto &item : request.items) {
        if (selectsPhase(request.target_phase, item.phase)) {
            selected_items.push_back(&item);
        }
    }
    std::vector<RenderPolicy::DrawSortItemV1> provider_items;
    provider_items.reserve(selected_items.size());
    for (const auto *item : selected_items)
        provider_items.push_back(providerItem(*item));
    std::vector<RenderPolicy::DrawSortKeyV1> provider_keys(
        selected_items.size());
    const auto provider_input =
        RenderPolicy::DrawSortInputV1{
            .target_phase = providerTargetPhase(request.target_phase),
            .logical_view = request.logical_view,
            .items = provider_items.data(),
            .item_count = static_cast<std::uint32_t>(provider_items.size()),
            .has_logical_view_snapshot = 1,
            .logical_view_origin = {view_origin.x, view_origin.y, view_origin.z},
            .logical_view_forward = {view_forward.x, view_forward.y,
                                     view_forward.z},
        };
    std::uint32_t output_count = 0;
    const auto status = provider_keys.empty()
                            ? RenderPolicy::Status::ok
                            : provider.sort(provider_input, provider_keys,
                                            output_count);
    if (status != RenderPolicy::Status::ok) {
        throw std::runtime_error(
            "draw sort provider '" + provider.info().name +
            "' failed with status " + statusName(status));
    }
    if (!provider_keys.empty() && output_count != provider_keys.size()) {
        throw std::runtime_error(
            "draw sort provider '" + provider.info().name + "' returned " +
            std::to_string(output_count) + " keys for " +
            std::to_string(provider_keys.size()) + " draw items");
    }

    std::vector<std::size_t> order(selected_items.size());
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
        return stableOrderKey(*selected_items[left]) <
               stableOrderKey(*selected_items[right]);
    });

    result.ordered_items_.reserve(order.size());
    for (const auto index : order) {
        result.ordered_items_.push_back(*selected_items[index]);
    }

    result.indirect_records_.reserve(result.ordered_items_.size());
    for (const auto &item : result.ordered_items_) {
        result.indirect_records_.push_back(materialize(item));
    }

    const auto build_ranges =
        [&](DrawQueueView view,
            const MaterialDrawTagFilter *filter,
            std::vector<DrawIndirectInfo> &output) {
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
            if (!isVisible(item.view_mask, view) ||
                (filter != nullptr &&
                 !materialDrawTagFilterMatches(
                     item.material_tags, *filter))) {
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

    build_ranges(
        DrawQueueView::third_person, nullptr,
        result.draw_ranges_[viewIndex(
            DrawQueueView::third_person)]);
    build_ranges(
        DrawQueueView::first_person, nullptr,
        result.draw_ranges_[viewIndex(
            DrawQueueView::first_person)]);

    std::vector<std::string> observed_tags;
    for (const auto &item : result.ordered_items_) {
        observed_tags.insert(
            observed_tags.end(),
            item.material_tags.begin(),
            item.material_tags.end());
    }
    std::sort(observed_tags.begin(), observed_tags.end());
    observed_tags.erase(
        std::unique(observed_tags.begin(), observed_tags.end()),
        observed_tags.end());

    result.material_filters_.reserve(
        request.material_filters.size());
    for (const auto &filter : request.material_filters) {
        CompiledDrawQueue::FilterPublication publication;
        publication.filter = filter;
        publication.resolution.filter_id = filter.id;
        publication.resolution.resolved_draw_count =
            static_cast<std::size_t>(std::count_if(
                result.ordered_items_.begin(),
                result.ordered_items_.end(),
                [&](const auto &item) {
                    return materialDrawTagFilterMatches(
                        item.material_tags, filter);
                }));
        std::set_difference(
            filter.include.begin(), filter.include.end(),
            observed_tags.begin(), observed_tags.end(),
            std::back_inserter(
                publication.resolution.unmatched_include));
        std::set_difference(
            filter.exclude.begin(), filter.exclude.end(),
            observed_tags.begin(), observed_tags.end(),
            std::back_inserter(
                publication.resolution.unmatched_exclude));
        for (const auto view :
             {DrawQueueView::third_person,
              DrawQueueView::first_person}) {
            build_ranges(
                view, &publication.filter,
                publication.draw_ranges[viewIndex(view)]);
        }
        result.material_filters_.push_back(
            std::move(publication));
    }
    return result;
}

const CompiledDrawQueueSet::Variant &CompiledDrawQueueSet::variant(
    DrawQueuePhase phase, std::uint32_t sort_view_index) const {
    const auto found = std::find_if(
        variants_.begin(), variants_.end(), [&](const Variant &candidate) {
            return candidate.phase == phase &&
                   candidate.sort_view_index == sort_view_index;
        });
    if (found == variants_.end()) {
        throw std::out_of_range("compiled draw queue phase/view is unavailable");
    }
    return *found;
}

CompiledDrawQueueSet CompiledDrawQueueSet::makeEmpty(
    std::span<const MaterialDrawTagFilter> material_filters,
    std::uint32_t sort_view_count) {
    if (sort_view_count == 0 ||
        sort_view_count > 2) {
        throw std::invalid_argument(
            "empty compiled draw queue set requires one or two sort views");
    }
    CompiledDrawQueueSet result;
    result.all_ranges_.resize(sort_view_count);
    std::unordered_set<
        MaterialDrawTagFilterId,
        MaterialDrawTagFilterId::Hash>
        ids;
    result.material_filters_.reserve(
        material_filters.size());
    for (const auto &filter : material_filters) {
        validateMaterialDrawTagFilter(
            filter,
            "empty compiled draw queue material filter");
        if (!ids.insert(filter.id).second) {
            throw std::invalid_argument(
                "empty compiled draw queue material filter ids must be unique");
        }
        FilterPublication publication;
        publication.filter = filter;
        publication.resolution.filter_id =
            filter.id;
        publication.resolution.unmatched_include =
            filter.include;
        publication.resolution.unmatched_exclude =
            filter.exclude;
        publication.all_ranges.resize(
            sort_view_count);
        result.material_filters_.push_back(
            std::move(publication));
    }
    return result;
}

CompiledDrawQueueSet CompiledDrawQueueSet::combine(
    std::vector<CompiledDrawQueueVariant> variants) {
    CompiledDrawQueueSet result;
    if (variants.empty()) return result;
    const auto phaseOrder = [](DrawQueuePhase phase) {
        switch (phase) {
        case DrawQueuePhase::opaque: return 0;
        case DrawQueuePhase::transparent: return 1;
        case DrawQueuePhase::mixed: return 2;
        }
        return 3;
    };
    std::sort(variants.begin(), variants.end(), [&](const auto &left,
                                                    const auto &right) {
        return std::tuple{left.sort_view_index, phaseOrder(left.phase)} <
               std::tuple{right.sort_view_index, phaseOrder(right.phase)};
    });

    std::uint32_t view_count = 0;
    for (std::size_t index = 0; index < variants.size(); ++index) {
        const auto &entry = variants[index];
        if (entry.phase == DrawQueuePhase::mixed) {
            throw std::invalid_argument(
                "compiled draw queue set cannot contain a mixed phase");
        }
        if (index != 0 &&
            variants[index - 1].sort_view_index == entry.sort_view_index &&
            variants[index - 1].phase == entry.phase) {
            throw std::invalid_argument(
                "compiled draw queue set phase/view pair is duplicated");
        }
        view_count = std::max(view_count, entry.sort_view_index + 1);
    }
    if (view_count > 2) {
        throw std::invalid_argument(
            "compiled draw queue set supports at most two sort views");
    }
    for (std::uint32_t view = 0; view < view_count; ++view) {
        for (const auto phase : {DrawQueuePhase::opaque,
                                 DrawQueuePhase::transparent}) {
            if (std::none_of(variants.begin(), variants.end(),
                             [&](const auto &entry) {
                                 return entry.sort_view_index == view &&
                                        entry.phase == phase;
                             })) {
                throw std::invalid_argument(
                    "compiled draw queue set requires both phases for every sort view");
            }
        }
    }

    result.all_ranges_.resize(view_count);
    const auto &reference_filters =
        variants.front().queue.material_filters_;
    result.material_filters_.reserve(
        reference_filters.size());
    for (const auto &filter : reference_filters) {
        FilterPublication publication;
        publication.filter = filter.filter;
        publication.resolution.filter_id =
            filter.filter.id;
        publication.resolution.unmatched_include =
            filter.filter.include;
        publication.resolution.unmatched_exclude =
            filter.filter.exclude;
        publication.all_ranges.resize(view_count);
        result.material_filters_.push_back(
            std::move(publication));
    }
    for (const auto &entry : variants) {
        if (entry.queue.material_filters_.size() !=
            reference_filters.size()) {
            throw std::invalid_argument(
                "compiled draw queue set material filters are inconsistent");
        }
        for (std::size_t filter_index = 0;
             filter_index < reference_filters.size();
             ++filter_index) {
            if (entry.queue.material_filters_[filter_index].filter !=
                reference_filters[filter_index].filter) {
                throw std::invalid_argument(
                    "compiled draw queue set material filters are inconsistent");
            }
        }
    }
    std::size_t total_records = 0;
    for (const auto &entry : variants) {
        if (entry.queue.indirectRecords().size() >
            std::numeric_limits<std::size_t>::max() - total_records) {
            throw std::overflow_error(
                "compiled draw queue set record count overflow");
        }
        total_records += entry.queue.indirectRecords().size();
    }
    result.indirect_records_.reserve(total_records);
    result.variants_.reserve(variants.size());
    for (auto &entry : variants) {
        const auto base = result.indirect_records_.size();
        result.indirect_records_.insert(
            result.indirect_records_.end(), entry.queue.indirectRecords().begin(),
            entry.queue.indirectRecords().end());
        Variant compiled{
            .phase = entry.phase,
            .sort_view_index = entry.sort_view_index,
            .queue = std::move(entry.queue),
        };
        for (const auto visibility : {DrawQueueView::third_person,
                                      DrawQueueView::first_person}) {
            const auto index = viewIndex(visibility);
            compiled.draw_ranges[index] =
                compiled.queue.drawRanges(visibility);
            for (auto &range : compiled.draw_ranges[index]) {
                range.offset += base * sizeof(RenderCommand);
            }
        }
        compiled.material_filters.reserve(
            compiled.queue.material_filters_.size());
        for (std::size_t filter_index = 0;
             filter_index <
             compiled.queue.material_filters_.size();
             ++filter_index) {
            const auto &source =
                compiled.queue.material_filters_[filter_index];
            Variant::FilterRanges filtered{
                .filter_id = source.filter.id,
                .draw_ranges = source.draw_ranges,
            };
            for (auto &view_ranges :
                 filtered.draw_ranges) {
                for (auto &range : view_ranges) {
                    range.offset +=
                        base * sizeof(RenderCommand);
                }
            }
            auto &published =
                result.material_filters_[filter_index];
            if (compiled.sort_view_index == 0) {
                published.resolution.resolved_draw_count +=
                    source.resolution.resolved_draw_count;
                std::vector<std::string> unmatched_include;
                std::set_intersection(
                    published.resolution.unmatched_include.begin(),
                    published.resolution.unmatched_include.end(),
                    source.resolution.unmatched_include.begin(),
                    source.resolution.unmatched_include.end(),
                    std::back_inserter(unmatched_include));
                published.resolution.unmatched_include =
                    std::move(unmatched_include);
                std::vector<std::string> unmatched_exclude;
                std::set_intersection(
                    published.resolution.unmatched_exclude.begin(),
                    published.resolution.unmatched_exclude.end(),
                    source.resolution.unmatched_exclude.begin(),
                    source.resolution.unmatched_exclude.end(),
                    std::back_inserter(unmatched_exclude));
                published.resolution.unmatched_exclude =
                    std::move(unmatched_exclude);
            }
            compiled.material_filters.push_back(
                std::move(filtered));
        }

        const auto publish_ranges =
            [&](std::vector<DrawIndirectInfo> &ranges,
                DrawQueueView visibility,
                std::uint32_t filter_index) {
                for (auto &range : ranges) {
                    if (range.offset %
                            sizeof(RenderCommand) !=
                        0) {
                        throw std::runtime_error(
                            "compiled draw queue range is not command aligned");
                    }
                    if (result.scene_draw_segments_.size() >=
                        noSceneDrawSegmentIndex) {
                        throw std::overflow_error(
                            "scene draw segment count exceeds uint32");
                    }
                    if (range.draw_count >
                        std::numeric_limits<std::uint32_t>::max() -
                            result.scene_draw_output_command_capacity_) {
                        throw std::overflow_error(
                            "scene draw segment output capacity exceeds uint32");
                    }
                    const auto segment_index =
                        static_cast<std::uint32_t>(
                            result.scene_draw_segments_.size());
                    const auto first_command =
                        range.offset /
                        sizeof(RenderCommand);
                    if (first_command >
                        std::numeric_limits<std::uint32_t>::max()) {
                        throw std::overflow_error(
                            "scene draw segment source offset exceeds uint32");
                    }
                    range.scene_segment_index =
                        segment_index;
                    result.scene_draw_segments_.push_back(
                        SceneDrawSegmentV1{
                            .source_first_command =
                                static_cast<std::uint32_t>(
                                    first_command),
                            .command_capacity =
                                range.draw_count,
                            .output_first_command =
                                result.scene_draw_output_command_capacity_,
                            .output_count_index =
                                segment_index,
                            .sort_view_index =
                                compiled.sort_view_index,
                            .phase =
                                compiled.phase ==
                                        DrawQueuePhase::opaque
                                    ? 0U
                                    : 1U,
                            .visibility_view =
                                visibility ==
                                        DrawQueueView::third_person
                                    ? 0U
                                    : 1U,
                            .material_filter_index =
                                filter_index,
                        });
                    result.scene_draw_output_command_capacity_ +=
                        range.draw_count;
                }
            };
        for (const auto visibility :
             {DrawQueueView::third_person,
              DrawQueueView::first_person}) {
            publish_ranges(
                compiled.draw_ranges[
                    viewIndex(visibility)],
                visibility,
                noSceneDrawSegmentIndex);
        }
        for (std::size_t filter_index = 0;
             filter_index <
             compiled.material_filters.size();
             ++filter_index) {
            for (const auto visibility :
                 {DrawQueueView::third_person,
                  DrawQueueView::first_person}) {
                publish_ranges(
                    compiled.material_filters[
                        filter_index]
                        .draw_ranges[
                            viewIndex(visibility)],
                    visibility,
                    static_cast<std::uint32_t>(
                        filter_index));
            }
        }

        for (const auto visibility :
             {DrawQueueView::third_person,
              DrawQueueView::first_person}) {
            const auto index =
                viewIndex(visibility);
            auto &all =
                result.all_ranges_[
                    compiled.sort_view_index][index];
            all.insert(
                all.end(),
                compiled.draw_ranges[index].begin(),
                compiled.draw_ranges[index].end());
        }
        for (std::size_t filter_index = 0;
             filter_index <
             compiled.material_filters.size();
             ++filter_index) {
            auto &published =
                result.material_filters_[filter_index];
            for (const auto visibility :
                 {DrawQueueView::third_person,
                  DrawQueueView::first_person}) {
                const auto index =
                    viewIndex(visibility);
                auto &all =
                    published.all_ranges
                        [compiled.sort_view_index][index];
                const auto &ranges =
                    compiled.material_filters[
                        filter_index]
                        .draw_ranges[index];
                all.insert(
                    all.end(),
                    ranges.begin(), ranges.end());
            }
        }
        result.variants_.push_back(std::move(compiled));
    }
    return result;
}

std::span<const std::byte>
CompiledDrawQueueSet::indirectBytes() const noexcept {
    return std::as_bytes(std::span<const RenderCommand>{
        indirect_records_.data(), indirect_records_.size()});
}

const std::vector<DrawIndirectInfo> &CompiledDrawQueueSet::drawRanges(
    DrawQueuePhase phase, std::uint32_t sort_view_index,
    DrawQueueView visibility_view,
    std::optional<MaterialDrawTagFilterId> filter_id) const {
    static const std::vector<DrawIndirectInfo> empty;
    if (variants_.empty()) {
        if (sort_view_index >=
            all_ranges_.size()) {
            throw std::out_of_range(
                "compiled draw queue sort view is unavailable");
        }
        if (filter_id &&
            materialFilterResolution(
                *filter_id) == nullptr) {
            throw std::out_of_range(
                "compiled draw queue set material filter is unavailable");
        }
        return empty;
    }
    const auto &selected =
        variant(phase, sort_view_index);
    if (!filter_id) {
        return selected
            .draw_ranges[viewIndex(visibility_view)];
    }
    const auto found = std::find_if(
        selected.material_filters.begin(),
        selected.material_filters.end(),
        [&](const auto &entry) {
            return entry.filter_id == *filter_id;
        });
    if (found == selected.material_filters.end()) {
        throw std::out_of_range(
            "compiled draw queue set material filter is unavailable");
    }
    return found->draw_ranges[
        viewIndex(visibility_view)];
}

const std::vector<DrawIndirectInfo> &CompiledDrawQueueSet::allDrawRanges(
    std::uint32_t sort_view_index,
    DrawQueueView visibility_view,
    std::optional<MaterialDrawTagFilterId> filter_id) const {
    static const std::vector<DrawIndirectInfo> empty;
    if (sort_view_index >= all_ranges_.size()) {
        throw std::out_of_range("compiled draw queue sort view is unavailable");
    }
    if (filter_id) {
        const auto found = std::find_if(
            material_filters_.begin(),
            material_filters_.end(),
            [&](const auto &entry) {
                return entry.filter.id == *filter_id;
            });
        if (found == material_filters_.end()) {
            throw std::out_of_range(
                "compiled draw queue set material filter is unavailable");
        }
        return found->all_ranges[sort_view_index]
                                [viewIndex(visibility_view)];
    }
    return all_ranges_[sort_view_index][viewIndex(visibility_view)];
}

const MaterialDrawFilterResolution *
CompiledDrawQueueSet::materialFilterResolution(
    MaterialDrawTagFilterId filter_id) const noexcept {
    const auto found = std::find_if(
        material_filters_.begin(), material_filters_.end(),
        [&](const auto &entry) {
            return entry.filter.id == filter_id;
        });
    return found == material_filters_.end()
               ? nullptr
               : &found->resolution;
}

const MaterialDrawFilterResolution *
CompiledDrawQueueSet::materialFilterResolution(
    DrawQueuePhase phase,
    std::uint32_t sort_view_index,
    MaterialDrawTagFilterId filter_id) const noexcept {
    const auto found = std::find_if(
        variants_.begin(), variants_.end(),
        [&](const Variant &candidate) {
            return candidate.phase == phase &&
                   candidate.sort_view_index ==
                       sort_view_index;
        });
    if (found == variants_.end()) {
        return nullptr;
    }
    return found->queue
        .materialFilterResolution(filter_id);
}

} // namespace Pelican
