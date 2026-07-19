#include "../src/core/animation/vrmaretarget.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <type_traits>

#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {
namespace {

constexpr std::array<std::string_view, 15> requiredBones{
    "hips",          "spine",         "head",          "leftUpperLeg",
    "leftLowerLeg",  "leftFoot",      "rightUpperLeg", "rightLowerLeg",
    "rightFoot",     "leftUpperArm",  "leftLowerArm",  "leftHand",
    "rightUpperArm", "rightLowerArm", "rightHand",
};

std::array<float, 4> xyzw(glm::quat value) {
    value = glm::normalize(value);
    return {value.x, value.y, value.z, value.w};
}

glm::quat xRotation(float degrees) {
    return glm::angleAxis(glm::radians(degrees), glm::vec3{1.0f, 0.0f, 0.0f});
}

glm::quat yRotation(float degrees) {
    return glm::angleAxis(glm::radians(degrees), glm::vec3{0.0f, 1.0f, 0.0f});
}

glm::quat zRotation(float degrees) {
    return glm::angleAxis(glm::radians(degrees), glm::vec3{0.0f, 0.0f, 1.0f});
}

VrmaVectorTrack rotationTrack(glm::quat begin, glm::quat end) {
    return {
        .interpolation = VrmaInterpolation::linear,
        .component_count = 4,
        .times = {0.0f, 1.0f},
        .values = {xyzw(begin), xyzw(end)},
    };
}

VrmaVectorTrack translationTrack(glm::vec3 begin, glm::vec3 end) {
    return {
        .interpolation = VrmaInterpolation::linear,
        .component_count = 3,
        .times = {0.0f, 1.0f},
        .values = {
            {begin.x, begin.y, begin.z, 0.0f},
            {end.x, end.y, end.z, 0.0f},
        },
    };
}

VrmaScalarTrack scalarTrack(float begin, float end) {
    return {
        .interpolation = VrmaInterpolation::linear,
        .times = {0.0f, 1.0f},
        .values = {begin, end},
    };
}

struct SourceFixture {
    VrmaClip clip;
    std::shared_ptr<VrmaSourceRig> rig;
};

SourceFixture sourceFixture(float hips_height = 1.0f) {
    SourceFixture fixture;
    fixture.rig = std::make_shared<VrmaSourceRig>();
    for (std::size_t i = 0; i < requiredBones.size(); ++i) {
        fixture.rig->nodes.push_back({
            .name = std::string{requiredBones[i]} + "Source",
            .translation = i == 0 ? std::array<float, 3>{0.0f, hips_height, 0.0f}
                                  : std::array<float, 3>{0.0f, 0.0f, 0.0f},
        });
        fixture.rig->human_bones.push_back({
            .name = std::string{requiredBones[i]},
            .source_node = static_cast<int>(i),
        });
    }
    fixture.rig->nodes.push_back({.name = "ExpressionWeight"});
    fixture.rig->nodes.push_back({.name = "LookAtDirection"});
    fixture.clip = {
        .spec_version = "1.0",
        .name = "#animation/0",
        .start = 0.0f,
        .end = 1.0f,
        .source_rig = fixture.rig,
        .metadata = {
            .source_uri = "asset://mocap/numeric.vrma",
            .source_fragment = "#animation/0",
            .content_sha256 =
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            .import_profile = "vrma-c0/default@1",
            .tool_version = "pelican-fixture-dcc/2.4.1",
        },
    };
    return fixture;
}

struct TargetFixture {
    SkeletalModelData rig;
    VrmSemanticData semantic;
};

TargetFixture targetFixture(float hips_height = 1.0f) {
    TargetFixture fixture;
    fixture.rig.source_path = "asset://avatars/numeric.vrm";
    fixture.semantic.spec_version = "1.0";
    for (std::size_t i = 0; i < requiredBones.size(); ++i) {
        fixture.rig.nodes.push_back({
            .translation = i == 0 ? glm::vec3{0.0f, hips_height, 0.0f}
                                  : glm::vec3{0.0f},
            .name = std::string{requiredBones[i]} + "Target",
        });
        fixture.semantic.human_bones.push_back({
            .name = std::string{requiredBones[i]},
            .node = static_cast<int>(i),
            .node_name = fixture.rig.nodes.back().name,
            .recognized = true,
            .required = true,
        });
    }
    return fixture;
}

void requireVec3(glm::vec3 actual, glm::vec3 expected,
                 float margin = 1.0e-5f) {
    REQUIRE(actual.x == Catch::Approx(expected.x).margin(margin));
    REQUIRE(actual.y == Catch::Approx(expected.y).margin(margin));
    REQUIRE(actual.z == Catch::Approx(expected.z).margin(margin));
}

void requireQuat(glm::quat actual, glm::quat expected,
                 float margin = 1.0e-5f) {
    REQUIRE(std::abs(glm::dot(glm::normalize(actual), glm::normalize(expected))) ==
            Catch::Approx(1.0f).margin(margin));
}

glm::mat4 localMatrix(const VrmaLocalTransform &transform) {
    return glm::translate(glm::mat4{1.0f}, transform.translation) *
           glm::mat4_cast(transform.rotation) *
           glm::scale(glm::mat4{1.0f}, transform.scale);
}

glm::mat4 worldMatrix(std::size_t node, const SkeletalModelData &rig,
                      const VrmaRetargetSample &sample) {
    auto result = localMatrix(sample.local_transforms[node]);
    int parent = rig.nodes[node].parent;
    while (parent >= 0) {
        result = localMatrix(sample.local_transforms[static_cast<std::size_t>(parent)]) *
                 result;
        parent = rig.nodes[static_cast<std::size_t>(parent)].parent;
    }
    return result;
}

template <class T>
concept HasRootMotionDelta = requires(T value) { value.root_motion_delta; };

static_assert(vrmaRetargetProfileVersion == 1);
static_assert(!HasRootMotionDelta<VrmaRetargetedClip>);
static_assert(std::is_same_v<decltype(retargetVrmaClip(
                                 std::declval<const VrmaClip &>(),
                                 std::declval<const SkeletalModelData &>(),
                                 std::declval<const VrmSemanticData &>())),
                             std::shared_ptr<const VrmaRetargetedClip>>);

} // namespace

TEST_CASE("VRMA-R0 identity rigs preserve body expression and gaze samples",
          "[wp177][vrma][retarget]") {
    auto source = sourceFixture();
    source.clip.body_channels = {
        {
            .human_bone = "hips",
            .source_node = 0,
            .path = VrmaBodyPath::translation,
            .track = translationTrack({0.0f, 1.0f, 0.0f},
                                      {0.25f, 1.5f, -0.5f}),
        },
        {
            .human_bone = "spine",
            .source_node = 1,
            .path = VrmaBodyPath::rotation,
            .track = rotationTrack(glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                   yRotation(45.0f)),
        },
    };
    source.clip.expression_channels = {{
        .expression = "happy",
        .preset = true,
        .source_node = 15,
        .weight = scalarTrack(0.0f, 1.0f),
    }};
    source.clip.gaze_channel = VrmaGazeChannel{
        .source_node = 16,
        .offset_from_head_bone = std::array<float, 3>{0.0f, 0.06f, 0.0f},
        .rotation = rotationTrack(glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                  xRotation(30.0f)),
    };
    auto target = targetFixture();
    target.semantic.preset_expressions.emplace("happy", VrmExpression{});

    const auto retargeted =
        retargetVrmaClip(source.clip, target.rig, target.semantic);
    const auto sample = retargeted->sample(0.5f);

    REQUIRE(retargeted->profile.version == 1);
    REQUIRE(retargeted->profile.root_motion_policy ==
            VrmaRootMotionPolicy::preserve_hips_translation);
    REQUIRE(retargeted->profile.hips_translation_scale == 1.0f);
    REQUIRE(retargeted->warnings.empty());
    REQUIRE(retargeted->profile.bone_mappings.size() == requiredBones.size());
    REQUIRE(retargeted->profile.provenance.source_clip.source_uri ==
            source.clip.metadata.source_uri);
    REQUIRE(retargeted->profile.provenance.profile_version == 1);
    REQUIRE(retargeted->profile.provenance.source_rig_sha256.size() == 64);
    REQUIRE(retargeted->profile.provenance.target_rig_sha256.size() == 64);
    REQUIRE(sample.local_transforms.size() == target.rig.nodes.size());
    requireVec3(sample.local_transforms[0].translation,
                {0.125f, 1.25f, -0.25f});
    requireQuat(sample.local_transforms[1].rotation, yRotation(22.5f));
    REQUIRE(sample.expressions.size() == 1);
    REQUIRE(sample.expressions[0].expression == "happy");
    REQUIRE(sample.expressions[0].weight == Catch::Approx(0.5f));
    REQUIRE(sample.gaze);
    REQUIRE(sample.gaze->offset_from_head_bone ==
            std::optional<std::array<float, 3>>{{0.0f, 0.06f, 0.0f}});
    requireQuat(sample.gaze->rotation, xRotation(15.0f));
}

TEST_CASE("VRMA-R0 doubles hips translation for a target twice as tall",
          "[wp177][vrma][retarget]") {
    auto source = sourceFixture(1.0f);
    source.clip.body_channels = {{
        .human_bone = "hips",
        .source_node = 0,
        .path = VrmaBodyPath::translation,
        .track = translationTrack({0.0f, 1.0f, 0.0f},
                                  {0.25f, 1.5f, -0.5f}),
    }};
    auto target = targetFixture(2.0f);

    const auto retargeted =
        retargetVrmaClip(source.clip, target.rig, target.semantic);
    const auto sample = retargeted->sample(1.0f);

    REQUIRE(retargeted->profile.hips_translation_scale == Catch::Approx(2.0f));
    requireVec3(sample.local_transforms[0].translation, {0.5f, 3.0f, -1.0f});
}

TEST_CASE("VRMA-R0 skips only absent optional tracks and target expressions",
          "[wp177][vrma][retarget]") {
    auto source = sourceFixture();
    const int chest = static_cast<int>(source.rig->nodes.size());
    source.rig->nodes.push_back({.name = "chestSource", .parent = 1});
    source.rig->human_bones.push_back({.name = "chest", .source_node = chest});
    source.clip.body_channels = {
        {
            .human_bone = "chest",
            .source_node = chest,
            .path = VrmaBodyPath::rotation,
            .track = rotationTrack(glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                   xRotation(20.0f)),
        },
        {
            .human_bone = "spine",
            .source_node = 1,
            .path = VrmaBodyPath::rotation,
            .track = rotationTrack(glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                   yRotation(30.0f)),
        },
    };
    source.clip.expression_channels = {{
        .expression = "wave",
        .preset = false,
        .source_node = 15,
        .weight = scalarTrack(0.0f, 1.0f),
    }};
    auto target = targetFixture();

    const auto retargeted =
        retargetVrmaClip(source.clip, target.rig, target.semantic);
    const auto sample = retargeted->sample(1.0f);

    REQUIRE(retargeted->body_channels.size() == 1);
    REQUIRE(retargeted->body_channels[0].human_bone == "spine");
    REQUIRE(retargeted->expression_channels.empty());
    REQUIRE(retargeted->warnings.size() == 2);
    REQUIRE(retargeted->warnings[0].kind ==
            VrmaRetargetWarningKind::missing_optional_target_bone);
    REQUIRE(retargeted->warnings[0].subject == "chest");
    REQUIRE(retargeted->warnings[1].kind ==
            VrmaRetargetWarningKind::missing_target_expression);
    REQUIRE(retargeted->warnings[1].subject == "wave");
    requireQuat(sample.local_transforms[1].rotation, yRotation(30.0f));
    REQUIRE(sample.expressions.empty());
}

TEST_CASE("VRMA-R0 composes source rest delta onto target T-pose in world space",
          "[wp177][vrma][retarget]") {
    auto source = sourceFixture();
    auto target = targetFixture();
    constexpr std::size_t hips = 0;
    constexpr std::size_t spine = 1;
    constexpr std::size_t left_upper_arm = 9;
    constexpr std::size_t left_lower_arm = 10;
    constexpr std::size_t left_hand = 11;

    source.rig->nodes[spine].parent = static_cast<int>(hips);
    source.rig->nodes[spine].rotation = xyzw(xRotation(90.0f));
    source.rig->nodes[left_upper_arm].parent = static_cast<int>(spine);
    source.rig->nodes[left_upper_arm].rotation = xyzw(zRotation(90.0f));
    source.rig->nodes[left_lower_arm].parent = static_cast<int>(left_upper_arm);
    source.rig->nodes[left_lower_arm].translation = {0.5f, 0.0f, 0.0f};
    source.rig->nodes[left_hand].parent = static_cast<int>(left_lower_arm);
    source.rig->nodes[left_hand].translation = {0.5f, 0.0f, 0.0f};
    source.clip.body_channels = {{
        .human_bone = "leftUpperArm",
        .source_node = static_cast<int>(left_upper_arm),
        .path = VrmaBodyPath::rotation,
        .track = rotationTrack(zRotation(90.0f), zRotation(180.0f)),
    }};

    target.rig.nodes[spine].parent = static_cast<int>(hips);
    target.rig.nodes[spine].rotation = xRotation(90.0f);
    target.rig.nodes[left_upper_arm].parent = static_cast<int>(spine);
    target.rig.nodes[left_upper_arm].rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    target.rig.nodes[left_lower_arm].parent = static_cast<int>(left_upper_arm);
    target.rig.nodes[left_lower_arm].translation = {0.5f, 0.0f, 0.0f};
    target.rig.nodes[left_hand].parent = static_cast<int>(left_lower_arm);
    target.rig.nodes[left_hand].translation = {0.5f, 0.0f, 0.0f};

    const auto retargeted =
        retargetVrmaClip(source.clip, target.rig, target.semantic);
    const auto sample = retargeted->sample(1.0f);

    // source rest Z90 -> animated Z180 gives local delta Z90. Reapplying that
    // delta to target rest identity, under target parent X90, takes the hand's
    // +X unit offset to +Z. The hips contributes the explicit +Y rest height.
    requireQuat(sample.local_transforms[left_upper_arm].rotation,
                zRotation(90.0f));
    const auto hand_world = worldMatrix(left_hand, target.rig, sample);
    requireVec3(glm::vec3{hand_world[3]}, {0.0f, 1.0f, 1.0f});
}

TEST_CASE("VRMA-R0 rejects invalid profile rig and hips scale inputs",
          "[wp177][vrma][retarget][negative]") {
    SECTION("unsupported profile version") {
        auto source = sourceFixture();
        auto target = targetFixture();
        REQUIRE_THROWS_WITH(
            retargetVrmaClip(source.clip, target.rig, target.semantic,
                             VrmaRetargetProfileOptions{.version = 2}),
            Catch::Matchers::ContainsSubstring("unsupported profile version 2"));
    }
    SECTION("missing required target bone") {
        auto source = sourceFixture();
        auto target = targetFixture();
        target.semantic.human_bones.pop_back();
        REQUIRE_THROWS_WITH(
            retargetVrmaClip(source.clip, target.rig, target.semantic),
            Catch::Matchers::ContainsSubstring(
                "target humanoid is missing required bone 'rightHand'"));
    }
    SECTION("zero source hips rest height") {
        auto source = sourceFixture(0.0f);
        source.clip.body_channels = {{
            .human_bone = "hips",
            .source_node = 0,
            .path = VrmaBodyPath::translation,
            .track = translationTrack({0.0f, 0.0f, 0.0f},
                                      {0.25f, 0.0f, 0.0f}),
        }};
        auto target = targetFixture();
        REQUIRE_THROWS_WITH(
            retargetVrmaClip(source.clip, target.rig, target.semantic),
            Catch::Matchers::ContainsSubstring(
                "source hips rest height must be non-zero"));
    }
    SECTION("cyclic target hierarchy") {
        auto source = sourceFixture();
        auto target = targetFixture();
        target.rig.nodes[0].parent = 1;
        target.rig.nodes[1].parent = 0;
        REQUIRE_THROWS_WITH(
            retargetVrmaClip(source.clip, target.rig, target.semantic),
            Catch::Matchers::ContainsSubstring(
                "target rig hierarchy contains a cycle"));
    }
}

} // namespace Pelican
