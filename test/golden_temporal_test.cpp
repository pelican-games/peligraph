#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("jitter-only Halton and equivalent table captures match byte-exactly",
          "[golden][headless][projection-jitter]") {
    GoldenHarness::runProjectionJitterEquivalence();
}

TEST_CASE("jitter preserves culling draw count and shadow-map bytes",
          "[golden][headless][projection-jitter][shadow]") {
    GoldenHarness::runProjectionJitterShadow();
}

TEST_CASE("TAA static accumulation is byte-exact across two independent runs",
          "[golden][headless][taa][determinism]") {
    GoldenHarness::runTaaDeterminism();
}

TEST_CASE("logical frame renders Vulkan-backed stereo views without advancing shared state twice",
          "[wp128][headless][stereo][vulkan]") {
    GoldenHarness::runLogicalFrameStereo();
}

TEST_CASE("OpenXR graph transition excludes TAA and restores flat temporal rendering once",
          "[wp133][openxr][taa][headless][vulkan]") {
    GoldenHarness::runOpenXrTaaTransition();
}

TEST_CASE("velocity feature compiles its standard pass and renders headless",
          "[temporal][velocity][headless]") {
    GoldenHarness::runVelocityFeature();
}

TEST_CASE("set_time automatically resets skinned velocity history",
          "[temporal][velocity][skeletal][headless]") {
    GoldenHarness::runSetTimeSkinnedVelocity();
}

TEST_CASE("morph-only deformation produces velocity and reset zeros it",
          "[wp121][temporal][velocity][morph][headless]") {
    GoldenHarness::runMorphVelocity();
}

} // namespace Pelican
