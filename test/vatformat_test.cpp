#include "../src/core/model/vatformat.hpp"
#include "vat_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <vector>

namespace Pelican {

namespace {

constexpr uint32_t vertexCount = 4;
constexpr uint32_t frameCount = 2;
constexpr size_t vatBytes = vertexCount * frameCount * 4 * sizeof(uint16_t);

std::vector<VatBufferViewInfo> validBufferViews() {
    return {
        VatBufferViewInfo{.index = 0, .buffer = 0, .byte_offset = 0, .byte_length = 48},
        VatBufferViewInfo{.index = 1, .buffer = 0, .byte_offset = 48, .byte_length = vatBytes},
        VatBufferViewInfo{.index = 2, .buffer = 0, .byte_offset = 48 + vatBytes, .byte_length = vatBytes},
    };
}

VatPrimitiveMeta validPrimitiveMeta() {
    return VatPrimitiveMeta{
        .present = true,
        .single_clip_object = true,
        .schema = std::string{"pelican.vat"},
        .version = 1,
        .generator = std::string{"vatformat_test"},
        .fps = 24.0,
        .frame_count = frameCount,
        .vertex_count = vertexCount,
        .bounds_min = std::array<double, 3>{-1.0, -1.0, -1.0},
        .bounds_max = std::array<double, 3>{1.0, 1.0, 1.0},
        .loop = true,
        .position_view = 1,
        .normal_view = 2,
    };
}

} // namespace

TEST_CASE("pelican.vat parser accepts a valid primitive clip", "[vatformat]") {
    const auto parsed = parseVatPrimitiveExtras(validPrimitiveMeta(), vertexCount, validBufferViews());

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->generator == "vatformat_test");
    REQUIRE(parsed->fps == 24.0);
    REQUIRE(parsed->frame_count == frameCount);
    REQUIRE(parsed->vertex_count == vertexCount);
    REQUIRE(parsed->loop);
    REQUIRE(parsed->position_view.index == 1);
    REQUIRE(parsed->position_view.buffer == 0);
    REQUIRE(parsed->position_view.byte_length == vatBytes);
    REQUIRE(parsed->normal_view.has_value());
    REQUIRE(parsed->normal_view->index == 2);
}

TEST_CASE("pelican.vat test fixture generates a small GLB", "[vatformat]") {
    const auto bytes = TestVatFixture::makeTinyVatGlb();

    REQUIRE(bytes.size() > 20);
    REQUIRE(bytes[0] == 'g');
    REQUIRE(bytes[1] == 'l');
    REQUIRE(bytes[2] == 'T');
    REQUIRE(bytes[3] == 'F');
}

TEST_CASE("pelican.vat parser leaves non-VAT primitives untouched", "[vatformat]") {
    const auto parsed = parseVatPrimitiveExtras(VatPrimitiveMeta{}, vertexCount, validBufferViews());
    REQUIRE_FALSE(parsed.has_value());
}

TEST_CASE("pelican.vat parser rejects constraint violations clearly", "[vatformat]") {
    SECTION("multiple clips") {
        auto meta = validPrimitiveMeta();
        meta.single_clip_object = false;

        REQUIRE_THROWS_WITH(parseVatPrimitiveExtras(meta, vertexCount, validBufferViews()),
                            Catch::Matchers::ContainsSubstring("single object clip"));
    }

    SECTION("vertex limit") {
        auto meta = validPrimitiveMeta();
        meta.vertex_count = maxVatVertexCount + 1;

        REQUIRE_THROWS_WITH(parseVatPrimitiveExtras(meta, maxVatVertexCount + 1, validBufferViews()),
                            Catch::Matchers::ContainsSubstring("exceeds max 8192"));
    }

    SECTION("primitive vertex count mismatch") {
        auto meta = validPrimitiveMeta();
        meta.vertex_count = vertexCount - 1;

        REQUIRE_THROWS_WITH(parseVatPrimitiveExtras(meta, vertexCount, validBufferViews()),
                            Catch::Matchers::ContainsSubstring("POSITION count"));
    }

    SECTION("missing position view") {
        auto meta = validPrimitiveMeta();
        meta.position_view = 99;

        REQUIRE_THROWS_WITH(parseVatPrimitiveExtras(meta, vertexCount, validBufferViews()),
                            Catch::Matchers::ContainsSubstring("position_view references missing bufferView"));
    }

    SECTION("undersized normal view") {
        auto views = validBufferViews();
        views[2].byte_length = vatBytes - 1;

        REQUIRE_THROWS_WITH(parseVatPrimitiveExtras(validPrimitiveMeta(), vertexCount, views),
                            Catch::Matchers::ContainsSubstring("normal_view bufferView 2 is too small"));
    }
}

} // namespace Pelican
