#include "../src/core/animation/animationservice.hpp"
#include "../src/core/animation/vrmapplication.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/userpublic/animation/pose_staging_v1.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"
#include "../src/core/vkcore/core.hpp"
#include "fixtures/animation_abi/vrm_application_fixture_protocol.hpp"
#include "morph_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <glm/gtc/quaternion.hpp>
#include <string_view>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace Pelican::Vrm {
namespace {

using Animation::Status;

template <class T> T animationDescriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = Animation::descriptorVersionV1;
    return value;
}

struct BasePosePublisher {
    Animation::AnimationServiceV1 service{};
    Animation::AnimationOwnerHandle owner{};
    Animation::AnimationSourceHandle source{};
    Animation::RigHandle rig{};
    Animation::PoseLayoutHandle layout{};
    Animation::SkinBindingHandle skin_binding{};
    std::uint32_t joint_count = 0;
    std::uint32_t palette_count = 0;
};

Status publishBasePose(
    void *user_context,
    const Animation::AnimationPhaseContextV1 *phase) {
    if (!user_context || !phase ||
        phase->phase != Animation::Phase::base_pose_and_root_modifier)
        return Status::invalid_argument;
    auto &publisher = *static_cast<BasePosePublisher *>(user_context);
    auto frame = animationDescriptor<Animation::BeginPoseArenaFrameDescV1>();
    frame.owner = publisher.owner;
    frame.frame_revision = phase->frame_revision;
    if (const auto status = publisher.service.begin_pose_frame(
            publisher.service.context, &frame);
        status != Status::ok)
        return status;

    auto acquire = [&](Animation::PoseViewV1 &pose) {
        pose = animationDescriptor<Animation::PoseViewV1>();
        auto request = animationDescriptor<Animation::AcquirePoseDescV1>();
        request.arena = frame.arena;
        request.layout = publisher.layout;
        request.joint_count = publisher.joint_count;
        request.out_view = &pose;
        return publisher.service.acquire_pose(publisher.service.context,
                                              &request);
    };
    Animation::PoseViewV1 local{};
    Animation::PoseViewV1 model{};
    if (const auto status = acquire(local); status != Status::ok) return status;
    if (const auto status = acquire(model); status != Status::ok) return status;
    for (std::uint32_t index = 0; index < publisher.joint_count; ++index) {
        local.translations[index] = {0.0f, 0.0f, 0.0f, 0.0f};
        local.rotations[index] = {0.0f, 0.0f, 0.0f, 1.0f};
        local.scales[index] = {1.0f, 1.0f, 1.0f, 0.0f};
    }

    std::vector<Animation::Matrix4fV1> model_matrices(
        publisher.joint_count);
    auto local_to_model =
        animationDescriptor<Animation::LocalToModelDescV1>();
    local_to_model.rig = publisher.rig;
    local_to_model.local_pose = local.pose;
    local_to_model.model_pose = model.pose;
    local_to_model.model_matrices = model_matrices.data();
    local_to_model.model_matrix_capacity = publisher.joint_count;
    if (const auto status = publisher.service.local_to_model(
            publisher.service.context, &local_to_model);
        status != Status::ok)
        return status;

    std::vector<Animation::Matrix4fV1> palette(publisher.palette_count);
    auto build = animationDescriptor<Animation::BuildSkinPaletteDescV1>();
    build.skin_binding = publisher.skin_binding;
    build.model_matrices = model_matrices.data();
    build.model_matrix_count = local_to_model.model_matrix_count;
    build.palette = palette.data();
    build.palette_capacity = publisher.palette_count;
    if (const auto status = publisher.service.build_skin_palette(
            publisher.service.context, &build);
        status != Status::ok)
        return status;

    auto publish =
        animationDescriptor<Animation::PublishAnimationFrameFromSourceDescV1>();
    publish.source = publisher.source;
    publish.frame =
        animationDescriptor<Animation::PublishAnimationFrameDescV1>();
    publish.frame.instance = phase->instance;
    publish.frame.local_pose = local.pose;
    publish.frame.model_pose = model.pose;
    publish.frame.palette = palette.data();
    publish.frame.palette_count = build.palette_count;
    publish.frame.frame_revision = phase->frame_revision;
    publish.frame.root_delta.rotation.w = 1.0f;
    return publisher.service.publish_animation_frame_from_source(
        publisher.service.context, &publish);
}

struct StagedPoseObserver {
    Animation::PoseStagingServiceV1 service{};
    Animation::PoseStageHandle stage{};
    Animation::QuatfV1 left_eye{};
    std::uint32_t callback_count = 0;
};

Status observeStagedPose(
    void *user_context,
    const Animation::AnimationPhaseContextV1 *phase) {
    if (!user_context || !phase ||
        phase->phase != Animation::Phase::world_post_process)
        return Status::invalid_argument;
    auto &observer = *static_cast<StagedPoseObserver *>(user_context);
    Animation::AcquireStagedPoseDescV1 acquire;
    acquire.instance = phase->instance;
    acquire.frame_revision = phase->frame_revision;
    if (const auto status = observer.service.acquire_staged_pose(
            observer.service.context, &acquire);
        status != Status::ok)
        return status;
    Animation::ResolveStagedSourceNodeDescV1 resolve;
    resolve.stage = acquire.stage;
    resolve.source_node_index = 2;
    if (const auto status = observer.service.resolve_source_node(
            observer.service.context, &resolve);
        status != Status::ok)
        return status;
    if (resolve.layout_node_index >= acquire.local_pose.joint_count)
        return Status::invalid_handle;
    observer.stage = acquire.stage;
    observer.left_eye = acquire.local_pose.rotations[resolve.layout_node_index];
    ++observer.callback_count;
    return Status::ok;
}

VrmExpression expressionWithMorph(int node, int index, double weight) {
    VrmExpression expression;
    expression.morph_target_binds.push_back({node, index, weight});
    return expression;
}

struct Sandbox {
    std::filesystem::path root;
    ~Sandbox() { std::filesystem::remove_all(root); }
};

Sandbox makeSandbox() {
    auto root = std::filesystem::temp_directory_path() /
                ("pelican_wp123_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    std::filesystem::create_directories(root);
    return {root};
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

#if defined(_WIN32)
struct FixtureDll {
    HMODULE module{};
    explicit FixtureDll(const char *path) : module(LoadLibraryA(path)) {
        REQUIRE(module != nullptr);
    }
    ~FixtureDll() {
        if (module) FreeLibrary(module);
    }
    template <class T> T symbol(const char *name) const {
        const auto address = GetProcAddress(module, name);
        REQUIRE(address != nullptr);
        return reinterpret_cast<T>(address);
    }
};
#endif

} // namespace

TEST_CASE("VRM expression resolution applies override, binary, lookAt and Base plus deltas",
          "[wp123][vrm][expression][unit]") {
    VrmSemanticData semantic;
    auto happy = expressionWithMorph(5, 0, 1.0);
    happy.material_color_binds = {
        {.material = 0, .type = "color", .target_value = {1.0, 0.2, 0.3, 1.0}},
        {.material = 0, .type = "emissionColor", .target_value = {0.5, 0.4, 0.3, 1.0}},
        {.material = 0, .type = "shadeColor", .target_value = {1.0, 1.0, 1.0, 1.0}},
        {.material = 0, .type = "matcapColor", .target_value = {1.0, 1.0, 1.0, 1.0}},
        {.material = 0, .type = "rimColor", .target_value = {1.0, 1.0, 1.0, 1.0}},
        {.material = 0, .type = "outlineColor", .target_value = {1.0, 1.0, 1.0, 1.0}},
    };
    happy.texture_transform_binds = {
        {.material = 0, .scale = {2.0, 0.5}, .offset = {0.3, -0.1}},
    };
    happy.override_blink = "blend";
    semantic.preset_expressions.emplace("happy", happy);
    VrmExpression blink;
    blink.is_binary = true;
    semantic.preset_expressions.emplace("blink", blink);
    semantic.preset_expressions.emplace("lookLeft", VrmExpression{});
    semantic.preset_expressions.emplace("lookRight", VrmExpression{});
    semantic.preset_expressions.emplace("lookUp", VrmExpression{});
    semantic.preset_expressions.emplace("lookDown", VrmExpression{});
    semantic.look_at = VrmLookAtData{
        .type = "expression",
        .horizontal_outer = VrmLookAtRangeMap{45.0, 1.0},
        .vertical_down = VrmLookAtRangeMap{30.0, 0.8},
        .vertical_up = VrmLookAtRangeMap{0.0, 0.6},
    };

    MorphTargetLayout layout;
    layout.generation = 8;
    layout.default_weights = {0.9f};
    layout.primitives.push_back({.node_index = 5,
                                 .weight_offset = 0,
                                 .delta_ranges = {{.target_index = 0}}});
    SourceMaterialInitialValueTable initial;
    initial.values.push_back({.source_material_index = 0,
                              .base_color_factor = {0.2f, 0.4f, 0.6f, 1.0f},
                              .emissive_factor = {0.1f, 0.2f, 0.1f, 1.0f},
                              .uv_offset = {0.1f, 0.1f},
                              .uv_scale = {1.0f, 1.0f},
                              .uv_rotation = 0.25f});
    ExpressionInputSnapshot snapshot;
    snapshot.input_revision = 3;
    snapshot.frame_revision = 4;
    snapshot.look_at_enabled = true;
    snapshot.look_at_yaw_degrees = 22.5f;
    snapshot.look_at_pitch_degrees = -1.0f;
    snapshot.expression_weights = {{"happy", 0.5f}, {"blink", 1.0f}};
    REQUIRE(evaluateExpressionLookAt(semantic, snapshot) == Status::ok);
    REQUIRE(snapshot.expression_weights.at("lookLeft") ==
            Catch::Approx(0.5f));
    REQUIRE(snapshot.expression_weights.at("lookRight") == 0.0f);
    REQUIRE(snapshot.expression_weights.at("lookUp") ==
            Catch::Approx(0.6f));

    ResolvedExpressionFrame resolved;
    REQUIRE(resolveExpressionFrame(semantic, &layout, &initial, snapshot,
                                   resolved) == Status::ok);
    REQUIRE(resolved.expression_weights.at("blink") == 0.0f);
    REQUIRE(resolved.morph_weights == std::vector<float>{0.5f});
    REQUIRE(resolved.material_overrides.size() == 1);
    const auto &material = resolved.material_overrides.front();
    REQUIRE(material.base_color_factor.x == Catch::Approx(0.6f));
    REQUIRE(material.base_color_factor.y == Catch::Approx(0.3f));
    REQUIRE(material.emissive_factor.x == Catch::Approx(0.3f));
    REQUIRE(material.uv_offset.x == Catch::Approx(0.2f));
    REQUIRE(material.uv_scale.x == Catch::Approx(1.5f));
    REQUIRE(material.uv_rotation == Catch::Approx(0.25f));
    REQUIRE(resolved.diagnostics.size() == 4);
    REQUIRE(resolved.diagnostics[0].material_color_type == "shadeColor");
    REQUIRE(resolved.diagnostics[1].material_color_type == "matcapColor");
    REQUIRE(resolved.diagnostics[2].material_color_type == "rimColor");
    REQUIRE(resolved.diagnostics[3].material_color_type == "outlineColor");
}

TEST_CASE("VRM bone lookAt maps inner outer up and down to absolute eye rotations",
          "[wp134][vrm][lookat][bone][unit]") {
    VrmSemanticData semantic;
    semantic.human_bones = {
        {.name = "leftEye", .node = 2},
        {.name = "rightEye", .node = 3},
    };
    semantic.look_at = VrmLookAtData{
        .type = "bone",
        .horizontal_inner = VrmLookAtRangeMap{20.0, 4.0},
        .horizontal_outer = VrmLookAtRangeMap{40.0, 12.0},
        .vertical_down = VrmLookAtRangeMap{30.0, 6.0},
        .vertical_up = VrmLookAtRangeMap{0.0, 8.0},
    };
    ExpressionInputSnapshot snapshot;
    snapshot.look_at_enabled = true;
    snapshot.look_at_yaw_degrees = 20.0f;
    snapshot.look_at_pitch_degrees = 15.0f;

    BoneLookAtRotations rotations;
    REQUIRE(evaluateBoneLookAt(semantic, snapshot, rotations) == Status::ok);
    REQUIRE(rotations.active);
    REQUIRE(rotations.has_left_eye);
    REQUIRE(rotations.has_right_eye);
    const auto expected = [](float yaw, float pitch) {
        return glm::normalize(
            glm::angleAxis(glm::radians(yaw), glm::vec3{0.0f, 1.0f, 0.0f}) *
            glm::angleAxis(glm::radians(pitch), glm::vec3{1.0f, 0.0f, 0.0f}));
    };
    const auto require_quat = [](const glm::quat &actual,
                                 const glm::quat &wanted) {
        REQUIRE(std::abs(glm::dot(actual, wanted)) == Catch::Approx(1.0f));
    };
    require_quat(rotations.left_eye, expected(6.0f, 3.0f));
    require_quat(rotations.right_eye, expected(4.0f, 3.0f));

    snapshot.look_at_yaw_degrees = -10.0f;
    snapshot.look_at_pitch_degrees = -1.0f;
    REQUIRE(evaluateBoneLookAt(semantic, snapshot, rotations) == Status::ok);
    require_quat(rotations.left_eye, expected(-2.0f, -8.0f));
    require_quat(rotations.right_eye, expected(-3.0f, -8.0f));
}

TEST_CASE("bone lookAt stays staged through phase 300 and commits one final palette",
          "[wp134][vrm][lookat][staging][phase][gpu]") {
    setupLogger();
    Animation::animationServiceRuntime().reset();
    applicationServiceRuntime().reset();
    FastModuleContainer modules;
    requireVulkan();

    ModelTemplate model;
    model.skeletal = std::make_shared<SkeletalModelData>();
    model.skeletal->nodes = {
        {.parent = -1, .name = "Root"},
        {.parent = 0, .name = "Head"},
        {.parent = 1, .name = "LeftEye"},
        {.parent = 1, .name = "RightEye"},
    };
    model.skeletal->joint_nodes = {0, 1, 2, 3};
    model.skeletal->inverse_bind_matrices.assign(4, glm::mat4{1.0f});
    model.skeletal->skin_bindings = {{"Body", 0, 4}};
    auto semantic = std::make_shared<VrmSemanticData>();
    semantic->human_bones = {
        {.name = "head", .node = 1},
        {.name = "leftEye", .node = 2},
        {.name = "rightEye", .node = 3},
    };
    semantic->look_at = VrmLookAtData{
        .type = "bone",
        .horizontal_inner = VrmLookAtRangeMap{20.0, 10.0},
        .horizontal_outer = VrmLookAtRangeMap{20.0, 10.0},
        .vertical_down = VrmLookAtRangeMap{20.0, 10.0},
        .vertical_up = VrmLookAtRangeMap{20.0, 10.0},
    };
    model.vrm_semantic = semantic;

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto polygon = instances.placeModelInstance(model);
    auto &animation_runtime = Animation::animationServiceRuntime();
    animation_runtime.registerObject("Hero", *model.skeletal, polygon);

    auto api = animationDescriptor<Animation::ApiV1>();
    REQUIRE(Animation::getApiV1(Animation::abiVersionV1, &api) == Status::ok);
    auto service = animationDescriptor<Animation::AnimationServiceV1>();
    REQUIRE(api.get_animation_service(api.context,
                                      Animation::animationServiceVersionV1,
                                      &service) == Status::ok);

    internal::ScopedRegistrationOwner owner_scope{90134};
    auto owner =
        animationDescriptor<Animation::CurrentAnimationOwnerDescV1>();
    REQUIRE(service.get_current_owner(service.context, &owner) == Status::ok);
    auto sink = animationDescriptor<Animation::ResolveAnimationSinkDescV1>();
    constexpr std::string_view object_name = "Hero";
    sink.object_name = object_name.data();
    sink.object_name_size = static_cast<std::uint32_t>(object_name.size());
    sink.sink_kind = Animation::AnimationSinkKind::skeletal_pose;
    REQUIRE(service.resolve_sink(service.context, &sink) == Status::ok);
    auto instance =
        animationDescriptor<Animation::ResolveAnimationInstanceDescV1>();
    instance.sink = sink.sink;
    REQUIRE(service.resolve_instance(service.context, &instance) == Status::ok);
    REQUIRE(instance.instance.identity == instances.animationInstance(polygon).identity);
    auto rig = animationDescriptor<Animation::ResolveAnimationRigDescV1>();
    rig.instance = instance.instance;
    REQUIRE(service.resolve_rig(service.context, &rig) == Status::ok);
    auto layout = animationDescriptor<Animation::ResolvePoseLayoutDescV1>();
    layout.rig = rig.rig;
    REQUIRE(service.resolve_layout(service.context, &layout) == Status::ok);
    REQUIRE(layout.joint_count == 4);
    REQUIRE(layout.palette_count == 4);

    auto claim = animationDescriptor<Animation::ClaimAnimationSourceDescV1>();
    claim.owner = owner.owner;
    claim.sink = sink.sink;
    claim.source_ordinal = 7;
    REQUIRE(service.claim_source(service.context, &claim) == Status::ok);

    BasePosePublisher publisher{
        .service = service,
        .owner = owner.owner,
        .source = claim.source,
        .rig = rig.rig,
        .layout = layout.layout,
        .skin_binding = layout.skin_binding,
        .joint_count = layout.joint_count,
        .palette_count = layout.palette_count,
    };
    auto register_phase = [&](Animation::Phase phase, std::int32_t priority,
                              std::uint64_t identity,
                              Animation::AnimationPhaseCallbackV1 callback,
                              void *context) {
        auto request =
            animationDescriptor<Animation::RegisterAnimationPhaseDescV1>();
        request.owner = owner.owner;
        request.registration =
            animationDescriptor<Animation::PhaseRegistrationV1>();
        request.registration.phase = phase;
        request.registration.priority = priority;
        request.registration.registration_identity = identity;
        request.registration.registration_generation = owner.owner.generation;
        request.registration.source_ordinal = 7;
        request.callback = callback;
        request.user_context = context;
        return service.register_phase(service.context, &request);
    };
    REQUIRE(register_phase(Animation::Phase::base_pose_and_root_modifier, 0,
                           0x5750313334424153ull, publishBasePose,
                           &publisher) == Status::ok);

    Animation::PoseStagingServiceV1 unsupported;
    REQUIRE(Animation::getPoseStagingServiceV1(2, &unsupported) ==
            Status::unsupported_version);
    StagedPoseObserver observer;
    REQUIRE(Animation::getPoseStagingServiceV1(
                Animation::poseStagingServiceVersionV1, &observer.service) ==
            Status::ok);
    REQUIRE((observer.service.capability_bits &
             Animation::poseStagingServiceCapabilitiesV1) ==
            Animation::poseStagingServiceCapabilitiesV1);
    REQUIRE(register_phase(Animation::Phase::world_post_process, 300,
                           0x57503133344f4253ull, observeStagedPose,
                           &observer) == Status::ok);

    ApplicationServiceV1 application;
    REQUIRE(getApplicationServiceV1(applicationServiceVersionV1, &application) ==
            Status::ok);
    SetExpressionInputDescV1 input;
    input.instance = instance.instance;
    input.input_revision = 1;
    input.look_at_yaw_degrees = 10.0f;
    input.look_at_pitch_degrees = 0.0f;
    input.flags = expression_input_look_at;
    REQUIRE(application.set_expression_inputs(application.context, &input) ==
            Status::ok);

    REQUIRE(animation_runtime.runPhases(sink.sink, 55) == Status::ok);
    REQUIRE(observer.callback_count == 1);
    REQUIRE(Animation::isValid(observer.stage));
    const glm::quat observed{observer.left_eye.w, observer.left_eye.x,
                             observer.left_eye.y, observer.left_eye.z};
    const auto expected = glm::angleAxis(glm::radians(5.0f),
                                         glm::vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(std::abs(glm::dot(observed, expected)) == Catch::Approx(1.0f));
    REQUIRE(instances.currentAnimationRevisionForTesting(polygon) == 55);
    const auto &palette = instances.currentSkinPaletteForTesting(polygon);
    REQUIRE(palette.size() == 4);
    REQUIRE(std::abs(glm::dot(glm::quat_cast(palette[2]), expected)) ==
            Catch::Approx(1.0f));

    GET_MODULE(VulkanManageCore).waitIdle();
    applicationServiceRuntime().reset();
    animation_runtime.reset();
}

#if defined(_WIN32)
TEST_CASE("third-party DLL publishes a VRM 1.0 expression frame through the additive public service",
          "[wp123][vrm][expression][dll][gpu]") {
    setupLogger();
    Animation::animationServiceRuntime().reset();
    applicationServiceRuntime().reset();
    auto sandbox = makeSandbox();
    const auto path = sandbox.root / "expression.vrm";
    TestMorphFixture::writeGlb(path, {.skinned = true,
                                     .vrm_expression = true});

    FastModuleContainer modules;
    requireVulkan();
    const auto model = GET_MODULE(GltfLoader).loadGltfBinary(path.string());
    REQUIRE(model.vrm_semantic);
    REQUIRE(model.morph_targets);
    REQUIRE(model.material_initial_values);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto polygon = instances.placeModelInstance(model);
    const auto instance = instances.animationInstance(polygon);

    ApplicationServiceV1 service{};
    service.struct_size = sizeof(service);
    service.version = applicationDescriptorVersionV1;
    REQUIRE(getApplicationServiceV1(applicationServiceVersionV1, &service) ==
            Status::ok);
    REQUIRE((service.capability_bits & applicationServiceCapabilitiesV1) ==
            applicationServiceCapabilitiesV1);

    using EvaluatorFn = std::uint32_t (*)(const ApplicationServiceV1 *,
                                           Animation::InstanceHandle,
                                           VrmApplicationFixtureResult *);
    FixtureDll evaluator{PELICAN_VRM_APPLICATION_EVALUATOR_DLL};
    VrmApplicationFixtureResult result{};
    REQUIRE(evaluator.symbol<EvaluatorFn>(
                "pelican_vrm_application_public_evaluator")(
                &service, instance, &result) ==
            static_cast<std::uint32_t>(Status::ok));
    REQUIRE(result.status == static_cast<std::uint32_t>(Status::ok));
    REQUIRE(result.happy_weight == Catch::Approx(0.75f));
    REQUIRE(result.morph_weight_count == 1);
    REQUIRE(result.material_override_count == 1);
    REQUIRE(result.diagnostic_count == 4);
    REQUIRE(result.diagnostic_code == static_cast<std::uint32_t>(
                                          ApplicationDiagnosticCodeV1::
                                              unsupported_material_color_type));
    REQUIRE(std::string{result.expression_name} == "happy");
    REQUIRE(std::string{result.material_color_type} == "shadeColor");

    const auto &morph = instances.morphWeightFrameForTesting(polygon);
    REQUIRE(morph.current_revision == 77);
    REQUIRE(morph.current == std::vector<float>{0.75f});
    const auto *material =
        instances.materialAbsoluteOverrideFrameForTesting(polygon, 0);
    REQUIRE(material != nullptr);
    REQUIRE(material->current_revision == 77);
    REQUIRE(material->current.base_color_factor.x == Catch::Approx(0.775f));
    REQUIRE(material->current.emissive_factor.x == Catch::Approx(0.305f));
    REQUIRE(material->current.uv_offset.x == Catch::Approx(0.15f));
    REQUIRE(material->current.uv_scale.x == Catch::Approx(1.375f));
    GET_MODULE(VulkanManageCore).waitIdle();
    applicationServiceRuntime().reset();
    Animation::animationServiceRuntime().reset();
}
#endif

} // namespace Pelican::Vrm
