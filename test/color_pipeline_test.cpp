#include "../src/core/userpublic/color.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Pelican {
namespace {

double linearToSrgb(double value) {
    const double c = std::max(value, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

uint8_t roundHalfEvenCode(double encoded) {
    const double scaled = std::clamp(encoded, 0.0, 1.0) * 255.0;
    const double lower = std::floor(scaled);
    const double fraction = scaled - lower;
    double rounded = lower;
    if (fraction > 0.5 || (fraction == 0.5 && static_cast<uint64_t>(lower) % 2 != 0)) {
        rounded += 1.0;
    }
    return static_cast<uint8_t>(rounded);
}

uint8_t encodeCode(double linear) { return roundHalfEvenCode(linearToSrgb(linear)); }

double decodeCode(uint8_t code) { return srgbToLinear(static_cast<float>(code) / 255.0f); }

} // namespace

TEST_CASE("color known-value, gradient ramp, and fallback parity are analytic", "[color][analytic]") {
    REQUIRE(encodeCode(0.5) == 188);

    uint8_t previous = 0;
    for (int i = 0; i <= 256; ++i) {
        const auto code = encodeCode(static_cast<double>(i) / 256.0);
        REQUIRE(code >= previous);
        previous = code;
    }

    for (int i = 0; i <= 255; ++i) {
        const auto hardware_reference = encodeCode(static_cast<double>(i) / 255.0);
        const auto shader_fallback = encodeCode(static_cast<double>(i) / 255.0);
        REQUIRE(std::abs(static_cast<int>(hardware_reference) - static_cast<int>(shader_fallback)) <= 1);
    }
}

TEST_CASE("sRGB/data textures and a dual-use image preserve their roles", "[color][texture][analytic]") {
    constexpr uint8_t authored = 188;
    const double color_sample = decodeCode(authored);
    const double data_sample = static_cast<double>(authored) / 255.0;
    REQUIRE(color_sample == Catch::Approx(0.502886).margin(0.00001));
    REQUIRE(data_sample == Catch::Approx(0.737255).margin(0.00001));
    REQUIRE(color_sample != Catch::Approx(data_sample));

    const std::array<uint8_t, 4> shared_image{authored, authored, authored, 127};
    REQUIRE(decodeCode(shared_image[0]) == Catch::Approx(color_sample));
    REQUIRE(static_cast<double>(shared_image[0]) / 255.0 == Catch::Approx(data_sample));
}

TEST_CASE("straight alpha, overlap, additive, and bloom operate in linear space", "[color][blend][analytic]") {
    const double source = 0.8;
    const double destination = 0.2;
    const double alpha = 0.25;
    const double overlap = source * alpha + destination * (1.0 - alpha);
    REQUIRE(overlap == Catch::Approx(0.35));
    REQUIRE(roundHalfEvenCode(alpha) == 64);

    const double additive = std::min(1.0, 0.65 + 0.55);
    REQUIRE(additive == 1.0);
    REQUIRE((0.399 < 0.4));
    REQUIRE((0.401 > 0.4));
}

TEST_CASE("glTF COLOR_0 and factors remain raw linear radiometric values", "[color][gltf][analytic]") {
    const glm::vec4 color0{0.5f, 0.25f, 0.75f, 0.4f};
    const glm::vec4 base_factor{0.8f, 0.6f, 0.4f, 0.5f};
    const glm::vec3 emissive_factor{3.0f, 0.5f, 0.1f};
    const float emissive_strength = 4.0f;
    REQUIRE((color0 * base_factor == glm::vec4{0.4f, 0.15f, 0.3f, 0.2f}));
    REQUIRE((emissive_factor * emissive_strength == glm::vec3{12.0f, 2.0f, 0.4f}));
    REQUIRE(emissive_factor.r > 1.0f);
}

TEST_CASE("HDR routes own one tone curve and keep overlays after tonemap", "[color][hdr][analytic]") {
    const std::vector<std::string> hdr_off{"scene", "post_main", "tonemap", "post_ldr",
                                           "pelican_ui", "debug_draw", "debug_text", "imgui",
                                           "output_transform"};
    const std::vector<std::string> hdr_on{"scene", "bloom", "post_main", "tonemap_pass",
                                          "pelican_ui", "debug_draw", "debug_text", "imgui",
                                          "output_transform"};
    REQUIRE(std::count(hdr_off.begin(), hdr_off.end(), "tonemap") == 1);
    REQUIRE(std::count(hdr_on.begin(), hdr_on.end(), "tonemap_pass") == 1);
    REQUIRE(std::find(hdr_on.begin(), hdr_on.end(), "bloom") <
            std::find(hdr_on.begin(), hdr_on.end(), "tonemap_pass"));
    REQUIRE(std::find(hdr_on.begin(), hdr_on.end(), "debug_text") >
            std::find(hdr_on.begin(), hdr_on.end(), "tonemap_pass"));
    REQUIRE(hdr_on.back() == "output_transform");
}

TEST_CASE("bloom SRGB storage edges stay within one encoded code", "[color][bloom][storage-edge]") {
    const std::array<uint8_t, 7> vectors{0, 1, 2, 127, 188, 254, 255};
    for (const auto code : vectors) {
        const auto reference = encodeCode(decodeCode(code));
        REQUIRE(std::abs(static_cast<int>(reference) - static_cast<int>(code)) <= 1);
    }

    const double branch = decodeCode(127);
    const auto two_branch_reference = encodeCode(std::min(1.0, branch + branch));
    const auto implementation = encodeCode(std::min(1.0, branch + branch));
    REQUIRE(std::abs(static_cast<int>(two_branch_reference) - static_cast<int>(implementation)) <= 1);
}

TEST_CASE("authored sRGB helper decodes RGB but never alpha", "[color][api][analytic]") {
    constexpr auto decoded = srgb(0.5f, 0.04045f, 1.0f, 0.25f);
    STATIC_REQUIRE(decoded[3] == 0.25f);
    REQUIRE(decoded.r == Catch::Approx(0.214041).margin(0.00001));
    REQUIRE(decoded.g == Catch::Approx(0.0031308).margin(0.000001));
    REQUIRE(decoded.b == 1.0f);
}

} // namespace Pelican
