#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE(
    "WP338 flat SSAO output is byte-identical to the pre-migration image",
    "[golden][headless][wp338][ssao][flat]") {
    GoldenHarness::runSsaoFlatGolden();
}

TEST_CASE(
    "WP338 sequential XR SSAO output is byte-identical to the pre-migration image",
    "[golden][headless][wp338][ssao][xr][sequential]") {
    GoldenHarness::runSsaoXrSequentialGolden();
}

TEST_CASE(
    "WP338 multiview XR SSAO output is byte-identical to the pre-migration image",
    "[golden][headless][wp338][ssao][xr][multiview]") {
    GoldenHarness::runSsaoXrMultiviewGolden();
}

TEST_CASE(
    "WP338 cube SSAO output is byte-identical on all six faces",
    "[golden][headless][wp338][ssao][cube]") {
    GoldenHarness::runSsaoCubeGolden();
}

TEST_CASE(
    "WP338 planar SSAO output is byte-identical in both views",
    "[golden][headless][wp338][ssao][planar]") {
    GoldenHarness::runSsaoPlanarGolden();
}

} // namespace Pelican
