#include "../src/core/animation/animationjobs.hpp"
#include "../src/core/animation/animationprobe.hpp"
#include "../src/core/model/skeletalanimation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

namespace Pelican::Animation {
namespace {

PoseViewV1 poseDescriptor() {
    PoseViewV1 result{};
    result.struct_size = sizeof(result);
    result.version = descriptorVersionV1;
    return result;
}

SkeletalModelData assetFixture() {
    SkeletalModelData model;
    // Deliberately child-before-parent in the source. The runtime layout must be
    // parent-before-child without losing source node names or channel targets.
    model.nodes = {
        {.parent = 1, .translation = {0.0f, 1.0f, 0.0f}, .name = "Tip"},
        {.parent = -1, .name = "Root"},
    };
    model.joint_nodes = {0};
    model.inverse_bind_matrices = {glm::mat4{1.0f}};
    model.skin_bindings = {{"Body", 0, 1}};
    model.clips.push_back({
        .name = "Move", .start = 0.0f, .end = 1.0f,
        .channels = {{.node = 0, .path = AnimationPath::translation,
                      .times = {0.0f, 1.0f}, .values = {{0, 1, 0, 0}, {2, 1, 0, 0}}}},
    });
    return model;
}

} // namespace

TEST_CASE("A1 jobs build a named real-asset rig and preserve the WP38 palette bytes", "[animation][a1]") {
    const auto model = assetFixture();
    AnimationAssetRegistry registry;
    const auto &asset = registry.getOrCreate(model);
    REQUIRE(asset.rig.node_names == std::vector<std::string>{"Root", "Tip"});
    REQUIRE(asset.rig.parents == std::vector<std::int32_t>{-1, 0});
    REQUIRE(asset.skin_bindings.size() == 1);
    REQUIRE(asset.skin_bindings[0].joint_layout_nodes == std::vector<std::uint32_t>{1});

    ProbeRuntime arena{4096};
    const auto frame = arena.beginFrame(1);
    auto local = poseDescriptor();
    auto model_pose = poseDescriptor();
    REQUIRE(arena.acquirePose(frame, asset.rig.layout, 2, local) == Status::ok);
    REQUIRE(arena.acquirePose(frame, asset.rig.layout, 2, model_pose) == Status::ok);
    const auto &clip = findClip(asset, model.clips[0]);
    REQUIRE(samplePoseAt(asset, &clip, 0.5, 1.0, false, 0.0, local) == Status::ok);
    REQUIRE(local.translations[1].x == Catch::Approx(1.0f));

    std::array<Matrix4fV1, 2> model_matrices{};
    REQUIRE(localToModel(asset.rig, local, model_matrices, &model_pose) == Status::ok);
    std::array<Matrix4fV1, 1> palette{};
    REQUIRE(buildSkinPalette(asset, model_matrices, palette) == Status::ok);
    const auto legacy = evaluateSkinPalette(model, &model.clips[0], 0.5, 1.0, false, 0.0);
    REQUIRE(legacy.size() == palette.size());
    REQUIRE(std::memcmp(legacy.data(), palette.data(), sizeof(glm::mat4)) == 0);
}

TEST_CASE("A1 normal blend applies per-joint weights and the frozen N-way quaternion rule",
          "[animation][a1][golden]") {
    const auto model = assetFixture();
    AnimationAssetRegistry registry;
    const auto &asset = registry.getOrCreate(model);
    ProbeRuntime arena{8192};
    const auto frame = arena.beginFrame(7);
    auto first = poseDescriptor();
    auto second = poseDescriptor();
    auto output = poseDescriptor();
    REQUIRE(arena.acquirePose(frame, asset.rig.layout, 2, first) == Status::ok);
    REQUIRE(arena.acquirePose(frame, asset.rig.layout, 2, second) == Status::ok);
    REQUIRE(arena.acquirePose(frame, asset.rig.layout, 2, output) == Status::ok);
    first.translations[1].x = 2.0f;
    second.translations[1].x = 10.0f;
    constexpr float s = 0.7071067811865475f;
    first.rotations[1] = {0, 0, 0, 1};
    second.rotations[1] = {0, 0, -s, -s};
    const std::array<float, 2> first_mask{1.0f, 1.0f};
    const std::array<float, 2> second_mask{1.0f, 0.0f};
    const std::array inputs{
        NormalBlendInput{&first, 0.25f, first_mask},
        NormalBlendInput{&second, 0.75f, second_mask},
    };
    REQUIRE(blendNormal(asset.rig, inputs, output) == Status::ok);
    REQUIRE(output.translations[0].x == Catch::Approx(0.0f));
    REQUIRE(output.translations[1].x == Catch::Approx(2.0f));
    REQUIRE(output.rotations[1].w == Catch::Approx(1.0f));
}

TEST_CASE("A1 clamp cursor emits each endpoint interval once without repeating past the clamp",
          "[animation][a1][interval]") {
    ProbeRuntime runtime;
    const std::array annotations{
        ProbeAnnotation{.time_seconds = 0.25, .source = 0, .ordinal = 0, .identity = 1},
        ProbeAnnotation{.time_seconds = 0.75, .source = 0, .ordinal = 1, .identity = 2},
    };
    const auto cursor = runtime.createCursor(1.0, WrapMode::clamp, annotations);
    auto advance = AdvanceDescV1{};
    advance.struct_size = sizeof(advance);
    advance.version = descriptorVersionV1;
    advance.cursor = cursor;
    advance.delta_seconds = 3.0;
    auto query = IntervalResultV1{};
    query.struct_size = sizeof(query);
    query.version = descriptorVersionV1;
    REQUIRE(runtime.advanceCursor(advance, query) == Status::buffer_too_small);
    REQUIRE(query.crossing_count == 2);
    REQUIRE(runtime.cursorTime(cursor) == 0.0);

    std::array<CrossingV1, 2> crossings{};
    auto result = IntervalResultV1{};
    result.struct_size = sizeof(result);
    result.version = descriptorVersionV1;
    result.crossings = crossings.data();
    result.crossing_capacity = static_cast<std::uint32_t>(crossings.size());
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(runtime.cursorTime(cursor) == 1.0);
    result.crossing_count = 99;
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.crossing_count == 0);
    REQUIRE(runtime.cursorTime(cursor) == 1.0);
}

} // namespace Pelican::Animation
