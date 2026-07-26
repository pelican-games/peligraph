#include <catch2/catch_test_macros.hpp>

#include "../src/core/light/lightcontainer.hpp"

#include <bit>
#include <cstdint>
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

std::vector<PointLight> pointLights(
    std::size_t count) {
    std::vector<PointLight> result;
    result.reserve(count);
    for (std::size_t index = 0;
         index < count; ++index) {
        result.push_back(PointLight{
            .name =
                "Point" + std::to_string(index),
            .position =
                glm::vec3{
                    static_cast<float>(index),
                    2.0f, -3.0f},
            .intensity = 1.0f +
                         static_cast<float>(index),
            .color = glm::vec3{1.0f},
        });
    }
    return result;
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

TEST_CASE(
    "lighting inventory v2 retains more than the legacy 32 lights",
    "[light][inventory-v2][clustered][wp208]") {
    const std::vector<DirectionalLight> directional{
        DirectionalLight{
            .name = "Sun",
            .direction = {0.0f, -1.0f, 0.0f},
            .intensity = 3.0f,
            .color = {1.0f, 0.9f, 0.8f}},
        DirectionalLight{
            .name = "Fill",
            .direction = {1.0f, -1.0f, 0.0f},
            .intensity = 0.5f,
            .color = {0.2f, 0.3f, 1.0f}},
    };
    const auto points = pointLights(40);
    const std::vector<SpotLight> spots{
        SpotLight{
            .name = "Spot0",
            .position = {0.0f, 2.0f, 0.0f},
            .direction = {0.0f, -1.0f, 0.0f},
            .intensity = 2.0f,
            .innerConeAngle = 12.5f,
            .outerConeAngle = 17.5f,
            .color = {1.0f, 1.0f, 1.0f}},
    };
    constexpr std::uint32_t record_count = 43;
    const auto capacity =
        sizeof(glm::uvec4) *
        (lightInventoryV2HeaderElements +
         record_count *
             lightInventoryV2RecordElements);

    const auto packed =
        packLightInventoryV2(
            directional, points, spots, capacity);

    REQUIRE(packed.input_count == record_count);
    REQUIRE(packed.accepted_count == record_count);
    REQUIRE(packed.dropped_count == 0);
    REQUIRE(packed.directional_count == 2);
    REQUIRE(packed.point_count == 40);
    REQUIRE(packed.spot_count == 1);
    REQUIRE((
        packed.elements[0] ==
        glm::uvec4{
            lightInventoryV2Magic,
            lightInventoryV2Version,
            record_count, 0}));
    REQUIRE((
        packed.elements[1] ==
        glm::uvec4{
            2, 40, 1,
            lightInventoryV2RecordElements}));

    const auto last_point_base =
        lightInventoryV2HeaderElements +
        (2u + 39u) *
            lightInventoryV2RecordElements;
    REQUIRE(
        packed.elements[last_point_base].x ==
        static_cast<std::uint32_t>(
            LightInventoryPresetV2::point));
    REQUIRE(
        std::bit_cast<float>(
            packed.elements[
                last_point_base + 1]
                .x) == 39.0f);
}

TEST_CASE(
    "lighting inventory v2 truncates deterministically at byte capacity",
    "[light][inventory-v2][overflow][wp208]") {
    const std::vector<DirectionalLight> directional{
        DirectionalLight{
            .name = "Sun",
            .direction = {0.0f, -1.0f, 0.0f},
            .intensity = 1.0f,
            .color = {1.0f, 1.0f, 1.0f}},
        DirectionalLight{
            .name = "Fill",
            .direction = {1.0f, -1.0f, 0.0f},
            .intensity = 1.0f,
            .color = {1.0f, 1.0f, 1.0f}},
    };
    const auto points = pointLights(40);
    constexpr std::uint32_t record_capacity = 33;
    const auto capacity =
        sizeof(glm::uvec4) *
        (lightInventoryV2HeaderElements +
         record_capacity *
             lightInventoryV2RecordElements);

    const auto packed =
        packLightInventoryV2(
            directional, points,
            std::span<const SpotLight>{},
            capacity);

    REQUIRE(packed.input_count == 42);
    REQUIRE(packed.accepted_count ==
            record_capacity);
    REQUIRE(packed.dropped_count == 9);
    REQUIRE(packed.directional_count == 2);
    REQUIRE(packed.point_count == 31);
    REQUIRE(packed.spot_count == 0);
    REQUIRE(packed.elements[0].w == 9);
    const auto last_record =
        lightInventoryV2HeaderElements +
        (record_capacity - 1u) *
            lightInventoryV2RecordElements;
    REQUIRE(
        std::bit_cast<float>(
            packed.elements[
                last_record + 1]
                .x) == 30.0f);
}

} // namespace Pelican
