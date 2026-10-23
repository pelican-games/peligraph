#include "render_coverage_test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Pelican::TestSupport;

TEST_CASE("CB1 coverage oracle thresholds bbox and empty images", "[cb1][coverage][cpu]") {
    std::vector<float> depth(2000, 1.0F);
    for (int y = 2; y <= 7; ++y) for (int x = 3; x <= 190; ++x) depth[y * 200 + x] = 0.5F;
    const auto a = depthCoverage(depth, 200, 10);
    CHECK(a.bbox() == std::array<int, 4>{3, 2, 190, 7});
    CHECK(compareCoverage(a, a).equivalent());
    auto b = a;
    b.pixels[4 * 200 + 20] = b.pixels[4 * 200 + 21] = 0;
    CHECK(compareCoverage(a, b).equivalent()); // exactly 0.1 percent
    b.pixels[4 * 200 + 22] = 0;
    CHECK_FALSE(compareCoverage(a, b).within_tolerance);
    CHECK_FALSE(compareCoverage(a, b).equivalent());
    b = a; b.pixels[4 * 200 + 191] = 1;
    CHECK(compareCoverage(a, b).within_tolerance);
    CHECK_FALSE(compareCoverage(a, b).equal_bbox);
    CHECK_FALSE(compareCoverage(a, b).equivalent());
    b = a; b.pixels[4 * 200 + 192] = 1;
    CHECK_FALSE(compareCoverage(a, b).within_one_pixel);
    b = a; std::fill(b.pixels.begin(), b.pixels.end(), 0);
    CHECK_FALSE(compareCoverage(b, b).equivalent());
    CHECK_FALSE(compareCoverage(a, b).equivalent());
    CHECK(unionCoverage(a, b).pixels == a.pixels);
    b.width = 100; b.height = 20;
    CHECK_THROWS_AS(compareCoverage(a, b), std::invalid_argument);
    CHECK_THROWS_AS(depthCoverage(depth, 20, 20), std::invalid_argument);
    depth[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS(depthCoverage(depth, 200, 10), std::invalid_argument);
}

TEST_CASE("CB1 velocity coverage uses both components and ignores signed zero", "[cb1][coverage][cpu]") {
    const std::array<std::uint16_t, 8> rg{0, 0x8000, 0x3c00, 0, 0, 0xbc00, 1, 0};
    CHECK(velocityCoverage(rg, 2, 2).pixels == std::vector<std::uint8_t>{0, 1, 1, 1});
    const std::array<std::uint16_t, 2> invalid{0x7c00, 0};
    CHECK_THROWS_AS(velocityCoverage(invalid, 1, 1), std::invalid_argument);
}
