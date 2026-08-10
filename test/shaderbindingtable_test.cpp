#include "../src/core/shader/shaderbindingtable.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace Pelican {

TEST_CASE(
    "SBT packing aligns handle records independently from region bases",
    "[wp285][ray-tracing][sbt]") {
    SECTION("handle size differs from handle alignment") {
        const auto layout = calculateShaderBindingTableLayout(
            24, 32, 32,
            ShaderBindingTableGroupCounts{
                .raygen = 1, .miss = 2, .hit = 3});
        CHECK(layout.raygen.offset == 0);
        CHECK(layout.raygen.stride == 32);
        CHECK(layout.raygen.size == 32);
        CHECK(layout.miss.offset == 32);
        CHECK(layout.miss.stride == 32);
        CHECK(layout.miss.size == 64);
        CHECK(layout.hit.offset == 96);
        CHECK(layout.hit.stride == 32);
        CHECK(layout.hit.size == 96);
        CHECK(layout.total_size == 192);
    }

    SECTION("region base alignment exceeds handle alignment") {
        const auto layout = calculateShaderBindingTableLayout(
            32, 32, 128,
            ShaderBindingTableGroupCounts{
                .raygen = 1, .miss = 2, .hit = 1});
        CHECK(layout.raygen.offset == 0);
        CHECK(layout.raygen.stride == 32);
        CHECK(layout.raygen.size == 32);
        CHECK(layout.miss.offset == 128);
        CHECK(layout.miss.size == 64);
        CHECK(layout.hit.offset == 256);
        CHECK(layout.hit.size == 32);
        CHECK(layout.total_size == 288);
    }
}

TEST_CASE(
    "SBT packing rejects malformed alignment and raygen contracts",
    "[wp285][ray-tracing][sbt]") {
    CHECK_THROWS_WITH(
        calculateShaderBindingTableLayout(
            0, 32, 64, {}),
        Catch::Matchers::ContainsSubstring(
            "pelican.sbt.invalid_alignment@1"));
    CHECK_THROWS_WITH(
        calculateShaderBindingTableLayout(
            32, 32, 64,
            ShaderBindingTableGroupCounts{
                .raygen = 2, .miss = 1, .hit = 1}),
        Catch::Matchers::ContainsSubstring(
            "pelican.sbt.invalid_raygen_group_count@1"));
}

} // namespace Pelican
