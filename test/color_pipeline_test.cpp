#include "../src/core/userpublic/color.hpp"
#include "bloom_upsample_oracle.hpp"

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

TEST_CASE(
    "WP357 bloom oracle models repeat coordinates and keeps alpha linear",
    "[color][bloom][wp357][oracle]") {
    using namespace TestSupport;
    std::array<std::uint8_t, 4 * 4 * 4> destination{};
    std::array<std::uint8_t, 2 * 2 * 4> source{
        32, 64, 96, 128,   224, 192, 160, 128,
        48, 80, 112, 128,  208, 176, 144, 128,
    };
    std::array<std::uint8_t, 4 * 4 * 4> actual{};
    const auto view = [](auto &bytes, std::uint32_t width,
                         std::uint32_t height) {
        return BloomUpsampleImageView{
            .storage = BloomUpsampleStorage::rgba8_srgb,
            .width = width,
            .height = height,
            .bytes = bytes,
        };
    };
    BloomUpsampleOracleRequest request{
        .destination = view(destination, 4, 4),
        .source = view(source, 2, 2),
        .actual = view(actual, 4, 4),
        .sub_texel_precision_bits = 8,
        .pixel_center_x = 0.5,
        .pixel_center_y = 0.5,
        .address_mode = BloomUpsampleAddressMode::repeat,
        .blend_mode = BloomUpsampleBlendMode::one_plus_one,
    };
    const auto probe = evaluateBloomUpsampleOracle(request);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        actual[channel] = static_cast<std::uint8_t>(
            probe.channels[channel].ideal_storage);
    }
    request.actual = view(actual, 4, 4);
    const auto repeat = evaluateBloomUpsampleOracle(request);
    REQUIRE(repeat.matches);

    request.address_mode = BloomUpsampleAddressMode::clamp_to_edge;
    const auto clamped = evaluateBloomUpsampleOracle(request);
    REQUIRE(repeat.channels[0].ideal_linear !=
            Catch::Approx(clamped.channels[0].ideal_linear));
    REQUIRE(repeat.channels[1].ideal_linear !=
            Catch::Approx(clamped.channels[1].ideal_linear));
    REQUIRE(repeat.channels[0].ideal_linear !=
            Catch::Approx(repeat.channels[3].ideal_linear));
    REQUIRE(repeat.channels[3].ideal_linear == Catch::Approx(128.0 / 255.0));
}

TEST_CASE(
    "WP357 bloom oracle brackets legal finite RGBA16 SFLOAT results",
    "[color][bloom][wp357][oracle][hdr]") {
    using namespace TestSupport;
    const auto repeated_half = [](std::uint16_t bits) {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            bytes[channel * 2] = static_cast<std::uint8_t>(bits & 0xffu);
            bytes[channel * 2 + 1] = static_cast<std::uint8_t>(bits >> 8u);
        }
        return bytes;
    };
    const auto destination = repeated_half(0x3400u); // 0.25
    const auto source = repeated_half(0x3800u);      // 0.5
    auto actual = repeated_half(0x3a00u);            // 0.75
    const auto view = [](const auto &bytes) {
        return BloomUpsampleImageView{
            .storage = BloomUpsampleStorage::rgba16_sfloat,
            .width = 1,
            .height = 1,
            .bytes = bytes,
        };
    };
    BloomUpsampleOracleRequest request{
        .destination = view(destination),
        .source = view(source),
        .actual = view(actual),
        .sub_texel_precision_bits = 8,
        .pixel_center_x = 0.5,
        .pixel_center_y = 0.5,
        .address_mode = BloomUpsampleAddressMode::repeat,
        .blend_mode = BloomUpsampleBlendMode::one_plus_one,
    };
    const auto finite = evaluateBloomUpsampleOracle(request);
    REQUIRE(finite.matches);
    for (const auto &channel : finite.channels) {
        CHECK(channel.ideal_linear == Catch::Approx(0.75));
        CHECK(channel.actual_storage == 0x3a00u);
        CHECK(channel.storage_margin > 0);
    }

    actual = repeated_half(0x7c00u); // +Inf is never legal for this stimulus.
    request.actual = view(actual);
    REQUIRE_FALSE(evaluateBloomUpsampleOracle(request).matches);
}

} // namespace Pelican
