#include <catch2/catch_test_macros.hpp>

#include "../src/core/light/lightcontainer.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

LightLoadEntry entry(std::string type, std::string name) {
    return LightLoadEntry{
        .name = std::move(name),
        .component = nlohmann::json{{"type", std::move(type)}},
    };
}

} // namespace

TEST_CASE("light cap warnings name every dropped light and its one-based ordinal",
          "[light][light-cap][wp142]") {
    std::vector<LightLoadEntry> lights;
    for (size_t i = 1; i <= MAX_DIRECTIONAL_LIGHTS + 2; ++i) {
        lights.push_back(entry("directional", "Directional" + std::to_string(i)));
    }
    for (size_t i = 1; i <= MAX_POINT_LIGHTS + 2; ++i) {
        lights.push_back(entry("point", "Point" + std::to_string(i)));
    }
    for (size_t i = 1; i <= MAX_SPOT_LIGHTS + 2; ++i) {
        lights.push_back(entry("spot", "Spot" + std::to_string(i)));
    }

    REQUIRE(collectLightCapWarnings(lights) == std::vector<std::string>{
        "Light cap exceeded: directional light #9 'Directional9' will not be rendered (cap 8)",
        "Light cap exceeded: directional light #10 'Directional10' will not be rendered (cap 8)",
        "Light cap exceeded: point light #17 'Point17' will not be rendered (cap 16)",
        "Light cap exceeded: point light #18 'Point18' will not be rendered (cap 16)",
        "Light cap exceeded: spot light #9 'Spot9' will not be rendered (cap 8)",
        "Light cap exceeded: spot light #10 'Spot10' will not be rendered (cap 8)",
    });
}

TEST_CASE("light cap warning uses an explicit placeholder for an unnamed dropped light",
          "[light][light-cap][wp142]") {
    std::vector<LightLoadEntry> lights;
    for (size_t i = 0; i < MAX_DIRECTIONAL_LIGHTS; ++i) {
        lights.push_back(entry("directional", "Directional" + std::to_string(i + 1)));
    }
    lights.push_back(entry("directional", ""));

    REQUIRE(collectLightCapWarnings(lights) == std::vector<std::string>{
        "Light cap exceeded: directional light #9 '<unnamed>' will not be rendered (cap 8)",
    });
}

} // namespace Pelican
