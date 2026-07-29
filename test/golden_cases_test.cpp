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

TEST_CASE(
    "planar reflection executes a clipped secondary view family on the GPU",
    "[golden][headless][reflection][view-family]") {
    GoldenHarness::runPlanarReflection();
}

TEST_CASE(
    "GPU-written indexed draw count handles zero max overflow and CPU fallback",
    "[golden][headless][wp210]") {
    GoldenHarness::runGpuDrawIndirect();
}

TEST_CASE(
    "depth pyramid compacts a fixed-state draw segment and preserves CPU fallback",
    "[golden][headless][wp210][occlusion]") {
    GoldenHarness::runGpuOcclusionCulling();
}

TEST_CASE(
    "depth pyramid compacts multiple CPU-bound material state segments",
    "[golden][headless][wp210][occlusion][segments]") {
    GoldenHarness::
        runGpuSegmentedOcclusionCulling();
}

TEST_CASE(
    "per-view GPU culling preserves segmented draws across XR execution modes",
    "[golden][headless][wp210][occlusion][segments][xr][multiview]") {
    GoldenHarness::
        runGpuSegmentedOcclusionXr();
}

TEST_CASE(
    "segmented GPU draw generations hot reload and roll back atomically",
    "[golden][headless][wp210][occlusion][segments][hot-reload][rollback]") {
    GoldenHarness::
        runGpuSegmentedOcclusionHotReload();
    SUCCEED(
        "segmented graph and shader transactions preserved the published frame");
}

TEST_CASE("golden final RGBA8 bytes match the WP74 C1b baseline hashes",
          "[golden][headless][byte-exact]") {
    GoldenHarness::runRgba8Hashes();
}

} // namespace Pelican
