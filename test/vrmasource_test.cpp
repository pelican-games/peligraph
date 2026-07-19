#include "../src/core/animation/animationservice.hpp"
#include "../src/core/animation/vrmapplication.hpp"
#include "../src/core/animation/vrmaretarget.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/vrmadecoder.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/userpublic/animation/animgraph.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"
#include "../src/core/vkcore/core.hpp"
#include "morph_fixture.hpp"
#include "vrma_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Pelican {
namespace {

using Animation::Status;

constexpr std::array<std::string_view, 15> requiredBones{
    "hips",          "spine",         "head",          "leftUpperLeg",
    "leftLowerLeg",  "leftFoot",      "rightUpperLeg", "rightLowerLeg",
    "rightFoot",     "leftUpperArm",  "leftLowerArm",  "leftHand",
    "rightUpperArm", "rightLowerArm", "rightHand",
};

template <class T> T descriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = Animation::descriptorVersionV1;
    return value;
}

struct Sandbox {
    std::filesystem::path root;
    ~Sandbox() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

Sandbox makeSandbox(std::string_view label) {
    auto root = std::filesystem::temp_directory_path() /
                ("pelican_wp178_" + std::string{label} + "_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    std::filesystem::create_directories(root);
    return {root};
}

std::vector<std::uint8_t> retargetableVrmaBytes(
    TestVrmaFixture::Kind kind) {
    auto builder = TestVrmaFixture::makeDocument(kind);
    builder.document["nodes"][0]["translation"] = {0.0, 1.0, 0.0};
    return TestVrmaFixture::makeGlb(std::move(builder));
}

void writeBytes(const std::filesystem::path &path,
                std::span<const std::uint8_t> bytes) {
    std::ofstream file{path, std::ios::binary};
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

VrmaDecodeOptions provenance(std::string uri) {
    return {
        .source_uri = std::move(uri),
        .import_profile = "vrma-i0/integration@1",
        .tool_version = "pelican-wp178-fixture/1",
    };
}

struct TargetFixture {
    SkeletalModelData model;
    VrmSemanticData semantic;
};

TargetFixture unitTarget() {
    TargetFixture result;
    result.semantic.spec_version = "1.0";
    for (std::size_t index = 0; index < requiredBones.size(); ++index) {
        result.model.nodes.push_back({
            .parent = -1,
            .translation = index == 0 ? glm::vec3{0.0f, 1.0f, 0.0f}
                                      : glm::vec3{0.0f},
            .name = std::string{requiredBones[index]} + "Target",
        });
        result.model.joint_nodes.push_back(static_cast<int>(index));
        result.model.inverse_bind_matrices.push_back(glm::mat4{1.0f});
        result.semantic.human_bones.push_back({
            .name = std::string{requiredBones[index]},
            .node = static_cast<int>(index),
            .node_name = result.model.nodes.back().name,
            .recognized = true,
            .required = true,
        });
    }
    result.model.skin_bindings = {
        {"Body", 0, static_cast<std::uint32_t>(requiredBones.size())}};
    result.model.clips.push_back({
        .name = "Idle",
        .start = 0.0f,
        .end = 1.0f,
    });
    result.semantic.preset_expressions.emplace("happy", VrmExpression{});
    return result;
}

std::shared_ptr<const VrmaRetargetedClip> retargetUnit(
    const TargetFixture &target, TestVrmaFixture::Kind kind) {
    const auto bytes = retargetableVrmaBytes(kind);
    const auto decoded = decodeVrmaAnimation(
        bytes, "unit.vrma", provenance("asset://wp178/unit.vrma"));
    return retargetVrmaClip(*decoded, target.model, target.semantic);
}

Animation::AnimationServiceV1 animationService() {
    auto api = descriptor<Animation::ApiV1>();
    REQUIRE(Animation::getApiV1(Animation::abiVersionV1, &api) ==
            Status::ok);
    auto service = descriptor<Animation::AnimationServiceV1>();
    REQUIRE(api.get_animation_service(
                api.context, Animation::animationServiceVersionV1, &service) ==
            Status::ok);
    return service;
}

Animation::AnimationSinkHandle resolveSink(
    const Animation::AnimationServiceV1 &service, std::string_view object) {
    auto request = descriptor<Animation::ResolveAnimationSinkDescV1>();
    request.object_name = object.data();
    request.object_name_size = static_cast<std::uint32_t>(object.size());
    request.sink_kind = Animation::AnimationSinkKind::skeletal_pose;
    REQUIRE(service.resolve_sink(service.context, &request) == Status::ok);
    return request.sink;
}

Animation::ClipHandle resolveClip(
    const Animation::AnimationServiceV1 &service,
    Animation::AnimationSinkHandle sink, std::string_view name) {
    auto instance = descriptor<Animation::ResolveAnimationInstanceDescV1>();
    instance.sink = sink;
    REQUIRE(service.resolve_instance(service.context, &instance) == Status::ok);
    auto rig = descriptor<Animation::ResolveAnimationRigDescV1>();
    rig.instance = instance.instance;
    REQUIRE(service.resolve_rig(service.context, &rig) == Status::ok);
    auto clip = descriptor<Animation::ResolveAnimationClipDescV1>();
    clip.rig = rig.rig;
    clip.clip_name = name.data();
    clip.clip_name_size = static_cast<std::uint32_t>(name.size());
    REQUIRE(service.resolve_clip(service.context, &clip) == Status::ok);
    return clip.clip;
}

void requireVulkan() {
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    try {
        (void)GET_MODULE(StandardMaterialResource);
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan unavailable: "} + error.what());
    }
}

constexpr auto blendGraph = R"json({
  "schema":"pelican.anim_graph","version":1,
  "parameters":{"mix":0.5},"initial_state":"Blend",
  "states":[{
    "name":"Blend","type":"blend1d","parameter":"mix",
    "clips":[
      {"clip":"Idle","threshold":0},
      {"clip":"Mocap","threshold":1}
    ]
  }]
})json";

constexpr auto motionGraph = R"json({
  "schema":"pelican.anim_graph","version":1,"initial_state":"Motion",
  "states":[{"name":"Motion","type":"clip","clip":"Mocap"}]
})json";

} // namespace

TEST_CASE("VRMA-I0 graph blends a generated .vrma twice and commits typed sinks at one revision",
          "[wp178][vrma][source][graph][determinism][gpu]") {
    setupLogger();
    auto &runtime = Animation::animationServiceRuntime();
    runtime.reset();
    Vrm::applicationServiceRuntime().reset();
    auto sandbox = makeSandbox("blend");
    const auto target_path = sandbox.root / "target.vrm";
    const auto source_path = sandbox.root / "motion.vrma";
    TestMorphFixture::writeGlb(
        target_path,
        {.skinned = true, .vrm_expression = true, .target_count = 2});
    writeBytes(source_path,
               retargetableVrmaBytes(TestVrmaFixture::Kind::full));

    FastModuleContainer modules;
    requireVulkan();
    auto model = GET_MODULE(GltfLoader).loadGltfBinary(target_path.string());
    REQUIRE(model.skeletal);
    REQUIRE(model.vrm_semantic);
    REQUIRE(model.morph_targets);
    // Both source and target use a non-zero hips rest height, making the R0
    // hips translation scale defined.
    const auto hips = std::find_if(
        model.vrm_semantic->human_bones.begin(),
        model.vrm_semantic->human_bones.end(),
        [](const auto &bone) { return bone.name == "hips"; });
    REQUIRE(hips != model.vrm_semantic->human_bones.end());
    REQUIRE(hips->node >= 0);
    model.skeletal->nodes.at(static_cast<std::size_t>(hips->node)).translation =
        {0.0f, 1.0f, 0.0f};
    model.skeletal->clips.push_back({
        .name = "Idle",
        .start = 0.0f,
        .end = 1.0f,
    });
    auto semantic = std::make_shared<VrmSemanticData>(*model.vrm_semantic);
    semantic->preset_expressions["lookLeft"]
        .morph_target_binds.push_back({0, 1, 1.0});
    model.vrm_semantic = semantic;

    const auto decoded = loadVrmaAnimation(
        source_path,
        provenance("asset://wp178/project/motion.vrma"));
    const auto retargeted = retargetVrmaClip(
        *decoded, *model.skeletal, *model.vrm_semantic);
    REQUIRE(retargeted->expression_channels.size() == 1);
    REQUIRE(retargeted->gaze_channel.has_value());

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto hero_polygon = instances.placeModelInstance(model);
    const auto control_polygon = instances.placeModelInstance(model);
    runtime.registerObject("Hero", *model.skeletal, hero_polygon);
    runtime.registerObject("Control", *model.skeletal, control_polygon);
    REQUIRE(runtime.registerVrmaSource("Hero", "Mocap", retargeted) ==
            Status::ok);
    REQUIRE(runtime.registerVrmaSource("Control", "Mocap", retargeted) ==
            Status::ok);

    const auto service = animationService();
    const auto hero_sink = resolveSink(service, "Hero");
    const auto control_sink = resolveSink(service, "Control");
    AnimationGraph::EvaluatorV1 hero{
        AnimationGraph::parseDocumentV1(blendGraph), "Hero", 178};
    AnimationGraph::EvaluatorV1 control{
        AnimationGraph::parseDocumentV1(blendGraph), "Control", 179};
    {
        internal::ScopedRegistrationOwner owner{1780};
        REQUIRE(hero.bind() == Status::ok);
    }
    {
        internal::ScopedRegistrationOwner owner{1790};
        REQUIRE(control.bind() == Status::ok);
    }

    constexpr std::uint64_t revision = 17801;
    REQUIRE(hero.prepareTick(0.5, 0.5, revision) == Status::ok);
    REQUIRE(control.prepareTick(0.5, 0.5, revision) == Status::ok);
    REQUIRE(runtime.runPhases(hero_sink, revision) == Status::ok);
    REQUIRE(runtime.runPhases(control_sink, revision) == Status::ok);
    const bool deterministic_pose =
        hero.lastPoseBytes() == control.lastPoseBytes();
    REQUIRE(deterministic_pose);
    REQUIRE_FALSE(hero.lastPoseBytes().empty());

    const auto hero_trace = hero.getStatus();
    const auto mocap = std::find_if(
        hero_trace.cursors.begin(), hero_trace.cursors.end(),
        [](const auto &cursor) { return cursor.clip == "Mocap"; });
    REQUIRE(mocap != hero_trace.cursors.end());
    REQUIRE(mocap->asset_generation == 1);
    REQUIRE(mocap->profile_version == 1);
    REQUIRE(mocap->source_rig_sha256.size() == 64);
    REQUIRE(mocap->target_rig_sha256.size() == 64);

    const auto &hero_morph =
        instances.morphWeightFrameForTesting(hero_polygon);
    const auto &control_morph =
        instances.morphWeightFrameForTesting(control_polygon);
    REQUIRE(hero_morph.current_revision == revision);
    REQUIRE(control_morph.current_revision == revision);
    REQUIRE(instances.currentAnimationRevisionForTesting(hero_polygon) ==
            revision);
    REQUIRE(hero_morph.current.size() == 2);
    // happy: source 0.5 at t=0.5, then graph weight 0.5.
    REQUIRE(hero_morph.current[0] == Catch::Approx(0.25f).margin(1.0e-5f));
    // gaze: source yaw 15 degrees, graph blends from identity to 7.5;
    // VRM target range maps 7.5 / 45 to lookLeft morph target 1.
    REQUIRE(hero_morph.current[1] ==
            Catch::Approx(1.0f / 6.0f).margin(1.0e-4f));
    REQUIRE(hero_morph.current == control_morph.current);

    GET_MODULE(VulkanManageCore).waitIdle();
    Vrm::applicationServiceRuntime().reset();
    runtime.reset();
}

TEST_CASE("VRMA-I0 reload invalidates cursor and pose snapshot then rebinds a new generation",
          "[wp178][vrma][source][reload][generation]") {
    auto target = unitTarget();
    auto first = retargetUnit(target, TestVrmaFixture::Kind::body_only);
    auto &runtime = Animation::animationServiceRuntime();
    runtime.reset();
    runtime.registerObject("Hero", target.model);
    REQUIRE(runtime.registerVrmaSource("Hero", "Mocap", first) == Status::ok);
    const auto service = animationService();
    const auto sink = resolveSink(service, "Hero");
    const auto first_handle = resolveClip(service, sink, "Mocap");
    auto first_metadata = descriptor<Animation::ClipMetadataV1>();
    first_metadata.clip = first_handle;
    REQUIRE(service.get_clip_metadata(service.context, &first_metadata) ==
            Status::ok);
    REQUIRE(first_metadata.clip_kind ==
            Animation::AnimationClipKindV1::vrma_retargeted_clip);

    internal::ScopedRegistrationOwner owner_scope{1781};
    auto owner = descriptor<Animation::CurrentAnimationOwnerDescV1>();
    REQUIRE(service.get_current_owner(service.context, &owner) == Status::ok);
    auto cursor = descriptor<Animation::CreateClipCursorDescV1>();
    cursor.owner = owner.owner;
    cursor.clip = first_handle;
    REQUIRE(service.create_cursor(service.context, &cursor) == Status::ok);

    AnimationGraph::EvaluatorV1 evaluator{
        AnimationGraph::parseDocumentV1(motionGraph), "Hero", 180};
    REQUIRE(evaluator.bind() == Status::ok);
    REQUIRE(evaluator.prepareTick(0.5, 0.5, 17810) == Status::ok);
    REQUIRE(runtime.runPhases(sink, 17810) == Status::ok);
    const auto before = evaluator.lastPoseBytes();
    REQUIRE_FALSE(before.empty());

    auto changed = std::make_shared<VrmaRetargetedClip>(*first);
    changed->profile.provenance.profile_version = 2;
    changed->profile.provenance.source_rig_sha256 = std::string(64, 'a');
    changed->target_rest_pose.at(2).translation.x = 0.75f;
    REQUIRE(runtime.reloadVrmaSource("Hero", "Mocap", changed) ==
            Status::ok);

    auto stale_metadata = descriptor<Animation::ClipMetadataV1>();
    stale_metadata.clip = first_handle;
    REQUIRE(service.get_clip_metadata(service.context, &stale_metadata) ==
            Status::stale_generation);
    auto api = descriptor<Animation::ApiV1>();
    REQUIRE(Animation::getApiV1(Animation::abiVersionV1, &api) ==
            Status::ok);
    auto advance = descriptor<Animation::AdvanceDescV1>();
    advance.cursor = cursor.cursor;
    advance.delta_seconds = 0.1;
    auto interval = descriptor<Animation::IntervalResultV1>();
    REQUIRE(api.advance_cursor(api.context, &advance, &interval) ==
            Status::stale_generation);
    REQUIRE(evaluator.prepareTick(0.6, 0.1, 17811) == Status::ok);
    REQUIRE(runtime.runPhases(sink, 17811) == Status::stale_generation);

    REQUIRE(evaluator.rebind() == Status::ok);
    REQUIRE(evaluator.getStatus().source_reset_count == 1);
    REQUIRE(evaluator.lastPoseBytes().empty());
    REQUIRE(evaluator.prepareTick(0.0, 0.0, 17812) == Status::ok);
    REQUIRE(runtime.runPhases(sink, 17812) == Status::ok);
    const bool pose_changed = evaluator.lastPoseBytes() != before;
    REQUIRE(pose_changed);
    const auto trace = evaluator.getStatus();
    REQUIRE(trace.source_reset_count == 1);
    REQUIRE(trace.cursors.size() == 1);
    REQUIRE(trace.cursors[0].asset_identity ==
            first_metadata.asset_identity);
    REQUIRE(trace.cursors[0].asset_generation ==
            first_metadata.asset_generation + 1);
    REQUIRE(trace.cursors[0].profile_version == 2);
    REQUIRE(trace.cursors[0].source_rig_sha256 == std::string(64, 'a'));
    runtime.reset();
}

TEST_CASE("VRMA-I0 graph apply is the sole writer while timeline authority is extract only",
          "[wp178][vrma][source][authority]") {
    auto target = unitTarget();
    auto source = retargetUnit(target, TestVrmaFixture::Kind::body_only);
    auto &runtime = Animation::animationServiceRuntime();
    runtime.reset();
    runtime.registerObject("Hero", target.model);
    REQUIRE(runtime.registerVrmaSource("Hero", "Mocap", source) == Status::ok);
    const auto service = animationService();
    const auto sink = resolveSink(service, "Hero");
    const auto clip = resolveClip(service, sink, "Mocap");

    AnimationGraph::EvaluatorV1 graph{
        AnimationGraph::parseDocumentV1(motionGraph), "Hero", 181};
    {
        internal::ScopedRegistrationOwner graph_owner{1782};
        REQUIRE(graph.bind() == Status::ok);
    }

    internal::ScopedRegistrationOwner timeline_owner{1783};
    auto owner = descriptor<Animation::CurrentAnimationOwnerDescV1>();
    REQUIRE(service.get_current_owner(service.context, &owner) == Status::ok);
    auto extract = descriptor<Animation::ClaimAnimationSourcePolicyDescV1>();
    extract.owner = owner.owner;
    extract.sink = sink;
    extract.source_ordinal = 2;
    extract.authority =
        Animation::AnimationSourceAuthorityV1::timeline_extract_only;
    REQUIRE(service.claim_source_policy(service.context, &extract) ==
            Status::ok);

    auto conflicting = descriptor<Animation::ClaimAnimationSourcePolicyDescV1>();
    conflicting.owner = owner.owner;
    conflicting.sink = sink;
    conflicting.source_ordinal = 3;
    conflicting.authority = Animation::AnimationSourceAuthorityV1::graph_apply;
    REQUIRE(service.claim_source_policy(service.context, &conflicting) ==
            Status::authority_conflict);

    auto instance = descriptor<Animation::ResolveAnimationInstanceDescV1>();
    instance.sink = sink;
    REQUIRE(service.resolve_instance(service.context, &instance) == Status::ok);
    auto publish =
        descriptor<Animation::PublishAnimationFrameFromSourceDescV1>();
    publish.source = extract.source;
    publish.frame = descriptor<Animation::PublishAnimationFrameDescV1>();
    publish.frame.instance = instance.instance;
    publish.frame.frame_revision = 17820;
    publish.frame.root_delta.rotation.w = 1.0f;
    REQUIRE(service.publish_animation_frame_from_source(service.context,
                                                        &publish) ==
            Status::authority_conflict);

    auto rig = descriptor<Animation::ResolveAnimationRigDescV1>();
    rig.instance = instance.instance;
    REQUIRE(service.resolve_rig(service.context, &rig) == Status::ok);
    auto layout = descriptor<Animation::ResolvePoseLayoutDescV1>();
    layout.rig = rig.rig;
    REQUIRE(service.resolve_layout(service.context, &layout) == Status::ok);
    auto frame = descriptor<Animation::BeginPoseArenaFrameDescV1>();
    frame.owner = owner.owner;
    frame.frame_revision = 17821;
    REQUIRE(service.begin_pose_frame(service.context, &frame) == Status::ok);
    auto pose_view = descriptor<Animation::PoseViewV1>();
    auto acquire = descriptor<Animation::AcquirePoseDescV1>();
    acquire.arena = frame.arena;
    acquire.layout = layout.layout;
    acquire.joint_count = layout.joint_count;
    acquire.out_view = &pose_view;
    REQUIRE(service.acquire_pose(service.context, &acquire) == Status::ok);
    auto sample = descriptor<Animation::SampleAnimationSourceAtDescV1>();
    sample.clip = clip;
    sample.time_seconds = 0.5;
    sample.output_pose = pose_view.pose;
    sample.gaze = descriptor<Animation::AnimationGazeSampleV1>();
    REQUIRE(service.sample_animation_source_at(service.context, &sample) ==
            Status::ok);
    REQUIRE(sample.expression_count == 0);
    REQUIRE(sample.gaze.present == 0);

    auto release = descriptor<Animation::ReleaseAnimationSourceDescV1>();
    release.owner = owner.owner;
    release.source = extract.source;
    REQUIRE(service.release_source(service.context, &release) == Status::ok);
    REQUIRE(graph.prepareTick(0.5, 0.5, 17822) == Status::ok);
    REQUIRE(runtime.runPhases(sink, 17822) == Status::ok);
    REQUIRE_FALSE(graph.lastPoseBytes().empty());
    REQUIRE(graph.getStatus().authority ==
            Animation::AnimationSourceAuthorityV1::graph_apply);
    runtime.reset();
}

} // namespace Pelican
