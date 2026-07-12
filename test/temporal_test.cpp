#include "../src/core/renderer/temporal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_transform.hpp>
#include <nlohmann/json.hpp>

namespace Pelican {

TEST_CASE("constant object translation matches analytic screen-space velocity",
          "[temporal][velocity]") {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                      "test/fixtures/temporal_velocity.json";
    std::ifstream file{path};
    const auto fixture = nlohmann::json::parse(file);
    const auto point_values = fixture.at("point").get<std::vector<float>>();
    const auto previous_values = fixture.at("previous_translation").get<std::vector<float>>();
    const auto current_values = fixture.at("current_translation").get<std::vector<float>>();
    const auto expected = fixture.at("expected_velocity").get<std::vector<float>>();
    const glm::vec4 point{point_values[0], point_values[1], point_values[2], point_values[3]};
    const auto previous_model = glm::translate(glm::mat4{1.0f},
        glm::vec3{previous_values[0], previous_values[1], previous_values[2]});
    const auto current_model = glm::translate(glm::mat4{1.0f},
        glm::vec3{current_values[0], current_values[1], current_values[2]});
    const auto actual = screenSpaceVelocity(current_model * point, previous_model * point);
    REQUIRE(actual.x == Catch::Approx(expected[0]));
    REQUIRE(actual.y == Catch::Approx(expected[1]));
}

} // namespace Pelican
