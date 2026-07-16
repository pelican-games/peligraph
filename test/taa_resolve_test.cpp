#include "../src/core/renderer/projectionjitter.hpp"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

namespace {

nlohmann::json readFixture() {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                      "test/fixtures/taa_resolve_equations.json";
    std::ifstream file{path, std::ios::binary};
    return nlohmann::json::parse(file);
}

glm::vec2 vec2(const nlohmann::json &value) {
    return {value.at(0).get<float>(), value.at(1).get<float>()};
}

glm::vec3 vec3(const nlohmann::json &value) {
    return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}

bool inUnitSquare(glm::vec2 uv) {
    return uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
}

float linearizeDepth(const glm::mat4 &projection, float depth, glm::vec2 uv) {
    const auto q = glm::inverse(projection) * glm::vec4{uv * 2.0f - 1.0f, depth, 1.0f};
    return -q.z / q.w;
}

} // namespace

TEST_CASE("TAA reprojection sign OOB edges and corners match the normative UV equations",
          "[taa][resolve][equations]") {
    const auto fixture = readFixture();
    for (const auto &entry : fixture.at("reprojection")) {
        DYNAMIC_SECTION(entry.at("name").get<std::string>()) {
            const auto history_uv = vec2(entry.at("current_uv")) -
                                    vec2(entry.at("velocity_uv"));
            const auto expected = vec2(entry.at("expected_history_uv"));
            REQUIRE(history_uv.x == Catch::Approx(expected.x).margin(1e-6f));
            REQUIRE(history_uv.y == Catch::Approx(expected.y).margin(1e-6f));
            REQUIRE(inUnitSquare(history_uv) == entry.at("valid").get<bool>());
        }
    }
}

TEST_CASE("TAA depth linearization reconstructs positive view depth for perspective and ortho",
          "[taa][resolve][equations][depth]") {
    const std::vector<glm::mat4> projections{
        applyProjectionJitter(glm::perspectiveRH_ZO(glm::radians(60.0f), 1.5f, 0.1f, 100.0f),
                              {0.03125f, -0.046875f}),
        applyProjectionJitter(glm::orthoRH_ZO(-3.0f, 3.0f, -2.0f, 2.0f, 0.1f, 50.0f),
                              {-0.0625f, 0.03125f}),
    };
    for (const auto &projection : projections) {
        for (const float view_depth : {0.25f, 3.0f, 20.0f}) {
            const glm::vec4 view_position{0.2f, -0.15f, -view_depth, 1.0f};
            const auto clip = projection * view_position;
            const glm::vec3 ndc = glm::vec3{clip} / clip.w;
            const glm::vec2 uv = glm::vec2{ndc} * 0.5f + 0.5f;
            REQUIRE(linearizeDepth(projection, ndc.z, uv) ==
                    Catch::Approx(view_depth).epsilon(1e-5f));
        }
    }
}

TEST_CASE("TAA disocclusion uses a strict greater-than threshold and epoch-pair reset",
          "[taa][resolve][equations][reset]") {
    const auto fixture = readFixture();
    for (const auto &entry : fixture.at("disocclusion")) {
        DYNAMIC_SECTION(entry.at("name").get<std::string>()) {
            const auto current = entry.at("current_depth").get<double>();
            const auto history = entry.at("history_depth").get<double>();
            const auto epsilon = entry.at("epsilon").get<double>();
            const auto tau = entry.at("tau").get<double>();
            const bool reject = std::abs(history - current) /
                                    std::max(std::abs(current), epsilon) >
                                tau;
            REQUIRE(reject == entry.at("reject").get<bool>());
        }
    }
    for (const auto &entry : fixture.at("epoch")) {
        const bool valid = entry.at("current").get<std::uint32_t>() ==
                           entry.at("previous").get<std::uint32_t>();
        REQUIRE(valid == entry.at("valid").get<bool>());
    }
}

TEST_CASE("TAA 3x3 neighborhood clamps four sides and corners before blending",
          "[taa][resolve][equations][clamp]") {
    const auto fixture = readFixture();
    const auto &edge = fixture.at("edge_neighborhood");
    const auto extent = edge.at("extent").get<std::vector<int>>();
    const auto pixel = edge.at("pixel").get<std::vector<int>>();
    std::size_t index = 0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int sample_x = std::clamp(pixel[0] + x, 0, extent[0] - 1);
            const int sample_y = std::clamp(pixel[1] + y, 0, extent[1] - 1);
            REQUIRE(edge.at("expected").at(index).at(0) == sample_x);
            REQUIRE(edge.at("expected").at(index).at(1) == sample_y);
            ++index;
        }
    }

    const auto &blend = fixture.at("clamp_blend");
    const auto history = vec3(blend.at("history"));
    const auto minimum = vec3(blend.at("minimum"));
    const auto maximum = vec3(blend.at("maximum"));
    const auto current = vec3(blend.at("current"));
    const auto alpha = blend.at("alpha").get<float>();
    const auto actual = glm::mix(glm::clamp(history, minimum, maximum), current, alpha);
    const auto expected = vec3(blend.at("expected"));
    REQUIRE(actual.x == Catch::Approx(expected.x).margin(1e-6f));
    REQUIRE(actual.y == Catch::Approx(expected.y).margin(1e-6f));
    REQUIRE(actual.z == Catch::Approx(expected.z).margin(1e-6f));
}

} // namespace Pelican
