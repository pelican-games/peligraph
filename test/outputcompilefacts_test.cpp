#include "../src/core/vkcore/outputcompilefacts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <vector>

using namespace Pelican;

namespace {

OutputCompileFacts facts() {
    return OutputCompileFacts{
        .target_kind = OutputTargetKind::window,
        .extent = vk::Extent2D{1920, 1080},
        .color_format = vk::Format::eB8G8R8A8Srgb,
        .color_space =
            vk::ColorSpaceKHR::eSrgbNonlinear,
        .encoding_path =
            OutputEncodingPath::srgb_hardware,
        .selected_usage =
            vk::ImageUsageFlagBits::eColorAttachment |
            vk::ImageUsageFlagBits::eTransferSrc,
        .capture_available = true,
        .surface_transform =
            vk::SurfaceTransformFlagBitsKHR::eIdentity,
        .graphics_queue_family = 2,
        .presentation_queue_family = 3,
    };
}

} // namespace

TEST_CASE(
    "WP215 output compile facts have a deterministic canonical fingerprint",
    "[wp215][output][fingerprint]") {
    const auto value = facts();
    const std::vector<std::uint8_t> expected{
        1, 0, 0, 0, 0, 0, 0, 0,
        128, 7, 0, 0, 56, 4, 0, 0,
        50, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 17, 0, 0, 0,
        1, 0, 0, 0, 1, 0, 0, 0,
        2, 0, 0, 0, 3, 0, 0, 0,
    };
    REQUIRE(canonicalOutputCompileFacts(value) ==
            expected);
    REQUIRE(outputCompileFactsFingerprint(value) ==
            0x4a5ba4a7900a262fULL);
}

TEST_CASE(
    "WP215 every output compiler field participates in the fingerprint",
    "[wp215][output][fingerprint][contract]") {
    const auto original = facts();
    const auto fingerprint =
        outputCompileFactsFingerprint(original);
    const std::vector<
        std::function<void(OutputCompileFacts &)>>
        mutations{
            [](auto &value) {
                value.target_kind =
                    OutputTargetKind::offscreen;
            },
            [](auto &value) { ++value.extent.width; },
            [](auto &value) { ++value.extent.height; },
            [](auto &value) {
                value.color_format =
                    vk::Format::eB8G8R8A8Unorm;
            },
            [](auto &value) {
                value.color_space =
                    vk::ColorSpaceKHR::
                        eDisplayP3NonlinearEXT;
            },
            [](auto &value) {
                value.encoding_path =
                    OutputEncodingPath::
                        srgb_shader_unorm;
            },
            [](auto &value) {
                value.selected_usage |=
                    vk::ImageUsageFlagBits::
                        eTransferDst;
            },
            [](auto &value) {
                value.capture_available = false;
            },
            [](auto &value) {
                value.surface_transform =
                    vk::SurfaceTransformFlagBitsKHR::
                        eRotate90;
            },
            [](auto &value) {
                ++value.graphics_queue_family;
            },
            [](auto &value) {
                ++value.presentation_queue_family;
            },
        };

    for (const auto &mutate : mutations) {
        auto changed = original;
        mutate(changed);
        REQUIRE(
            outputCompileFactsFingerprint(changed) !=
            fingerprint);
    }
}

TEST_CASE(
    "WP215 typed output encoding is stringified only at display boundaries",
    "[wp215][output][encoding]") {
    REQUIRE(std::string{outputEncodingPathName(
                OutputEncodingPath::srgb_hardware)} ==
            "srgb_hardware");
    REQUIRE(std::string{outputEncodingPathName(
                OutputEncodingPath::srgb_shader_unorm)} ==
            "srgb_shader_unorm");
    REQUIRE(std::string{outputEncodingPathRpcName(
                OutputEncodingPath::srgb_hardware)} ==
            "srgb");
    REQUIRE(std::string{outputEncodingPathRpcName(
                OutputEncodingPath::srgb_shader_unorm)} ==
            "unorm_fallback");
}

TEST_CASE(
    "WP216 WSI-only configuration has an independent complete fingerprint",
    "[wp216][output][wsi][fingerprint]") {
    const WsiPresentConfiguration original{
        .present_mode =
            vk::PresentModeKHR::eMailbox,
        .image_count = 3,
        .composite_alpha =
            vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .clipped = true,
    };
    const auto fingerprint =
        wsiPresentConfigurationFingerprint(original);
    CHECK(fingerprint ==
          wsiPresentConfigurationFingerprint(original));

    auto changed = original;
    changed.present_mode =
        vk::PresentModeKHR::eFifo;
    CHECK(wsiPresentConfigurationFingerprint(changed) !=
          fingerprint);
    changed = original;
    ++changed.image_count;
    CHECK(wsiPresentConfigurationFingerprint(changed) !=
          fingerprint);
    changed = original;
    changed.composite_alpha =
        vk::CompositeAlphaFlagBitsKHR::
            ePreMultiplied;
    CHECK(wsiPresentConfigurationFingerprint(changed) !=
          fingerprint);
    changed = original;
    changed.clipped = false;
    CHECK(wsiPresentConfigurationFingerprint(changed) !=
          fingerprint);
}
