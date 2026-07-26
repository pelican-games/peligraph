#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("golden image cases match expected output", "[golden][headless]") {
    GoldenHarness::runGoldenImages();
}

TEST_CASE("B-layer directional shadow is visible and copied features are equivalent",
          "[golden][headless][wp205]") {
    GoldenHarness::runBLayerShadowEquivalence();
}

TEST_CASE("golden final RGBA8 bytes match the WP74 C1b baseline hashes",
          "[golden][headless][byte-exact]") {
    GoldenHarness::runRgba8Hashes();
}

} // namespace Pelican
