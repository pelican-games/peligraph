#include "../src/core/animation/animationjobs.hpp"
#include "../src/core/animation/animationprobe.hpp"
#include "../src/core/model/skeletalanimation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <utility>
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

TEST_CASE("WP99 fixture 1 keeps a rotating non-joint ancestor in the skin palette", "[animation][a1.5]") {
    // 仕様引用: design_animation_graph.md §1-1
    // 「全アニメ階層(skin joint 集合ではない — non-joint ノードを含む)」
    SkeletalModelData model;
    constexpr float s = 0.7071067811865475f;
    model.nodes = {
        {.parent = -1, .name = "Root"},
        {.parent = 0, .rotation = glm::quat{s, 0.0f, 0.0f, s}, .name = "RotatingNonJoint"},
        {.parent = 1, .translation = {1.0f, 0.0f, 0.0f}, .name = "OnlySkinJoint"},
    };
    model.joint_nodes = {2};
    model.inverse_bind_matrices = {glm::mat4{1.0f}};

    AnimationAssetRegistry registry;
    const auto &asset = registry.getOrCreate(model);
    REQUIRE(asset.rig.rest_pose.size() == 3);
    REQUIRE(asset.skin_bindings[0].joint_layout_nodes == std::vector<std::uint32_t>{2});

    ProbeRuntime arena{4096};
    auto local = poseDescriptor();
    REQUIRE(arena.acquirePose(arena.beginFrame(1), asset.rig.layout, 3, local) == Status::ok);
    REQUIRE(samplePoseAt(asset, nullptr, 0.0, 1.0, false, 0.0, local) == Status::ok);
    std::array<Matrix4fV1, 3> model_matrices{};
    std::array<Matrix4fV1, 1> palette{};
    REQUIRE(localToModel(asset.rig, local, model_matrices) == Status::ok);
    REQUIRE(buildSkinPalette(asset, model_matrices, palette) == Status::ok);
    REQUIRE(palette[0].column_major[12] == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(palette[0].column_major[13] == Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("WP99 fixture 2 rejects equal-count poses from a different rig layout", "[animation][a1.5]") {
    // 仕様引用: animation_abi_v1.md §2
    // 「同じ node 数、同じ名前集合、同じ byte size は互換性の根拠にならない。」
    auto first_model = assetFixture();
    auto second_model = assetFixture();
    second_model.nodes[0].name = "OtherTip";
    second_model.nodes[1].name = "OtherRoot";
    AnimationAssetRegistry registry;
    const auto &first_asset = registry.getOrCreate(first_model);
    const auto &second_asset = registry.getOrCreate(second_model);
    REQUIRE(first_asset.rig.rest_pose.size() == second_asset.rig.rest_pose.size());
    REQUIRE(first_asset.rig.layout.identity != second_asset.rig.layout.identity);

    ProbeRuntime arena{8192};
    const auto frame = arena.beginFrame(2);
    auto foreign = poseDescriptor();
    auto output = poseDescriptor();
    REQUIRE(arena.acquirePose(frame, first_asset.rig.layout, 2, foreign) == Status::ok);
    REQUIRE(arena.acquirePose(frame, second_asset.rig.layout, 2, output) == Status::ok);
    output.translations[0].x = 123.0f;
    const std::array inputs{NormalBlendInput{&foreign, 1.0f, {}}};
    REQUIRE(blendNormal(second_asset.rig, inputs, output) == Status::incompatible_layout);
    REQUIRE(output.translations[0].x == 123.0f);
}

TEST_CASE("WP99 fixture 3 preserves event order loop indexes and root delta across loops reverse and seek",
          "[animation][a1.5]") {
    // 仕様引用: animation_abi_v1.md §4.2
    // 「0/1/N loop を unwrapped time 上で列挙し、結果の loop index を保持する。」
    // 「同時刻は再生方向によらず (source, ordinal) の昇順」「seek は crossing を emit せず root delta を identity」
    ProbeRuntime runtime;
    const std::array annotations{
        ProbeAnnotation{.time_seconds = 0.25, .source = 2, .ordinal = 1, .identity = 21},
        ProbeAnnotation{.time_seconds = 0.25, .source = 1, .ordinal = 9, .identity = 19},
        ProbeAnnotation{.time_seconds = 0.75, .source = 3, .ordinal = 0, .identity = 30},
    };
    const auto cursor = runtime.createCursor(1.0, WrapMode::repeat, annotations);
    AdvanceDescV1 advance{};
    advance.struct_size = sizeof(advance);
    advance.version = descriptorVersionV1;
    advance.cursor = cursor;
    advance.flags = advance_absolute_seek;
    advance.absolute_seconds = 0.8;
    IntervalResultV1 seek{};
    seek.struct_size = sizeof(seek);
    seek.version = descriptorVersionV1;
    REQUIRE(runtime.advanceCursor(advance, seek) == Status::ok);
    REQUIRE(seek.crossing_count == 0);
    REQUIRE(seek.root_delta.translation.x == 0.0f);
    REQUIRE(seek.root_delta.rotation.w == 1.0f);

    advance.flags = advance_none;
    advance.delta_seconds = 2.5;
    IntervalResultV1 query{};
    query.struct_size = sizeof(query);
    query.version = descriptorVersionV1;
    REQUIRE(runtime.advanceCursor(advance, query) == Status::buffer_too_small);
    REQUIRE(query.crossing_count == 8);
    std::array<CrossingV1, 8> forward{};
    IntervalResultV1 result{};
    result.struct_size = sizeof(result);
    result.version = descriptorVersionV1;
    result.crossings = forward.data();
    result.crossing_capacity = static_cast<std::uint32_t>(forward.size());
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.root_delta.translation.x == Catch::Approx(2.5f));
    REQUIRE((result.result_flags & interval_looped) != 0);
    REQUIRE(forward[0].source == 1);
    REQUIRE(forward[1].source == 2);
    REQUIRE(forward[0].loop_index == 1);
    REQUIRE(forward[7].loop_index == 3);

    advance.delta_seconds = -2.2;
    query = {};
    query.struct_size = sizeof(query);
    query.version = descriptorVersionV1;
    REQUIRE(runtime.advanceCursor(advance, query) == Status::buffer_too_small);
    std::vector<CrossingV1> reverse(query.crossing_count);
    result = {};
    result.struct_size = sizeof(result);
    result.version = descriptorVersionV1;
    result.crossings = reverse.data();
    result.crossing_capacity = static_cast<std::uint32_t>(reverse.size());
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.root_delta.translation.x == Catch::Approx(-2.2f));
    REQUIRE((result.result_flags & interval_reverse) != 0);
    for (std::size_t i = 1; i < reverse.size(); ++i) {
        const auto previous_time = reverse[i - 1].clip_time_seconds + reverse[i - 1].loop_index;
        const auto current_time = reverse[i].clip_time_seconds + reverse[i].loop_index;
        REQUIRE(previous_time >= current_time);
        if (previous_time == current_time)
            REQUIRE(std::pair{reverse[i - 1].source, reverse[i - 1].ordinal} <
                    std::pair{reverse[i].source, reverse[i].ordinal});
    }

    advance.flags = advance_absolute_seek | advance_discontinuity;
    advance.absolute_seconds = 4.6;
    advance.delta_seconds = 0.0;
    result = {};
    result.struct_size = sizeof(result);
    result.version = descriptorVersionV1;
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.crossing_count == 0);
    REQUIRE(result.root_delta.translation.x == 0.0f);
    REQUIRE(result.root_delta.rotation.w == 1.0f);
    REQUIRE(result.normalized_phase == Catch::Approx(0.6f));
}

TEST_CASE("WP99 fixture 4 N-way blend is byte stable under source permutation", "[animation][a1.5]") {
    // 仕様引用: design_animation_graph.md §6 / animation_abi_v1.md §6
    // 「同一 build/architecture での byte 一致」「sign 正準化」「0 以下の weight は 0」
    const auto model = assetFixture();
    AnimationAssetRegistry registry;
    const auto &asset = registry.getOrCreate(model);
    ProbeRuntime arena{32768};
    const auto frame = arena.beginFrame(4);
    std::array<PoseViewV1, 4> poses{};
    for (auto &pose : poses) {
        pose = poseDescriptor();
        REQUIRE(arena.acquirePose(frame, asset.rig.layout, 2, pose) == Status::ok);
    }
    constexpr float s = 0.7071067811865475f;
    poses[0].translations[1].x = 0.0f;
    poses[0].rotations[1] = {0, 0, 0, 1};
    poses[1].translations[1].x = 4.0f;
    poses[1].rotations[1] = {0, 0, s, s};
    poses[2].translations[1].x = 4.0f;
    poses[2].rotations[1] = {0, 0, -s, -s};
    poses[3].translations[1].x = 1000.0f;
    poses[3].rotations[1] = {1, 0, 0, 0};
    const std::array weights{0.25f, 0.5f, 0.25f, -0.0f};
    std::array<std::uint32_t, 4> order{0, 1, 2, 3};
    std::array<Vec4fV1, 2> expected_translations{};
    std::array<QuatfV1, 2> expected_rotations{};
    std::array<Vec4fV1, 2> expected_scales{};
    bool first = true;
    do {
        std::array<NormalBlendInput, 4> inputs{};
        for (std::size_t i = 0; i < order.size(); ++i)
            inputs[i] = {&poses[order[i]], weights[order[i]], {}};
        auto output = poseDescriptor();
        REQUIRE(arena.acquirePose(frame, asset.rig.layout, 2, output) == Status::ok);
        REQUIRE(blendNormal(asset.rig, inputs, output) == Status::ok);
        if (first) {
            std::memcpy(expected_translations.data(), output.translations, sizeof(expected_translations));
            std::memcpy(expected_rotations.data(), output.rotations, sizeof(expected_rotations));
            std::memcpy(expected_scales.data(), output.scales, sizeof(expected_scales));
            first = false;
        } else {
            CAPTURE(order[0], order[1], order[2], order[3]);
            CAPTURE(expected_rotations[1].x, expected_rotations[1].y,
                    expected_rotations[1].z, expected_rotations[1].w,
                    output.rotations[1].x, output.rotations[1].y,
                    output.rotations[1].z, output.rotations[1].w);
            REQUIRE(std::memcmp(expected_translations.data(), output.translations, sizeof(expected_translations)) == 0);
            REQUIRE(std::memcmp(expected_rotations.data(), output.rotations, sizeof(expected_rotations)) == 0);
            REQUIRE(std::memcmp(expected_scales.data(), output.scales, sizeof(expected_scales)) == 0);
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_CASE("WP99 fixture 5 invalidates rig clip cursor and pose handles on generation advance",
          "[animation][a1.5]") {
    // 仕様引用: animation_abi_v1.md §2-3
    // 「unload/reload/destroy は参照メモリを解放する前に generation を進める」
    // 「同じ identity の古い handle は stale_generation」「失敗時は部分更新しない」
    const auto model = assetFixture();
    AnimationAssetRegistry registry;
    const AnimationAsset stale_asset = registry.getOrCreate(model);
    const AnimationClipResource stale_clip = stale_asset.clips.front();
    ProbeRuntime arena{8192};
    auto stale_pose = poseDescriptor();
    REQUIRE(arena.acquirePose(arena.beginFrame(1), stale_asset.rig.layout, 2, stale_pose) == Status::ok);
    const auto stale_pose_handle = stale_pose.pose;
    const auto cursor = arena.createCursor(1.0, WrapMode::repeat);

    auto same_layout_pose = poseDescriptor();
    REQUIRE(arena.acquirePose(arena.beginFrame(2), stale_asset.rig.layout, 2, same_layout_pose) == Status::ok);
    same_layout_pose.translations[0].x = 66.0f;
    REQUIRE(samplePoseAt(stale_asset, &stale_clip, 0.5, 1.0, false, 0.0, stale_pose) ==
            Status::stale_generation);
    REQUIRE(same_layout_pose.translations[0].x == 66.0f);
    const std::array stale_inputs{NormalBlendInput{&stale_pose, 1.0f, {}}};
    REQUIRE(blendNormal(stale_asset.rig, stale_inputs, same_layout_pose) == Status::stale_generation);
    REQUIRE(same_layout_pose.translations[0].x == 66.0f);
    std::array<Matrix4fV1, 2> stale_matrices{};
    stale_matrices[0].column_major[0] = 33.0f;
    REQUIRE(localToModel(stale_asset.rig, stale_pose, stale_matrices) == Status::stale_generation);
    REQUIRE(stale_matrices[0].column_major[0] == 33.0f);

    registry.clear();
    const auto &current_asset = registry.getOrCreate(model);
    REQUIRE(stale_asset.rig.generation_state->current.load() != stale_asset.rig.handle.generation);
    REQUIRE(stale_clip.generation_state->current.load() != stale_clip.handle.generation);
    auto current_pose = poseDescriptor();
    REQUIRE(arena.acquirePose(arena.beginFrame(3), current_asset.rig.layout, 2, current_pose) == Status::ok);
    REQUIRE(arena.validatePose(stale_pose_handle) == Status::stale_generation);

    current_pose.translations[0].x = 77.0f;
    REQUIRE(samplePoseAt(current_asset, &stale_clip, 0.5, 1.0, false, 0.0, current_pose) ==
            Status::stale_generation);
    REQUIRE(current_pose.translations[0].x == 77.0f);
    const std::array inputs{NormalBlendInput{&current_pose, 1.0f, {}}};
    REQUIRE(blendNormal(stale_asset.rig, inputs, current_pose) == Status::stale_generation);
    REQUIRE(current_pose.translations[0].x == 77.0f);
    std::array<Matrix4fV1, 2> matrices{};
    matrices[0].column_major[0] = 55.0f;
    REQUIRE(localToModel(stale_asset.rig, current_pose, matrices) == Status::stale_generation);
    REQUIRE(matrices[0].column_major[0] == 55.0f);
    std::array<Matrix4fV1, 1> palette{};
    palette[0].column_major[0] = 44.0f;
    REQUIRE(buildSkinPalette(stale_asset, matrices, palette) == Status::stale_generation);
    REQUIRE(palette[0].column_major[0] == 44.0f);

    REQUIRE(arena.destroyCursor(cursor) == Status::ok);
    REQUIRE(arena.destroyCursor(cursor) == Status::stale_generation);
    AdvanceDescV1 advance{};
    advance.struct_size = sizeof(advance);
    advance.version = descriptorVersionV1;
    advance.cursor = cursor;
    advance.delta_seconds = 0.5;
    IntervalResultV1 result{};
    result.struct_size = sizeof(result);
    result.version = descriptorVersionV1;
    result.normalized_phase = 0.75f;
    REQUIRE(arena.advanceCursor(advance, result) == Status::stale_generation);
    REQUIRE(result.normalized_phase == 0.75f);
}

TEST_CASE("WP99 fixture 6 evaluates and commits two actors in parallel without cross-talk",
          "[animation][a1.5]") {
    // 仕様引用: animation_abi_v1.md §3 / §9
    // 「並列 actor 評価は thread ごとに独立 arena」「commit は instance identity へ publish」
    const auto model = assetFixture();
    AnimationAssetRegistry registry;
    const auto &asset = registry.getOrCreate(model);
    const auto &clip = findClip(asset, model.clips[0]);
    ProbeRuntime commits;
    const std::array instances{InstanceHandle{101, 1, 0}, InstanceHandle{202, 1, 0}};
    std::array<std::array<Matrix4fV1, 1>, 2> palettes{};
    std::array<std::uintptr_t, 2> arena_addresses{};
    std::atomic_uint32_t ready{0};
    std::atomic_bool go{false};
    std::atomic_bool ok{true};
    auto evaluate = [&](std::size_t actor) {
        ProbeRuntime local_arena{8192};
        auto local = poseDescriptor();
        auto model_pose = poseDescriptor();
        const auto frame = local_arena.beginFrame(actor + 1);
        if (local_arena.acquirePose(frame, asset.rig.layout, 2, local) != Status::ok ||
            local_arena.acquirePose(frame, asset.rig.layout, 2, model_pose) != Status::ok) {
            ok = false;
            return;
        }
        arena_addresses[actor] = reinterpret_cast<std::uintptr_t>(local.translations);
        ++ready;
        while (!go.load()) std::this_thread::yield();
        if (samplePoseAt(asset, &clip, static_cast<double>(actor), 1.0, false, 0.0, local) != Status::ok) ok = false;
        std::array<Matrix4fV1, 2> matrices{};
        if (localToModel(asset.rig, local, matrices, &model_pose) != Status::ok ||
            buildSkinPalette(asset, matrices, palettes[actor]) != Status::ok)
            ok = false;
        PublishAnimationFrameDescV1 publish{};
        publish.struct_size = sizeof(publish);
        publish.version = descriptorVersionV1;
        publish.instance = instances[actor];
        publish.local_pose = local.pose;
        publish.model_pose = model_pose.pose;
        publish.palette = palettes[actor].data();
        publish.palette_count = 1;
        publish.frame_revision = (actor + 1) * 10;
        publish.root_delta.rotation.w = 1.0f;
        if (commits.publishAnimationFrame(publish) != Status::ok) ok = false;
    };
    std::thread first{evaluate, 0};
    std::thread second{evaluate, 1};
    while (ready.load() != 2) std::this_thread::yield();
    go = true;
    first.join();
    second.join();
    REQUIRE(ok.load());
    REQUIRE(arena_addresses[0] != arena_addresses[1]);
    REQUIRE(palettes[0][0].column_major[12] == Catch::Approx(0.0f));
    REQUIRE(palettes[1][0].column_major[12] == Catch::Approx(2.0f));
    REQUIRE(std::memcmp(palettes[0].data(), palettes[1].data(), sizeof(Matrix4fV1)) != 0);
    REQUIRE(commits.currentRevision(instances[0]) == 10);
    REQUIRE(commits.currentRevision(instances[1]) == 20);
}

TEST_CASE("WP99 fixture 7 clamp endpoint annotations are never re-emitted", "[animation][a1.5]") {
    // 仕様引用: animation_abi_v1.md §4.2
    // 「clamp clip は範囲外を端へ clamp し、端到達後に同じ annotation を再 emit しない。」
    ProbeRuntime runtime;
    const std::array annotations{
        ProbeAnnotation{.time_seconds = 0.25, .source = 0, .ordinal = 0, .identity = 1},
        ProbeAnnotation{.time_seconds = 0.75, .source = 0, .ordinal = 1, .identity = 2},
    };
    const auto cursor = runtime.createCursor(1.0, WrapMode::clamp, annotations);
    AdvanceDescV1 advance{};
    advance.struct_size = sizeof(advance);
    advance.version = descriptorVersionV1;
    advance.cursor = cursor;
    advance.delta_seconds = 10.0;
    std::array<CrossingV1, 2> crossings{};
    IntervalResultV1 result{};
    result.struct_size = sizeof(result);
    result.version = descriptorVersionV1;
    result.crossings = crossings.data();
    result.crossing_capacity = static_cast<std::uint32_t>(crossings.size());
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.crossing_count == 2);
    REQUIRE(result.root_delta.translation.x == Catch::Approx(1.0f));
    advance.delta_seconds = 10.0;
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.crossing_count == 0);
    REQUIRE(result.root_delta.translation.x == 0.0f);
    advance.delta_seconds = -10.0;
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.crossing_count == 2);
    REQUIRE(result.root_delta.translation.x == Catch::Approx(-1.0f));
    REQUIRE(runtime.advanceCursor(advance, result) == Status::ok);
    REQUIRE(result.crossing_count == 0);
}

TEST_CASE("WP99 fixture 8 crossing capacity exact one-short and zero preserve cursor atomicity",
          "[animation][a1.5]") {
    // 仕様引用: animation_abi_v1.md §4.1
    // 「required count が capacity を超えた場合は BUFFER_TOO_SMALL」「配列は書かず、cursor を一切進めない。」
    const std::array annotations{
        ProbeAnnotation{.time_seconds = 0.2, .source = 0, .ordinal = 0, .identity = 1},
        ProbeAnnotation{.time_seconds = 0.4, .source = 0, .ordinal = 1, .identity = 2},
        ProbeAnnotation{.time_seconds = 0.6, .source = 0, .ordinal = 2, .identity = 3},
    };
    ProbeRuntime runtime;
    const auto cursor = runtime.createCursor(1.0, WrapMode::repeat, annotations);
    AdvanceDescV1 advance{};
    advance.struct_size = sizeof(advance);
    advance.version = descriptorVersionV1;
    advance.cursor = cursor;
    advance.delta_seconds = 0.7;

    IntervalResultV1 zero{};
    zero.struct_size = sizeof(zero);
    zero.version = descriptorVersionV1;
    zero.normalized_phase = 0.9f;
    zero.root_delta.translation.x = 9.0f;
    REQUIRE(runtime.advanceCursor(advance, zero) == Status::buffer_too_small);
    REQUIRE(zero.crossing_count == 3);
    REQUIRE(zero.normalized_phase == 0.9f);
    REQUIRE(zero.root_delta.translation.x == 9.0f);
    REQUIRE(runtime.cursorTime(cursor) == 0.0);

    std::array<CrossingV1, 2> short_buffer{};
    short_buffer[0].element_size = 0xdead;
    IntervalResultV1 one_short{};
    one_short.struct_size = sizeof(one_short);
    one_short.version = descriptorVersionV1;
    one_short.crossings = short_buffer.data();
    one_short.crossing_capacity = static_cast<std::uint32_t>(short_buffer.size());
    REQUIRE(runtime.advanceCursor(advance, one_short) == Status::buffer_too_small);
    REQUIRE(one_short.crossing_count == 3);
    REQUIRE(short_buffer[0].element_size == 0xdead);
    REQUIRE(runtime.cursorTime(cursor) == 0.0);

    std::array<CrossingV1, 3> exact{};
    IntervalResultV1 fill{};
    fill.struct_size = sizeof(fill);
    fill.version = descriptorVersionV1;
    fill.crossings = exact.data();
    fill.crossing_capacity = static_cast<std::uint32_t>(exact.size());
    REQUIRE(runtime.advanceCursor(advance, fill) == Status::ok);
    REQUIRE(fill.crossing_count == 3);
    REQUIRE(runtime.cursorTime(cursor) == Catch::Approx(0.7));
    REQUIRE(exact[0].annotation_identity == 1);
    REQUIRE(exact[2].annotation_identity == 3);
}

} // namespace Pelican::Animation
