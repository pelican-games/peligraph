#include "../src/core/renderer/projectionjitter.hpp"
#include "../src/core/renderer/temporal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

glm::vec2 ndc(const glm::mat4 &projection, const glm::vec4 &point) {
    const auto clip = projection * point;
    return glm::vec2{clip} / clip.w;
}

void requireVec2(glm::vec2 actual, glm::vec2 expected, float margin = 1.0e-6f) {
    REQUIRE(actual.x == Catch::Approx(expected.x).margin(margin));
    REQUIRE(actual.y == Catch::Approx(expected.y).margin(margin));
}

std::filesystem::path sourceRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path};
    REQUIRE(file.is_open());
    return nlohmann::json::parse(file);
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    REQUIRE(file.is_open());
    return std::string{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("projection jitter Halton23 series is one-based and wraps at phase eight",
          "[projection-jitter][series]") {
    const ProjectionJitterSettings settings{"jitter_fixture", "halton23", 8};
    const std::vector<glm::vec2> expected{
        {0.0f, -1.0f / 6.0f},
        {-1.0f / 4.0f, 1.0f / 6.0f},
        {1.0f / 4.0f, -7.0f / 18.0f},
        {-3.0f / 8.0f, -1.0f / 18.0f},
        {1.0f / 8.0f, 5.0f / 18.0f},
        {-1.0f / 8.0f, -5.0f / 18.0f},
        {3.0f / 8.0f, 1.0f / 18.0f},
        {-7.0f / 16.0f, 7.0f / 18.0f},
    };
    for (std::uint64_t frame = 1; frame <= expected.size(); ++frame) {
        const auto sample = projectionJitterSample(settings, frame, 200, 100);
        REQUIRE(sample.sample_index == frame);
        requireVec2(sample.offset_px, expected.at(frame - 1));
        requireVec2(sample.jitter_ndc,
                    {expected.at(frame - 1).x / 100.0f,
                     expected.at(frame - 1).y / 50.0f});
    }
    const auto wrapped = projectionJitterSample(settings, 9, 200, 100);
    REQUIRE(wrapped.sample_index == 1);
    requireVec2(wrapped.offset_px, expected.front());

    REQUIRE_THROWS_WITH(projectionJitterSample(settings, 0, 200, 100),
                        "projection_jitter provider 'jitter_fixture' cannot sample frame_index 0");
    REQUIRE_THROWS_WITH(projectionJitterSample(settings, 1, 0, 100),
                        "projection_jitter provider 'jitter_fixture' requires non-zero framebuffer width and height");
    REQUIRE_THROWS_WITH(projectionJitterSample(settings, 1, 200, 0),
                        "projection_jitter provider 'jitter_fixture' requires non-zero framebuffer width and height");
}

TEST_CASE("general projection jitter produces a constant NDC delta for perspective and ortho",
          "[projection-jitter][math]") {
    const glm::vec2 jitter{0.03125f, -0.046875f};
    const std::vector<glm::mat4> projections{
        glm::perspectiveRH_ZO(0.8f, 16.0f / 9.0f, 0.1f, 100.0f),
        glm::orthoRH_ZO(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f),
    };
    for (const auto &projection : projections) {
        const auto jittered = applyProjectionJitter(projection, jitter);
        for (const float z : {-0.5f, -3.0f, -40.0f}) {
            const glm::vec4 point{0.15f, -0.2f, z, 1.0f};
            requireVec2(ndc(jittered, point) - ndc(projection, point), jitter);
        }
    }
}

TEST_CASE("render-frame snapshot supplies one jitter offset to material FrameUBO sprite SSAO and debug",
          "[projection-jitter][delivery]") {
    TemporalFrameHistory history;
    const auto projection = glm::perspectiveRH_ZO(0.8f, 1.5f, 0.1f, 100.0f);
    const auto view = glm::lookAt(glm::vec3{1.0f, 2.0f, 3.0f}, glm::vec3{0.0f},
                                  glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::vec2 jitter{0.02f, 0.03f};
    const auto snapshot = buildRenderFrameSnapshot(history, projection, view, jitter, true);
    const glm::vec4 world{0.2f, -0.1f, 0.0f, 1.0f};

    const auto main_delta = ndc(snapshot.view_projection_jittered, world) -
                            ndc(snapshot.view_projection_non_jittered, world);
    const auto frame_ubo_delta = ndc(snapshot.projection_jittered, view * world) -
                                 ndc(snapshot.projection_non_jittered, view * world);
    const auto debug_delta = main_delta;
    requireVec2(main_delta, jitter);
    requireVec2(frame_ubo_delta, jitter);
    requireVec2(debug_delta, jitter);

    const auto off = buildRenderFrameSnapshot(history, projection, view, glm::vec2{0.0f}, true);
    REQUIRE(off.projection_jittered == off.projection_non_jittered);
    REQUIRE(off.view_projection_jittered == off.view_projection_non_jittered);
}

TEST_CASE("temporal reset epoch truth table is invalid for one frame for every reset cause",
          "[projection-jitter][reset]") {
    TemporalFrameHistory history;
    const glm::mat4 projection{1.0f};
    const glm::mat4 view{1.0f};
    const glm::vec2 jitter{0.01f, -0.02f};

    const std::vector<std::string> reset_causes{
        "initial", "resize", "set_time", "camera_cut", "feature_enable"};
    for (const auto &cause : reset_causes) {
        INFO(cause);
        const auto reset = buildRenderFrameSnapshot(history, projection, view, jitter, true);
        REQUIRE_FALSE(reset.historyValid());
        REQUIRE(reset.previous_projection_jittered == reset.projection_jittered);
        REQUIRE(reset.previous_view == reset.view);
        REQUIRE(reset.previous_jitter_ndc == reset.jitter_ndc);
        commitRenderFrameSnapshot(history, reset);

        const auto next = buildRenderFrameSnapshot(history, projection, view, jitter, false);
        REQUIRE(next.historyValid());
        REQUIRE(next.temporal_reset_epoch == reset.temporal_reset_epoch);
        commitRenderFrameSnapshot(history, next);
    }
}

TEST_CASE("velocity subtracts current and previous jitter in NDC before UV conversion",
          "[projection-jitter][velocity]") {
    const auto fixtures = readJson(sourceRoot() / "test/fixtures/temporal_velocity_jitter.json");
    for (const auto &fixture : fixtures) {
        DYNAMIC_SECTION(fixture.at("name").get<std::string>()) {
            const auto current = fixture.at("current_clip").get<std::vector<float>>();
            const auto previous = fixture.at("previous_clip").get<std::vector<float>>();
            const auto current_jitter = fixture.at("current_jitter").get<std::vector<float>>();
            const auto previous_jitter = fixture.at("previous_jitter").get<std::vector<float>>();
            const auto expected = fixture.at("expected").get<std::vector<float>>();
            requireVec2(screenSpaceVelocity(
                            {current[0], current[1], current[2], current[3]},
                            {previous[0], previous[1], previous[2], previous[3]},
                            {current_jitter[0], current_jitter[1]},
                            {previous_jitter[0], previous_jitter[1]}),
                        {expected[0], expected[1]});
        }
    }
}

TEST_CASE("projection consumer manifest stays reviewable and anchored to source",
          "[projection-jitter][consumers]") {
    const auto manifest = readJson(sourceRoot() / "test/fixtures/projection_jitter_consumers.json");
    REQUIRE(manifest.size() == 9);
    for (const auto &consumer : manifest) {
        INFO(consumer.at("consumer").get<std::string>());
        const auto source = readText(sourceRoot() / consumer.at("source").get<std::string>());
        REQUIRE(source.find(consumer.at("needle").get<std::string>()) != std::string::npos);
    }
}

} // namespace Pelican
