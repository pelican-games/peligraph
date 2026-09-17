#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE(
    "WP357a bloom stages and shipping Final composite satisfy the physical oracle",
    "[golden][headless][wp357][wp357a][bloom][oracle]") {
    GoldenHarness::runBloomUpsampleOracle();
}

TEST_CASE(
    "P-2a-4a a pelican.graph root draws the legacy frame and its derivative still changes it",
    "[golden][headless][p2a4a][graph-root][bloom]") {
    GoldenHarness::runGraphRootBloom();
}

TEST_CASE(
    "P-2a-3 an imported render component draws the frame its exporting project drew",
    "[golden][headless][p2a3][render-component][bloom]") {
    GoldenHarness::runImportedRenderComponent();
}

TEST_CASE(
    "WP357 attachment Load differs from explicit zero Clear in a minimal graph",
    "[golden][headless][wp357][bloom][load]") {
    GoldenHarness::runBloomLoadSemantics();
}

TEST_CASE(
    "P-2a-1c bloom parameters reach the shader and survive materialization",
    "[golden][headless][p2a1c][bloom][feature-parameters]") {
    GoldenHarness::runBloomFeatureParameters();
}

} // namespace Pelican
