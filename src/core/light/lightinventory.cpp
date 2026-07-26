#include "lightinventory.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace Pelican {

namespace {

static_assert(sizeof(glm::uvec4) == 16);
static_assert(sizeof(float) == sizeof(std::uint32_t));

std::uint32_t bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

glm::uvec4 floatBits(
    glm::vec3 xyz, float w) {
    return {
        bits(xyz.x), bits(xyz.y), bits(xyz.z), bits(w)};
}

std::uint64_t stableLightName(
    std::string_view name) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : name) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

glm::uvec4 recordMetadata(
    LightInventoryPresetV2 preset,
    std::string_view name) {
    const auto identity = stableLightName(name);
    return {
        static_cast<std::uint32_t>(preset),
        lightInventoryNoExtension,
        static_cast<std::uint32_t>(identity),
        static_cast<std::uint32_t>(identity >> 32),
    };
}

void appendDirectional(
    std::vector<glm::uvec4> &elements,
    const DirectionalLight &light) {
    elements.push_back(
        recordMetadata(
            LightInventoryPresetV2::directional,
            light.name));
    elements.push_back(
        floatBits(light.direction, light.intensity));
    elements.push_back(
        floatBits(light.color, 0.0f));
    elements.push_back(glm::uvec4{0u});
}

void appendPoint(
    std::vector<glm::uvec4> &elements,
    const PointLight &light) {
    elements.push_back(
        recordMetadata(
            LightInventoryPresetV2::point,
            light.name));
    elements.push_back(
        floatBits(light.position, light.intensity));
    // A zero range asks the selection algorithm to derive one from radiance
    // and its project-owned threshold.
    elements.push_back(
        floatBits(light.color, 0.0f));
    elements.push_back(glm::uvec4{0u});
}

void appendSpot(
    std::vector<glm::uvec4> &elements,
    const SpotLight &light) {
    elements.push_back(
        recordMetadata(
            LightInventoryPresetV2::spot,
            light.name));
    elements.push_back(
        floatBits(light.position, light.intensity));
    elements.push_back(
        floatBits(
            light.color,
            std::cos(glm::radians(
                light.outerConeAngle))));
    elements.push_back(
        floatBits(
            light.direction,
            std::cos(glm::radians(
                light.innerConeAngle))));
}

std::uint32_t checkedCount(
    std::size_t value,
    std::string_view context) {
    if (value >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            std::string{context} +
            " exceeds the v2 inventory count range");
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

PackedLightInventoryV2 packLightInventoryV2(
    std::span<const DirectionalLight> directional,
    std::span<const PointLight> point,
    std::span<const SpotLight> spot,
    vk::DeviceSize byte_capacity) {
    constexpr auto element_bytes =
        static_cast<vk::DeviceSize>(
            sizeof(glm::uvec4));
    const auto minimum_bytes =
        element_bytes *
        lightInventoryV2HeaderElements;
    if (byte_capacity < minimum_bytes) {
        throw std::runtime_error(
            "scene_lights_v2 inventory requires at least " +
            std::to_string(minimum_bytes) + " bytes");
    }

    const auto element_capacity =
        byte_capacity / element_bytes;
    const auto raw_record_capacity =
        (element_capacity -
         lightInventoryV2HeaderElements) /
        lightInventoryV2RecordElements;
    const auto record_capacity =
        static_cast<std::uint32_t>(
            std::min<vk::DeviceSize>(
                raw_record_capacity,
                std::numeric_limits<std::uint32_t>::max()));

    const auto directional_input =
        checkedCount(
            directional.size(),
            "directional light count");
    const auto point_input =
        checkedCount(
            point.size(), "point light count");
    const auto spot_input =
        checkedCount(
            spot.size(), "spot light count");
    const auto total64 =
        static_cast<std::uint64_t>(
            directional_input) +
        point_input + spot_input;
    if (total64 >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "scene light count exceeds the v2 inventory range");
    }
    const auto input_count =
        static_cast<std::uint32_t>(total64);

    PackedLightInventoryV2 result;
    result.input_count = input_count;
    result.record_capacity = record_capacity;
    result.accepted_count =
        std::min(input_count, record_capacity);
    result.dropped_count =
        input_count - result.accepted_count;
    result.elements.reserve(
        lightInventoryV2HeaderElements +
        static_cast<std::size_t>(
            result.accepted_count) *
            lightInventoryV2RecordElements);
    result.elements.resize(
        lightInventoryV2HeaderElements);

    auto remaining = result.accepted_count;
    result.directional_count =
        std::min(directional_input, remaining);
    for (std::uint32_t index = 0;
         index < result.directional_count; ++index) {
        appendDirectional(
            result.elements,
            directional[index]);
    }
    remaining -= result.directional_count;

    result.point_count =
        std::min(point_input, remaining);
    for (std::uint32_t index = 0;
         index < result.point_count; ++index) {
        appendPoint(result.elements, point[index]);
    }
    remaining -= result.point_count;

    result.spot_count =
        std::min(spot_input, remaining);
    for (std::uint32_t index = 0;
         index < result.spot_count; ++index) {
        appendSpot(result.elements, spot[index]);
    }

    result.elements[0] = {
        lightInventoryV2Magic,
        lightInventoryV2Version,
        result.accepted_count,
        result.dropped_count,
    };
    result.elements[1] = {
        result.directional_count,
        result.point_count,
        result.spot_count,
        lightInventoryV2RecordElements,
    };
    return result;
}

} // namespace Pelican
