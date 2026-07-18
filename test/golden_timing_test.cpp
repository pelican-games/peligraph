#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("GPU timing on and off preserve bytes and publish ordered node identity",
          "[wp143][golden][gpu-timing][byte-exact]") {
    GoldenHarness::runGpuTimingIdentity();
}

TEST_CASE("GPU timing reuses one in-flight query ring and caps history at 120 frames",
          "[wp143][gpu-timing][ring][headless]") {
    GoldenHarness::runGpuTimingRing();
}

TEST_CASE("GPU timing records compute body separately from incoming barriers",
          "[wp143][gpu-timing][compute][headless]") {
    GoldenHarness::runGpuTimingCompute();
}

TEST_CASE("GPU timing marks sprite anchor body as supported work",
          "[wp143][gpu-timing][anchor][sprite][headless]") {
    GoldenHarness::runGpuTimingSprite();
}

} // namespace Pelican
