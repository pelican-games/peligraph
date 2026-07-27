#include "../src/core/renderer/drawqueuebuilder.hpp"
#include "../src/core/renderer/indirectdrawlimits.hpp"
#include "../src/core/model/polygonvertdata.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {
namespace {

DrawSortProviderLease builtinProvider() {
    return renderPolicyRegistry().resolveDrawSortProvider(
        builtinStateBatchedDrawSortProvider);
}

DrawSortProviderLease backToFrontProvider() {
    return renderPolicyRegistry().resolveDrawSortProvider(
        builtinBackToFrontDrawSortProvider);
}

DrawItemSnapshot item(std::uint64_t ordinal, int material,
                      std::uint32_t source_material, bool skinned,
                      PrimitiveViewVisibility visibility,
                      std::uint32_t instance, std::uint32_t first_index,
                      MaterialRouteClass route =
                          MaterialRouteClass::deferred_geometry) {
    return DrawItemSnapshot{
        .stable_identity =
            DrawStableIdentity{
                .instance = ModelInstanceId{instance, 1, 7},
                .mesh_index = instance % 2,
                .primitive_index = static_cast<std::uint32_t>(ordinal),
                .node_index = static_cast<std::uint32_t>(ordinal + 10),
            },
        .declaration_ordinal = ordinal,
        .indexed =
            DrawIndexedArguments{
                .index_count = static_cast<std::uint32_t>(ordinal + 3),
                .instance_count = 1,
                .first_index = first_index,
                .vertex_offset = static_cast<std::int32_t>(ordinal) - 2,
                .first_instance = instance,
            },
        .pipeline_material_key =
            DrawPipelineMaterialKey{
                .material = GlobalMaterialId{material},
                .source_material_index = source_material,
                .skinned = skinned,
            },
        .route = route,
        .phase = drawPhaseForMaterialRoute(route),
        .world_bounds = ordinal == 2
                            ? std::optional{DrawWorldBounds{
                                  .minimum = {-1.0f, -2.0f, -3.0f},
                                  .maximum = {1.0f, 2.0f, 3.0f},
                              }}
                            : std::nullopt,
        .view_mask = drawViewMask(visibility),
    };
}

DrawItemSnapshot transparentItem(std::uint64_t ordinal,
                                 std::uint32_t instance, float x, float z,
                                 int material = 5) {
    auto result = item(ordinal, material, 0, false,
                       PrimitiveViewVisibility::both, instance,
                       static_cast<std::uint32_t>(ordinal * 3),
                       MaterialRouteClass::forward_transparent);
    result.world_bounds = DrawWorldBounds{
        .minimum = {x - 0.25F, -0.25F, z - 0.25F},
        .maximum = {x + 0.25F, 0.25F, z + 0.25F},
    };
    return result;
}

void requireBounds(const DrawWorldBounds &actual,
                   const std::array<float, 3> &minimum,
                   const std::array<float, 3> &maximum) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        REQUIRE(actual.minimum[axis] == Catch::Approx(minimum[axis]));
        REQUIRE(actual.maximum[axis] == Catch::Approx(maximum[axis]));
    }
}

RenderCommand legacyRecord(const DrawItemSnapshot &snapshot) {
    RenderCommand result{};
    result.command = vk::DrawIndexedIndirectCommand{
        snapshot.indexed.index_count,
        snapshot.indexed.instance_count,
        snapshot.indexed.first_index,
        snapshot.indexed.vertex_offset,
        snapshot.indexed.first_instance,
    };
    result.material = snapshot.pipeline_material_key.material;
    result.source_material_index =
        snapshot.pipeline_material_key.source_material_index;
    result.node_index = snapshot.stable_identity.node_index;
    result.skinned = snapshot.pipeline_material_key.skinned;
    result.view_visibility = primitiveViewVisibility(snapshot.view_mask);
    return result;
}

struct LegacyQueue {
    std::vector<RenderCommand> commands;
    std::array<std::vector<DrawIndirectInfo>, 2> ranges;
};

LegacyQueue legacyBuild(std::span<const DrawItemSnapshot> items,
                        std::uint32_t max_draw_count) {
    LegacyQueue result;
    result.commands.reserve(items.size());
    for (const auto &snapshot : items)
        result.commands.push_back(legacyRecord(snapshot));

    std::sort(result.commands.begin(), result.commands.end(),
              [](const RenderCommand &left, const RenderCommand &right) {
                  return std::tie(left.material.value,
                                  left.source_material_index, left.skinned,
                                  left.view_visibility) <
                         std::tie(right.material.value,
                                  right.source_material_index, right.skinned,
                                  right.view_visibility);
              });

    const auto build_ranges = [&](bool first_person) {
        auto &output = result.ranges[first_person ? 1u : 0u];
        std::optional<std::size_t> first;
        const auto visible = [&](const RenderCommand &command) {
            return first_person
                       ? command.view_visibility !=
                             PrimitiveViewVisibility::third_person_only
                       : command.view_visibility !=
                             PrimitiveViewVisibility::first_person_only;
        };
        const auto flush = [&](std::size_t end) {
            if (!first) return;
            for (const auto &segment : renderer_detail::splitIndirectDrawRange(
                     *first, end, max_draw_count)) {
                const auto &command = result.commands[segment.first_command];
                output.push_back(DrawIndirectInfo{
                    .material = command.material,
                    .source_material_index = command.source_material_index,
                    .offset = segment.first_command * sizeof(RenderCommand),
                    .draw_count = segment.draw_count,
                    .stride = sizeof(RenderCommand),
                    .skinned = command.skinned,
                });
            }
            first.reset();
        };
        for (std::size_t index = 0; index < result.commands.size(); ++index) {
            const auto &command = result.commands[index];
            if (!visible(command)) {
                flush(index);
                continue;
            }
            if (first) {
                const auto &begin = result.commands[*first];
                if (begin.material.value != command.material.value ||
                    begin.source_material_index !=
                        command.source_material_index ||
                    begin.skinned != command.skinned) {
                    flush(index);
                }
            }
            if (!first) first = index;
        }
        flush(result.commands.size());
    };
    build_ranges(false);
    build_ranges(true);
    return result;
}

void requireSameBytes(std::span<const std::byte> actual,
                      std::span<const RenderCommand> expected) {
    const auto expected_bytes = std::as_bytes(expected);
    REQUIRE(actual.size() == expected_bytes.size());
    REQUIRE(std::equal(actual.begin(), actual.end(), expected_bytes.begin()));
}

DrawItemSnapshot withTags(
    DrawItemSnapshot snapshot,
    std::vector<std::string> tags) {
    snapshot.material_tags =
        canonicalizeMaterialDrawTags(
            std::move(tags), "test material");
    return snapshot;
}

std::vector<std::uint32_t> selectedPrimitiveIds(
    const CompiledDrawQueue &queue,
    MaterialDrawTagFilterId filter_id,
    DrawQueueView view =
        DrawQueueView::third_person) {
    std::vector<std::uint32_t> result;
    for (const auto &range :
         queue.drawRanges(view, filter_id)) {
        REQUIRE(
            range.stride == sizeof(RenderCommand));
        const auto first =
            static_cast<std::size_t>(
                range.offset /
                sizeof(RenderCommand));
        for (std::uint32_t draw = 0;
             draw < range.draw_count; ++draw) {
            result.push_back(
                queue.orderedItems()
                    .at(first + draw)
                    .stable_identity
                    .primitive_index);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::uint32_t totalDrawCount(
    const std::vector<DrawIndirectInfo> &ranges) {
    return std::accumulate(
        ranges.begin(), ranges.end(),
        std::uint32_t{0},
        [](std::uint32_t total,
           const DrawIndirectInfo &range) {
            return total + range.draw_count;
        });
}

} // namespace

TEST_CASE("state_batched_v1 reproduces the legacy command bytes and view ranges",
          "[renderer][draw-queue][wp182]") {
    const std::vector input{
        item(0, 2, 0, false, PrimitiveViewVisibility::both, 0, 60),
        item(1, 1, 3, true, PrimitiveViewVisibility::first_person_only, 1,
             50),
        item(2, 1, 2, false, PrimitiveViewVisibility::both, 2, 40,
             MaterialRouteClass::forward_transparent),
        item(3, 1, 2, false, PrimitiveViewVisibility::third_person_only, 3,
             30),
        item(4, 1, 2, false, PrimitiveViewVisibility::first_person_only, 4,
             20),
        item(5, 1, 2, true, PrimitiveViewVisibility::both, 5, 10,
             MaterialRouteClass::forward_opaque),
    };
    const auto original = input;
    const auto legacy = legacyBuild(input, 2);
    const auto provider = builtinProvider();
    const auto compiled = DrawQueueBuilder::build(
        DrawQueueBuildRequest{.items = input,
                              .max_draw_indirect_count = 2},
        provider);

    REQUIRE(input == original);
    REQUIRE(compiled.provider().name ==
            std::string{builtinStateBatchedDrawSortProvider});
    REQUIRE(compiled.provider().provider_version ==
            RenderPolicy::providerVersionV1);
    REQUIRE(compiled.orderedItems().size() == input.size());
    requireSameBytes(compiled.indirectBytes(), legacy.commands);
    REQUIRE(compiled.drawRanges(DrawQueueView::third_person) ==
            legacy.ranges[0]);
    REQUIRE(compiled.drawRanges(DrawQueueView::first_person) ==
            legacy.ranges[1]);

    const auto &ordered = compiled.indirectRecords();
    REQUIRE(ordered[0].view_visibility ==
            PrimitiveViewVisibility::third_person_only);
    REQUIRE(ordered[1].view_visibility == PrimitiveViewVisibility::both);
    REQUIRE(ordered[2].view_visibility ==
            PrimitiveViewVisibility::first_person_only);
    REQUIRE(compiled.orderedItems()[1].world_bounds.has_value());
    REQUIRE(compiled.orderedItems()[1].route ==
            MaterialRouteClass::forward_transparent);
}

TEST_CASE("state_batched_v1 splits large batches without changing stride or offsets",
          "[renderer][draw-queue][limits][wp182]") {
    std::vector<DrawItemSnapshot> input;
    for (std::uint64_t ordinal = 0; ordinal < 5; ++ordinal) {
        input.push_back(item(ordinal, 4, 9, false,
                             PrimitiveViewVisibility::both,
                             static_cast<std::uint32_t>(ordinal),
                             static_cast<std::uint32_t>(ordinal * 3)));
    }

    const auto provider = builtinProvider();
    const auto compiled = DrawQueueBuilder::build(
        DrawQueueBuildRequest{.items = input,
                              .max_draw_indirect_count = 2},
        provider);
    for (const auto view : {DrawQueueView::third_person,
                            DrawQueueView::first_person}) {
        const auto &ranges = compiled.drawRanges(view);
        REQUIRE(ranges.size() == 3);
        CHECK(ranges[0].offset == 0);
        CHECK(ranges[0].draw_count == 2);
        CHECK(ranges[1].offset == 2 * sizeof(RenderCommand));
        CHECK(ranges[1].draw_count == 2);
        CHECK(ranges[2].offset == 4 * sizeof(RenderCommand));
        CHECK(ranges[2].draw_count == 1);
        CHECK(ranges[0].stride == sizeof(RenderCommand));
        CHECK(ranges[1].stride == sizeof(RenderCommand));
        CHECK(ranges[2].stride == sizeof(RenderCommand));
    }
}

TEST_CASE("draw queue compilation is repeatable, input preserving, and fail fast",
          "[renderer][draw-queue][determinism][wp182]") {
    const std::vector input{
        item(0, 3, 1, false, PrimitiveViewVisibility::both, 0, 6),
        item(1, 2, 1, true, PrimitiveViewVisibility::both, 1, 3),
    };
    const auto original = input;
    const DrawQueueBuildRequest request{.items = input,
                                        .max_draw_indirect_count = 64};
    const auto provider = builtinProvider();
    const auto first = DrawQueueBuilder::build(request, provider);
    const auto second = DrawQueueBuilder::build(request, provider);

    REQUIRE(input == original);
    REQUIRE(first.orderedItems() == second.orderedItems());
    REQUIRE(first.drawRanges(DrawQueueView::third_person) ==
            second.drawRanges(DrawQueueView::third_person));
    REQUIRE(first.drawRanges(DrawQueueView::first_person) ==
            second.drawRanges(DrawQueueView::first_person));
    REQUIRE(first.indirectBytes().size() == second.indirectBytes().size());
    REQUIRE(std::equal(first.indirectBytes().begin(), first.indirectBytes().end(),
                       second.indirectBytes().begin()));

    const auto empty = DrawQueueBuilder::build(
        DrawQueueBuildRequest{.items = {}, .max_draw_indirect_count = 0},
        provider);
    REQUIRE(empty.empty());

    CHECK_THROWS_AS(DrawQueueBuilder::build(DrawQueueBuildRequest{
                        .items = input, .max_draw_indirect_count = 0},
                        provider),
                    std::invalid_argument);

    auto invalid_phase = input;
    invalid_phase.front().phase = MaterialPhase::transparent;
    CHECK_THROWS_AS(DrawQueueBuilder::build(DrawQueueBuildRequest{
                        .items = invalid_phase,
                        .max_draw_indirect_count = 64},
                        provider),
                    std::invalid_argument);

    auto duplicate_ordinal = input;
    duplicate_ordinal.back().declaration_ordinal =
        duplicate_ordinal.front().declaration_ordinal;
    CHECK_THROWS_AS(DrawQueueBuilder::build(DrawQueueBuildRequest{
                        .items = duplicate_ordinal,
                        .max_draw_indirect_count = 64},
                        provider),
                    std::invalid_argument);
}

TEST_CASE(
    "WP206a material tag selection is invariant under registration and sort order",
    "[renderer][draw-queue][draw-tag][wp206a]") {
    const auto selected_filter =
        makeMaterialDrawTagFilter(
            {"ghost", "outline"}, {"hidden"},
            "selected filter");
    const auto unknown_include =
        makeMaterialDrawTagFilter(
            {"not_registered"}, {},
            "unknown include filter");
    const auto unknown_exclude =
        makeMaterialDrawTagFilter(
            {}, {"not_registered"},
            "unknown exclude filter");
    const std::array filters{
        selected_filter,
        unknown_include,
        unknown_exclude,
    };

    std::vector first_input{
        withTags(
            item(0, 0, 0, false,
                 PrimitiveViewVisibility::both,
                 0, 0),
            {"outline"}),
        withTags(
            item(1, 1, 0, false,
                 PrimitiveViewVisibility::both,
                 1, 3),
            {"hidden", "outline"}),
        withTags(
            item(2, 2, 0, false,
                 PrimitiveViewVisibility::both,
                 2, 6),
            {"environment"}),
        withTags(
            item(3, 3, 0, false,
                 PrimitiveViewVisibility::both,
                 3, 9),
            {"character", "outline"}),
    };
    auto second_input = first_input;
    std::reverse(
        second_input.begin(),
        second_input.end());
    for (auto &snapshot : second_input) {
        snapshot.pipeline_material_key.material =
            GlobalMaterialId{
                20 -
                snapshot.pipeline_material_key
                    .material.value};
    }

    const auto provider = builtinProvider();
    const auto first = DrawQueueBuilder::build(
        DrawQueueBuildRequest{
            .items = first_input,
            .material_filters = filters,
            .max_draw_indirect_count = 64,
        },
        provider);
    const auto second = DrawQueueBuilder::build(
        DrawQueueBuildRequest{
            .items = second_input,
            .material_filters = filters,
            .max_draw_indirect_count = 64,
        },
        provider);

    const std::vector<std::uint32_t>
        expected{0, 3};
    REQUIRE(
        selectedPrimitiveIds(
            first, selected_filter.id) ==
        expected);
    REQUIRE(
        selectedPrimitiveIds(
            second, selected_filter.id) ==
        expected);
    REQUIRE(
        first.materialFilterResolution(
                 selected_filter.id)
            ->resolved_draw_count == 2);
    REQUIRE(
        first.materialFilterResolution(
                 selected_filter.id)
            ->unmatched_include ==
        std::vector<std::string>{"ghost"});
    REQUIRE(
        first.drawRanges(
                 DrawQueueView::third_person,
                 unknown_include.id)
            .empty());
    REQUIRE(
        first.materialFilterResolution(
                 unknown_include.id)
            ->unmatched_include ==
        std::vector<std::string>{
            "not_registered"});
    REQUIRE(
        totalDrawCount(first.drawRanges(
            DrawQueueView::third_person,
            unknown_exclude.id)) ==
        first_input.size());
    REQUIRE(
        first.materialFilterResolution(
                 unknown_exclude.id)
            ->unmatched_exclude ==
        std::vector<std::string>{
            "not_registered"});
}

TEST_CASE(
    "WP206a resolved filters survive flat and two-view queue publication",
    "[renderer][draw-queue][draw-tag][xr][wp206a]") {
    const auto filter =
        makeMaterialDrawTagFilter(
            {"fx"}, {}, "fx filter");
    const std::array filters{filter};
    const std::vector input{
        withTags(
            item(0, 1, 0, false,
                 PrimitiveViewVisibility::both,
                 0, 0),
            {"fx"}),
        withTags(
            transparentItem(
                1, 1, 0.0F, -2.0F, 2),
            {"fx", "transparent"}),
        withTags(
            item(2, 3, 0, false,
                 PrimitiveViewVisibility::both,
                 2, 6),
            {"base"}),
    };

    const auto compile = [&](std::uint32_t view_count) {
        std::vector<CompiledDrawQueueVariant>
            variants;
        for (std::uint32_t view = 0;
             view < view_count; ++view) {
            const auto request =
                [&](DrawQueuePhase phase) {
                    return DrawQueueBuildRequest{
                        .items = input,
                        .material_filters =
                            filters,
                        .max_draw_indirect_count =
                            64,
                        .target_phase = phase,
                        .logical_view_origin =
                            {static_cast<float>(
                                 view),
                             0.0F, 0.0F},
                    };
                };
            variants.push_back({
                .phase =
                    DrawQueuePhase::opaque,
                .sort_view_index = view,
                .queue =
                    DrawQueueBuilder::build(
                        request(
                            DrawQueuePhase::
                                opaque),
                        builtinProvider()),
            });
            variants.push_back({
                .phase =
                    DrawQueuePhase::transparent,
                .sort_view_index = view,
                .queue =
                    DrawQueueBuilder::build(
                        request(
                            DrawQueuePhase::
                                transparent),
                        backToFrontProvider()),
            });
        }
        return CompiledDrawQueueSet::combine(
            std::move(variants));
    };

    const auto flat = compile(1);
    const auto xr = compile(2);
    const auto empty_xr =
        CompiledDrawQueueSet::makeEmpty(
            filters, 2);
    REQUIRE(
        flat.materialFilterResolution(filter.id)
            ->resolved_draw_count == 2);
    REQUIRE(
        xr.materialFilterResolution(filter.id)
            ->resolved_draw_count == 2);
    REQUIRE(
        xr.materialFilterResolution(
              DrawQueuePhase::opaque, 0,
              filter.id)
            ->resolved_draw_count == 1);
    REQUIRE(
        xr.materialFilterResolution(
              DrawQueuePhase::transparent, 0,
              filter.id)
            ->resolved_draw_count == 1);
    REQUIRE(
        empty_xr.materialFilterResolution(
                    filter.id)
            ->resolved_draw_count == 0);
    REQUIRE(
        empty_xr.materialFilterResolution(
                    filter.id)
            ->unmatched_include ==
        std::vector<std::string>{"fx"});
    REQUIRE(
        empty_xr.drawRanges(
                    DrawQueuePhase::opaque,
                    1,
                    DrawQueueView::
                        third_person,
                    filter.id)
            .empty());
    for (std::uint32_t view = 0;
         view < 2; ++view) {
        const auto &ranges =
            xr.allDrawRanges(
                view,
                DrawQueueView::
                    third_person,
                filter.id);
        REQUIRE(
            totalDrawCount(
                ranges) == 2);
        for (const auto &range : ranges) {
            REQUIRE(
                range.scene_segment_index !=
                noSceneDrawSegmentIndex);
            const auto &segment =
                xr.sceneDrawSegment(
                    range
                        .scene_segment_index);
            CHECK(
                segment.sort_view_index ==
                view);
            CHECK(
                segment.visibility_view ==
                0);
            CHECK(
                segment
                    .material_filter_index ==
                0);
        }
    }
}

TEST_CASE("RPE5 bounds use indexed vertices and current deformation envelopes",
          "[renderer][draw-queue][bounds][wp184]") {
    CommonPolygonVertData geometry;
    geometry.pos = {
        {100.0F, 100.0F, 100.0F}, // deliberately unreferenced
        {-1.0F, 2.0F, 3.0F},
        {4.0F, -2.0F, 1.0F},
    };
    geometry.indices = {1, 2, 1};
    geometry.morph_weight_offset = 1;
    geometry.morph_targets.push_back(MorphTargetVertexData{
        .position = {
            {1000.0F, 1000.0F, 1000.0F}, // deliberately unreferenced
            {-2.0F, 1.0F, 0.0F},
            {3.0F, -4.0F, 2.0F},
        },
        .presence_mask = morphPositionPresent,
    });

    const auto source = makePrimitiveBoundsSource(geometry);
    REQUIRE(source != nullptr);
    REQUIRE(source->base.minimum == glm::vec3{-1.0F, -2.0F, 1.0F});
    REQUIRE(source->base.maximum == glm::vec3{4.0F, 2.0F, 3.0F});
    REQUIRE(source->morph_position_deltas.size() == 1);
    REQUIRE(source->morph_position_deltas[0].minimum ==
            glm::vec3{-2.0F, -4.0F, 0.0F});
    REQUIRE(source->morph_position_deltas[0].maximum ==
            glm::vec3{3.0F, 1.0F, 2.0F});

    const auto model =
        glm::translate(glm::mat4{1.0F}, {10.0F, -1.0F, 7.0F}) *
        glm::scale(glm::mat4{1.0F}, {-2.0F, 3.0F, 0.5F});
    const std::array weights{99.0F, -0.5F};
    const auto world = resolveDrawWorldBounds(*source, model, {}, weights);
    requireBounds(world, {0.0F, -8.5F, 7.0F}, {15.0F, 11.0F, 8.5F});

    const auto moved = resolveDrawWorldBounds(
        *source, glm::translate(glm::mat4{1.0F}, {2.0F, 0.0F, 0.0F}) *
                     model,
        {}, weights);
    requireBounds(moved, {2.0F, -8.5F, 7.0F}, {17.0F, 11.0F, 8.5F});

    const ModelPrimitiveBoundsSource skinned_source{
        .base = ModelPrimitiveBounds{{-1.0F, -1.0F, -1.0F},
                                     {1.0F, 1.0F, 1.0F}},
    };
    const std::array skin_palette{
        glm::translate(glm::mat4{1.0F}, {-3.0F, 0.0F, 0.0F}),
        glm::translate(glm::mat4{1.0F}, {4.0F, 0.0F, 0.0F}),
    };
    const auto skinned = resolveDrawWorldBounds(
        skinned_source,
        glm::translate(glm::mat4{1.0F}, {10.0F, 0.0F, 0.0F}),
        skin_palette);
    requireBounds(skinned, {6.0F, -1.0F, -1.0F},
                  {15.0F, 1.0F, 1.0F});

    auto invalid_index = geometry;
    invalid_index.indices = {3};
    CHECK_THROWS_AS(makePrimitiveBoundsSource(invalid_index),
                    std::runtime_error);

    auto invalid_position = geometry;
    invalid_position.pos[1].x = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS(makePrimitiveBoundsSource(invalid_position),
                    std::runtime_error);

    auto invalid_source = skinned_source;
    invalid_source.base.minimum.x = 2.0F;
    CHECK_THROWS_AS(resolveDrawWorldBounds(invalid_source, glm::mat4{1.0F}),
                    std::invalid_argument);

    invalid_source = skinned_source;
    invalid_source.morph_position_deltas.push_back(ModelPrimitiveBounds{
        {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F},
    });
    const std::array invalid_source_weights{1.0F};
    CHECK_THROWS_AS(resolveDrawWorldBounds(invalid_source, glm::mat4{1.0F},
                                           {}, invalid_source_weights),
                    std::invalid_argument);

    auto extreme = transparentItem(99, 99, 0.0F, 0.0F);
    extreme.world_bounds = DrawWorldBounds{
        .minimum = {std::numeric_limits<float>::max(), 0.0F, 0.0F},
        .maximum = {std::numeric_limits<float>::max(), 0.0F, 0.0F},
    };
    const std::array extreme_input{extreme};
    auto depth_provider = backToFrontProvider();
    CHECK_NOTHROW(DrawQueueBuilder::build(
        DrawQueueBuildRequest{
            .items = extreme_input,
            .max_draw_indirect_count = 1,
            .target_phase = DrawQueuePhase::transparent,
            .logical_view_forward = {1.0F, 0.0F, 0.0F},
        },
        depth_provider));
}

TEST_CASE("RPE5 phase queues preserve opaque batching and sort transparent back to front",
          "[renderer][draw-queue][transparent][wp184]") {
    const std::vector input{
        item(0, 2, 0, false, PrimitiveViewVisibility::both, 0, 30),
        transparentItem(1, 1, 0.0F, -2.0F),
        item(2, 1, 0, false, PrimitiveViewVisibility::both, 2, 20,
             MaterialRouteClass::forward_opaque),
        transparentItem(3, 9, 0.0F, -8.0F),
        transparentItem(4, 4, 0.0F, -8.0F),
    };

    const auto compile = [&] {
        auto opaque_provider = builtinProvider();
        auto transparent_provider = backToFrontProvider();
        std::vector<CompiledDrawQueueVariant> variants;
        variants.push_back({
            .phase = DrawQueuePhase::opaque,
            .sort_view_index = 0,
            .queue = DrawQueueBuilder::build(
                DrawQueueBuildRequest{
                    .items = input,
                    .max_draw_indirect_count = 64,
                    .target_phase = DrawQueuePhase::opaque,
                },
                opaque_provider),
        });
        variants.push_back({
            .phase = DrawQueuePhase::transparent,
            .sort_view_index = 0,
            .queue = DrawQueueBuilder::build(
                DrawQueueBuildRequest{
                    .items = input,
                    .max_draw_indirect_count = 64,
                    .target_phase = DrawQueuePhase::transparent,
                },
                transparent_provider),
        });
        return CompiledDrawQueueSet::combine(std::move(variants));
    };

    const auto first = compile();
    const auto second = compile();
    REQUIRE(first.sortViewCount() == 1);
    REQUIRE(first.queueCount() == 2);

    const auto &opaque = first.queue(DrawQueuePhase::opaque, 0).orderedItems();
    REQUIRE(opaque.size() == 2);
    REQUIRE(opaque[0].pipeline_material_key.material.value == 1);
    REQUIRE(opaque[1].pipeline_material_key.material.value == 2);
    REQUIRE(std::all_of(opaque.begin(), opaque.end(), [](const auto &entry) {
        return entry.phase == MaterialPhase::opaque;
    }));

    const auto &transparent =
        first.queue(DrawQueuePhase::transparent, 0).orderedItems();
    REQUIRE(transparent.size() == 3);
    REQUIRE(transparent[0].stable_identity.instance.index == 4);
    REQUIRE(transparent[1].stable_identity.instance.index == 9);
    REQUIRE(transparent[2].stable_identity.instance.index == 1);
    REQUIRE(std::all_of(
        transparent.begin(), transparent.end(), [](const auto &entry) {
            return entry.phase == MaterialPhase::transparent;
        }));

    REQUIRE(first.indirectRecords().size() == input.size());
    const auto &transparent_ranges = first.drawRanges(
        DrawQueuePhase::transparent, 0, DrawQueueView::third_person);
    REQUIRE(transparent_ranges.size() == 1);
    REQUIRE(transparent_ranges[0].offset == 2 * sizeof(RenderCommand));
    REQUIRE(transparent_ranges[0].draw_count == 3);

    REQUIRE(first.indirectBytes().size() == second.indirectBytes().size());
    REQUIRE(std::equal(first.indirectBytes().begin(),
                       first.indirectBytes().end(),
                       second.indirectBytes().begin()));
    REQUIRE(first.allDrawRanges(0, DrawQueueView::third_person) ==
            second.allDrawRanges(0, DrawQueueView::third_person));
}

TEST_CASE(
    "WP210d draw queue segments assign disjoint GPU output ranges per fixed state",
    "[renderer][draw-queue][gpu-segment][wp210]") {
    const std::vector input{
        item(0, 1, 0, false,
             PrimitiveViewVisibility::both, 0, 0),
        item(1, 2, 0, false,
             PrimitiveViewVisibility::both, 1, 3),
    };
    auto opaque_provider = builtinProvider();
    auto transparent_provider =
        backToFrontProvider();
    std::vector<CompiledDrawQueueVariant>
        variants;
    variants.push_back({
        .phase = DrawQueuePhase::opaque,
        .sort_view_index = 0,
        .queue = DrawQueueBuilder::build(
            DrawQueueBuildRequest{
                .items = input,
                .max_draw_indirect_count = 64,
                .target_phase =
                    DrawQueuePhase::opaque,
            },
            opaque_provider),
    });
    variants.push_back({
        .phase = DrawQueuePhase::transparent,
        .sort_view_index = 0,
        .queue = DrawQueueBuilder::build(
            DrawQueueBuildRequest{
                .items = input,
                .max_draw_indirect_count = 64,
                .target_phase =
                    DrawQueuePhase::transparent,
            },
            transparent_provider),
    });

    const auto compiled =
        CompiledDrawQueueSet::combine(
            std::move(variants));
    const auto &segments =
        compiled.sceneDrawSegments();
    REQUIRE(segments.size() == 4);
    REQUIRE(
        compiled
            .sceneDrawOutputCommandCapacity() ==
        4);

    const auto &third_person =
        compiled.allDrawRanges(
            0,
            DrawQueueView::third_person);
    REQUIRE(third_person.size() == 2);
    CHECK(
        third_person[0]
            .scene_segment_index == 0);
    CHECK(
        third_person[1]
            .scene_segment_index == 1);
    for (std::uint32_t index = 0;
         index < segments.size(); ++index) {
        const auto &segment =
            segments[index];
        CHECK(
            segment.source_first_command ==
            index % 2);
        CHECK(
            segment.command_capacity == 1);
        CHECK(
            segment.output_first_command ==
            index);
        CHECK(
            segment.output_count_index ==
            index);
        CHECK(
            segment.sort_view_index == 0);
        CHECK(segment.phase == 0);
        CHECK(
            segment.visibility_view ==
            index / 2);
        CHECK(
            segment.material_filter_index ==
            noSceneDrawSegmentIndex);
    }
}

TEST_CASE("RPE5 XR queue policy supports one logical center or deterministic per-view order",
          "[renderer][draw-queue][xr][wp184]") {
    const std::vector input{
        transparentItem(0, 10, -2.0F, -5.0F),
        transparentItem(1, 20, 2.0F, -5.0F),
    };

    const auto compile = [&](std::span<const std::array<float, 3>> forwards) {
        std::vector<CompiledDrawQueueVariant> variants;
        for (std::uint32_t view = 0; view < forwards.size(); ++view) {
            auto opaque_provider = builtinProvider();
            auto transparent_provider = backToFrontProvider();
            const auto request = [&](DrawQueuePhase phase) {
                return DrawQueueBuildRequest{
                    .items = input,
                    .max_draw_indirect_count = 64,
                    .target_phase = phase,
                    .logical_view =
                        RenderPolicy::DrawSortLogicalViewV1::first_person,
                    .logical_view_forward = forwards[view],
                };
            };
            variants.push_back({
                .phase = DrawQueuePhase::opaque,
                .sort_view_index = view,
                .queue = DrawQueueBuilder::build(
                    request(DrawQueuePhase::opaque), opaque_provider),
            });
            variants.push_back({
                .phase = DrawQueuePhase::transparent,
                .sort_view_index = view,
                .queue = DrawQueueBuilder::build(
                    request(DrawQueuePhase::transparent),
                    transparent_provider),
            });
        }
        return CompiledDrawQueueSet::combine(std::move(variants));
    };

    const std::array<std::array<float, 3>, 1> logical_center_forwards{
        std::array{0.0F, 0.0F, -1.0F},
    };
    const auto logical_center = compile(logical_center_forwards);
    REQUIRE(logical_center.sortViewCount() == 1);
    const auto &center_order = logical_center
                                   .queue(DrawQueuePhase::transparent, 0)
                                   .orderedItems();
    REQUIRE(center_order[0].stable_identity.instance.index == 10);
    REQUIRE(center_order[1].stable_identity.instance.index == 20);

    const std::array<std::array<float, 3>, 2> per_view_forwards{
        std::array{1.0F, 0.0F, -1.0F},
        std::array{-1.0F, 0.0F, -1.0F},
    };
    const auto per_view = compile(per_view_forwards);
    REQUIRE(per_view.sortViewCount() == 2);
    REQUIRE(per_view.queueCount() == 4);
    const auto &left =
        per_view.queue(DrawQueuePhase::transparent, 0).orderedItems();
    const auto &right =
        per_view.queue(DrawQueuePhase::transparent, 1).orderedItems();
    REQUIRE(left[0].stable_identity.instance.index == 20);
    REQUIRE(left[1].stable_identity.instance.index == 10);
    REQUIRE(right[0].stable_identity.instance.index == 10);
    REQUIRE(right[1].stable_identity.instance.index == 20);

    const auto &left_ranges = per_view.drawRanges(
        DrawQueuePhase::transparent, 0, DrawQueueView::third_person);
    const auto &right_ranges = per_view.drawRanges(
        DrawQueuePhase::transparent, 1, DrawQueueView::third_person);
    REQUIRE(left_ranges.size() == 1);
    REQUIRE(right_ranges.size() == 1);
    REQUIRE(left_ranges[0].offset == 0);
    REQUIRE(right_ranges[0].offset == 2 * sizeof(RenderCommand));
}

} // namespace Pelican
