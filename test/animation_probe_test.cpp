#include "../src/core/animation/animationprobe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ranges>
#include <thread>
#include <vector>

namespace Pelican::Animation {
namespace {

template <class T> T descriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

nlohmann::json golden() {
    std::ifstream input(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test/fixtures/animation_probe_golden.json");
    REQUIRE(input.good());
    return nlohmann::json::parse(input);
}

PublishAnimationFrameDescV1 publishDesc(InstanceHandle instance, std::uint64_t revision, std::uint32_t flags = 0) {
    auto result = descriptor<PublishAnimationFrameDescV1>();
    result.instance = instance;
    result.frame_revision = revision;
    result.flags = flags;
    result.root_delta.rotation.w = 1.0f;
    return result;
}

} // namespace

TEST_CASE("pose arena is aligned frame-owned and independently usable by two actors", "[animation][a0]") {
    ProbeRuntime runtime{4096};
    const PoseLayoutHandle layout{9, 3, 0};
    const auto frame1 = runtime.beginFrame(10);
    auto first = descriptor<PoseViewV1>();
    auto second = descriptor<PoseViewV1>();
    REQUIRE(runtime.acquirePose(frame1, layout, 4, first) == Status::ok);
    REQUIRE(runtime.acquirePose(frame1, layout, 4, second) == Status::ok);
    REQUIRE(reinterpret_cast<std::uintptr_t>(first.translations) % poseAlignmentV1 == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(first.rotations) % poseAlignmentV1 == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(first.scales) % poseAlignmentV1 == 0);
    REQUIRE(first.translations != second.translations);
    first.translations[0].x = 17.0f;
    REQUIRE(second.translations[0].x == 0.0f);

    const auto stale_pose = first.pose;
    const auto frame2 = runtime.beginFrame(11);
    REQUIRE(frame2.generation != frame1.generation);
    REQUIRE(runtime.validatePose(stale_pose) == Status::stale_generation);
    REQUIRE(runtime.validatePose(PoseHandle{999, frame2.generation, 0}) == Status::invalid_handle);
    auto stale_output = descriptor<PoseViewV1>();
    REQUIRE(runtime.acquirePose(frame1, layout, 1, stale_output) == Status::stale_generation);

    std::atomic_uint32_t ready{0};
    std::atomic_bool go{false};
    std::atomic_bool parallel_ok{true};
    auto actor = [&] {
        ProbeRuntime local{2048};
        ++ready;
        while (!go.load()) std::this_thread::yield();
        auto view = descriptor<PoseViewV1>();
        const auto arena = local.beginFrame(1);
        if (local.acquirePose(arena, layout, 8, view) != Status::ok) parallel_ok = false;
        if (reinterpret_cast<std::uintptr_t>(view.translations) % poseAlignmentV1 != 0) parallel_ok = false;
    };
    std::thread a{actor};
    std::thread b{actor};
    while (ready.load() != 2) std::this_thread::yield();
    go = true;
    a.join();
    b.join();
    REQUIRE(parallel_ok.load());
}

TEST_CASE("N-way quaternion reference matches the frozen golden", "[animation][a0][golden]") {
    constexpr float s = 0.7071067811865475f;
    const std::array values{QuatfV1{0, 0, 0, 1}, QuatfV1{0, s, 0, s}, QuatfV1{0, -s, 0, -s}};
    const std::array weights{0.25f, 0.50f, 0.25f};
    const auto result = ProbeRuntime::blendQuaternions(values, weights);
    const auto expected = golden().at("quaternion");
    REQUIRE(result.x == Catch::Approx(expected[0].get<float>()).margin(1e-6));
    REQUIRE(result.y == Catch::Approx(expected[1].get<float>()).margin(1e-6));
    REQUIRE(result.z == Catch::Approx(expected[2].get<float>()).margin(1e-6));
    REQUIRE(result.w == Catch::Approx(expected[3].get<float>()).margin(1e-6));

    const std::array zero_weights{0.0f, 0.0f, 0.0f};
    const auto zero = ProbeRuntime::blendQuaternions(values, zero_weights);
    REQUIRE(zero.x == 0.0f);
    REQUIRE(zero.y == 0.0f);
    REQUIRE(zero.z == 0.0f);
    REQUIRE(zero.w == 1.0f);
}

TEST_CASE("interval advance is atomic and handles wrap reverse multi-loop and seek", "[animation][a0][golden]") {
    ProbeRuntime runtime;
    const std::array annotations{
        ProbeAnnotation{.time_seconds = 0.25, .source = 2, .ordinal = 1, .identity = 21},
        ProbeAnnotation{.time_seconds = 0.25, .source = 1, .ordinal = 9, .identity = 19},
    };
    const auto cursor = runtime.createCursor(1.0, WrapMode::repeat, annotations);

    auto place = descriptor<AdvanceDescV1>();
    place.cursor = cursor;
    place.flags = advance_absolute_seek;
    place.absolute_seconds = 0.8;
    auto place_result = descriptor<IntervalResultV1>();
    REQUIRE(runtime.advanceCursor(place, place_result) == Status::ok);
    REQUIRE(runtime.cursorTime(cursor) == Catch::Approx(0.8));

    auto advance = descriptor<AdvanceDescV1>();
    advance.cursor = cursor;
    advance.delta_seconds = 2.5;
    advance.reserved2 = 1;
    auto validation_result = descriptor<IntervalResultV1>();
    REQUIRE(runtime.advanceCursor(advance, validation_result) == Status::reserved_not_zero);
    REQUIRE(runtime.cursorTime(cursor) == Catch::Approx(0.8));
    advance.reserved2 = 0;
    auto query = descriptor<IntervalResultV1>();
    REQUIRE(runtime.advanceCursor(advance, query) == Status::buffer_too_small);
    REQUIRE(query.crossing_count == 6);
    REQUIRE(runtime.cursorTime(cursor) == Catch::Approx(0.8));

    std::vector<CrossingV1> crossings(query.crossing_count);
    auto result = descriptor<IntervalResultV1>();
    result.crossings = crossings.data();
    result.crossing_capacity = static_cast<std::uint32_t>(crossings.size());
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(runtime.cursorTime(cursor) == Catch::Approx(0.3));
    REQUIRE((result.result_flags & interval_looped) != 0);
    const auto fixture = golden();
    for (std::size_t i = 0; i < crossings.size(); ++i) {
        REQUIRE(crossings[i].source == fixture["forward_sources"][i].get<std::uint32_t>());
        REQUIRE(crossings[i].loop_index == fixture["forward_loops"][i].get<std::int64_t>());
    }

    advance.delta_seconds = -2.2;
    query = descriptor<IntervalResultV1>();
    REQUIRE(runtime.advanceCursor(advance, query) == Status::buffer_too_small);
    REQUIRE(query.crossing_count == 6);
    REQUIRE(runtime.cursorTime(cursor) == Catch::Approx(0.3));
    crossings.assign(query.crossing_count, {});
    result = descriptor<IntervalResultV1>();
    result.crossings = crossings.data();
    result.crossing_capacity = static_cast<std::uint32_t>(crossings.size());
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE((result.result_flags & interval_reverse) != 0);
    REQUIRE(runtime.cursorTime(cursor) == Catch::Approx(0.1));
    for (std::size_t i = 0; i < crossings.size(); ++i) {
        REQUIRE(crossings[i].source == fixture["reverse_sources"][i].get<std::uint32_t>());
        REQUIRE(crossings[i].loop_index == fixture["reverse_loops"][i].get<std::int64_t>());
    }

    advance.delta_seconds = 99.0;
    advance.flags = advance_absolute_seek | advance_discontinuity;
    advance.absolute_seconds = 4.25;
    result = descriptor<IntervalResultV1>();
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.crossing_count == 0);
    REQUIRE((result.result_flags & interval_seeked) != 0);
    REQUIRE(result.normalized_phase == Catch::Approx(0.25f));

    REQUIRE(runtime.destroyCursor(cursor) == Status::ok);
    REQUIRE(runtime.advanceCursor(advance, result) == Status::stale_generation);
}

TEST_CASE("interval advance bounds hostile repeat traversal", "[animation][a0][hostile]") {
    ProbeRuntime runtime;
    const auto cursor = runtime.createCursor(1.0, WrapMode::repeat);
    REQUIRE(isValid(cursor));

    auto advance = descriptor<AdvanceDescV1>();
    advance.cursor = cursor;
    auto result = descriptor<IntervalResultV1>();
    result.crossing_count = 17;
    result.normalized_phase = 0.75f;

    advance.delta_seconds = std::numeric_limits<double>::max();
    REQUIRE(runtime.advanceCursor(advance, result) == Status::invalid_argument);
    REQUIRE(runtime.cursorTime(cursor) == 0.0);
    REQUIRE(result.crossing_count == 17);
    REQUIRE(result.normalized_phase == 0.75f);

    advance.delta_seconds =
        static_cast<double>(maxIntervalTraversalLoopsV1);
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(runtime.cursorTime(cursor) == 0.0);
    REQUIRE(result.crossing_count == 0);
    REQUIRE((result.result_flags & interval_looped) != 0);
}

TEST_CASE("interval advance caps reported crossings without moving cursor",
          "[animation][a0][hostile]") {
    ProbeRuntime runtime;
    const std::array annotations{
        ProbeAnnotation{.time_seconds = 0.25, .source = 0, .ordinal = 0,
                        .identity = 1},
        ProbeAnnotation{.time_seconds = 0.75, .source = 0, .ordinal = 1,
                        .identity = 2},
    };
    const auto cursor = runtime.createCursor(1.0, WrapMode::repeat, annotations);
    auto advance = descriptor<AdvanceDescV1>();
    advance.cursor = cursor;
    advance.delta_seconds =
        static_cast<double>(maxIntervalCrossingsV1 / annotations.size() + 1);
    auto result = descriptor<IntervalResultV1>();

    REQUIRE(runtime.advanceCursor(advance, result) == Status::invalid_argument);
    REQUIRE(runtime.cursorTime(cursor) == 0.0);
    REQUIRE(result.crossing_count == 0);
}

TEST_CASE("local-to-model and commit history complete one frozen round trip", "[animation][a0][golden]") {
    const std::array local{
        TransformV1{{1, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 0}},
        TransformV1{{0, 2, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 0}},
    };
    const std::array<std::int32_t, 2> parents{-1, 0};
    std::array<Matrix4fV1, 2> model{};
    REQUIRE(ProbeRuntime::localToModel(local, parents, model) == Status::ok);
    const auto expected = golden().at("child_model_translation");
    REQUIRE(model[1].column_major[12] == Catch::Approx(expected[0].get<float>()));
    REQUIRE(model[1].column_major[13] == Catch::Approx(expected[1].get<float>()));
    REQUIRE(model[1].column_major[14] == Catch::Approx(expected[2].get<float>()));

    ProbeRuntime runtime;
    const InstanceHandle instance{42, 7, 0};
    auto first = publishDesc(instance, 10, commit_reset_history);
    REQUIRE(runtime.publishAnimationFrame(first) == Status::ok);
    REQUIRE(runtime.currentRevision(instance) == 10);
    REQUIRE(runtime.previousRevision(instance) == 10);
    auto history = descriptor<AdvanceTemporalHistoryDescV1>();
    history.rendered_frame_revision = 10;
    history.velocity_consumed = 1;
    REQUIRE(runtime.advanceTemporalHistoryAfterRender(history) == Status::ok);

    auto second = publishDesc(instance, 11);
    REQUIRE(runtime.publishAnimationFrame(second) == Status::ok);
    REQUIRE(runtime.currentRevision(instance) == 11);
    REQUIRE(runtime.previousRevision(instance) == 10);
    history.rendered_frame_revision = 11;
    REQUIRE(runtime.advanceTemporalHistoryAfterRender(history) == Status::ok);
    REQUIRE(runtime.previousRevision(instance) == 11);
    REQUIRE(runtime.advanceTemporalHistoryAfterRender(history) == Status::duplicate_revision);
    history.rendered_frame_revision = 99;
    REQUIRE(runtime.advanceTemporalHistoryAfterRender(history) == Status::invalid_argument);
}

TEST_CASE("API descriptor negotiation ignores an unknown caller tail", "[animation][a0][abi]") {
    struct ExtendedApi { ApiV1 api; std::array<std::byte, 32> tail; };
    ExtendedApi extended{};
    extended.api = descriptor<ApiV1>();
    extended.api.struct_size = sizeof(extended);
    extended.tail.fill(std::byte{0x5a});
    REQUIRE(getApiV1(1, &extended.api) == Status::ok);
    REQUIRE(extended.api.engine_abi_version == 1);
    REQUIRE(extended.api.advance_cursor != nullptr);
    REQUIRE(std::ranges::all_of(extended.tail, [](std::byte value) { return value == std::byte{0x5a}; }));
    REQUIRE(getApiV1(2, &extended.api) == Status::unsupported_version);
}

} // namespace Pelican::Animation
