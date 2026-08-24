#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE(
    "WP339 flat SSAO blur matches its producer relationship",
    "[golden][headless][wp339][ssao-blur][flat]") {
    GoldenHarness::runSsaoBlurFlatOracle();
}

TEST_CASE(
    "WP339 half-resolution SSAO blur rejects the render-size mutant",
    "[golden][headless][wp339][ssao-blur][half-resolution]") {
    GoldenHarness::runSsaoBlurHalfResolutionOracle();
}

TEST_CASE(
    "WP339 sequential XR SSAO blur matches its producer relationship",
    "[golden][headless][wp339][ssao-blur][xr][sequential]") {
    GoldenHarness::runSsaoBlurXrSequentialOracle();
}

TEST_CASE(
    "WP339 multiview XR SSAO blur uses the array producer relationship",
    "[golden][headless][wp339][ssao-blur][xr][multiview]") {
    GoldenHarness::runSsaoBlurXrMultiviewOracle();
}

TEST_CASE(
    "WP339 cube SSAO blur matches every producer face",
    "[golden][headless][wp339][ssao-blur][cube]") {
    GoldenHarness::runSsaoBlurCubeOracle();
}

TEST_CASE(
    "WP339 planar SSAO blur matches every producer layer",
    "[golden][headless][wp339][ssao-blur][planar]") {
    GoldenHarness::runSsaoBlurPlanarOracle();
}

TEST_CASE(
    "WP339 cross-family SSAO blur consumes producer family layer zero",
    "[golden][headless][wp339][ssao-blur][cross-family]") {
    GoldenHarness::runSsaoBlurCrossFamilyOracle();
}

TEST_CASE(
    "WP339 shipped SSAO producer still feeds the shipped blur",
    "[golden][headless][wp339][ssao-blur][integration]") {
    GoldenHarness::runSsaoBlurIntegrationSmoke();
}

} // namespace Pelican
