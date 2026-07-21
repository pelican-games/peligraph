#include "../src/core/renderer/drawqueuebuilder.hpp"
#include "../src/core/renderer/indirectdrawlimits.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <tuple>
#include <vector>

namespace Pelican {
namespace {

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
    const auto compiled = DrawQueueBuilder::build(
        DrawQueueBuildRequest{.items = input,
                              .max_draw_indirect_count = 2});

    REQUIRE(input == original);
    REQUIRE(compiled.policy() == DrawQueuePolicy::state_batched_v1);
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

    const auto compiled = DrawQueueBuilder::build(
        DrawQueueBuildRequest{.items = input,
                              .max_draw_indirect_count = 2});
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
    const auto first = DrawQueueBuilder::build(request);
    const auto second = DrawQueueBuilder::build(request);

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
        DrawQueueBuildRequest{.items = {}, .max_draw_indirect_count = 0});
    REQUIRE(empty.empty());

    CHECK_THROWS_AS(DrawQueueBuilder::build(DrawQueueBuildRequest{
                        .items = input, .max_draw_indirect_count = 0}),
                    std::invalid_argument);

    auto invalid_phase = input;
    invalid_phase.front().phase = MaterialPhase::transparent;
    CHECK_THROWS_AS(DrawQueueBuilder::build(DrawQueueBuildRequest{
                        .items = invalid_phase,
                        .max_draw_indirect_count = 64}),
                    std::invalid_argument);

    auto duplicate_ordinal = input;
    duplicate_ordinal.back().declaration_ordinal =
        duplicate_ordinal.front().declaration_ordinal;
    CHECK_THROWS_AS(DrawQueueBuilder::build(DrawQueueBuildRequest{
                        .items = duplicate_ordinal,
                        .max_draw_indirect_count = 64}),
                    std::invalid_argument);
}

} // namespace Pelican
