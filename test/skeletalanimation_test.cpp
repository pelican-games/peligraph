#include "../src/core/model/skeletalanimation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cstring>

namespace Pelican {
namespace {
SkeletalModelData fixtureModel() {
    SkeletalModelData model;
    model.nodes = {
        {.parent = -1},
        {.parent = 0, .translation = {0.0f, 1.0f, 0.0f}},
    };
    model.joint_nodes = {0, 1};
    model.inverse_bind_matrices = {glm::mat4{1.0f},
        glm::translate(glm::mat4{1.0f}, glm::vec3{0.0f, -1.0f, 0.0f})};
    const float s = 0.7071067811865475f;
    model.clips.push_back({
        .name = "Turn", .start = 0.0f, .end = 2.0f,
        .channels = {{.node = 0, .path = AnimationPath::rotation,
                      .interpolation = AnimationInterpolation::linear,
                      .times = {0.0f, 1.0f, 2.0f},
                      .values = {{0,0,0,1}, {0,0,s,s}, {0,0,1,0}}}},
    });
    model.clips.push_back({
        .name = "Step", .start = 0.0f, .end = 2.0f,
        .channels = {{.node = 1, .path = AnimationPath::translation,
                      .interpolation = AnimationInterpolation::step,
                      .times = {0.0f, 1.0f, 2.0f},
                      .values = {{0,1,0,0}, {1,1,0,0}, {2,1,0,0}}}},
    });
    return model;
}
} // namespace

TEST_CASE("two-bone palette evaluates LINEAR STEP loop and fixed-time determinism", "[skeletal]") {
    const auto model = fixtureModel();
    const auto &turn = findAnimationClip(model, "Turn");
    const auto half = evaluateSkinPalette(model, &turn, 0.5, 1.0, false, 0.0);
    const auto p = glm::vec3{half[0] * glm::vec4{1, 0, 0, 1}};
    REQUIRE(p.x == Catch::Approx(0.70710678f));
    REQUIRE(p.y == Catch::Approx(0.70710678f));

    const auto again = evaluateSkinPalette(model, &turn, 0.5, 1.0, false, 0.0);
    REQUIRE(std::memcmp(half.data(), again.data(), half.size() * sizeof(glm::mat4)) == 0);
    const auto loop_boundary = evaluateSkinPalette(model, &turn, 2.0, 1.0, true, 0.0);
    REQUIRE(loop_boundary[0] == glm::mat4{1.0f});
    REQUIRE(loop_boundary[1] == glm::mat4{1.0f});

    const auto &step = findAnimationClip(model, "Step");
    const auto before = evaluateSkinPalette(model, &step, 0.999, 1.0, false, 0.0);
    const auto after = evaluateSkinPalette(model, &step, 1.0, 1.0, false, 0.0);
    REQUIRE(before[1][3].x == Catch::Approx(0.0f));
    REQUIRE(after[1][3].x == Catch::Approx(1.0f));
}

} // namespace Pelican
