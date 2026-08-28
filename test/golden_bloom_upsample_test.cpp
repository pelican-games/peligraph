#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE(
    "WP357a bloom stages and shipping Final composite satisfy the physical oracle",
    "[golden][headless][wp357][wp357a][bloom][oracle]") {
    GoldenHarness::runBloomUpsampleOracle();
}

TEST_CASE(
    "WP357 attachment Load differs from explicit zero Clear in a minimal graph",
    "[golden][headless][wp357][bloom][load]") {
    GoldenHarness::runBloomLoadSemantics();
}

} // namespace Pelican
