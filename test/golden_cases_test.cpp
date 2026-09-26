#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("golden image cases match expected output",
          "[golden][headless][byte-exact][framegraph]") {
    GoldenHarness::runGoldenImages();
}

TEST_CASE(
    "LP-2 hybrid golden cases draw identically with today's steps spelled out",
    "[golden][headless][lp2][draw-loop]") {
    GoldenHarness::runDrawLoopIdentityEquivalence();
}

TEST_CASE(
    "LP-3a hybrid golden cases draw identically with their material passes written in the graph",
    "[golden][headless][lp3a][draw-loop]") {
    GoldenHarness::runDrawLoopDecomposedEquivalence();
}

TEST_CASE(
    "LP-3b decompose_material_pass's rewrite of the hybrid golden cases draws identically",
    "[golden][headless][lp3b][draw-loop]") {
    GoldenHarness::runMaterialPassDecomposeEquivalence();
}

#if PELICAN_WITH_RPC
TEST_CASE(
    "inspector edits unnamed parented runtime objects and changes rendered pixels",
    "[golden][headless][editor][transform][collider][load-scene][wp244]") {
    GoldenHarness::runEditorRuntimeBinding();
}
#endif

TEST_CASE("B-layer directional shadow is visible and copied features are equivalent",
          "[golden][headless][wp205]") {
    GoldenHarness::runBLayerShadowEquivalence();
}

TEST_CASE("LP-Va cascade view_select split matches the shadow feature per layer",
          "[golden][headless][lpva][view-select][shadow]") {
    GoldenHarness::runViewSelectCascadeShadowEquivalence();
}

TEST_CASE("LP-0e a render.shadow_depth graph node in shadow_depth's place matches the feature per layer",
          "[golden][headless][lp0e][shadow]") {
    GoldenHarness::runGraphShadowDepthEquivalence();
}

TEST_CASE(
    "multiple directional lights cast matching forward and deferred shadows",
    "[golden][headless][shadow][wp242a]") {
    GoldenHarness::runMultiLightDirectionalShadows();
}

TEST_CASE(
    "planar reflection executes a clipped secondary view family on the GPU",
    "[golden][headless][reflection][view-family]") {
    GoldenHarness::runPlanarReflection();
}

TEST_CASE(
    "LP-1c the shipped planar mip chain loop fills every mip as the six filter tasks do",
    "[golden][headless][lp1c][reflection][graph-loop]") {
    GoldenHarness::runLp1cPlanarDecomposition();
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

TEST_CASE("Model watch reload restores visible skin and material defaults",
          "[golden][headless][gpu][model-reload][reload-gpu-state]") {
    GoldenHarness::runModelReloadGpuState();
}

TEST_CASE("point and spot shadows darken forward and deferred receiver pixels",
          "[golden][headless][shadow][wp242b]") {
    GoldenHarness::runPunctualShadows();
}

TEST_CASE("shadow PCF bias and strength change point and spot receiver pixels",
          "[golden][headless][shadow][wp242c]") {
    GoldenHarness::runShadowFiltering();
}

} // namespace Pelican
