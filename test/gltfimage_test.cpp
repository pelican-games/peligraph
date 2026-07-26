#include "../src/core/model/gltfimage.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>

namespace Pelican::GltfInternal {
namespace {

constexpr std::array<std::uint8_t, 74> rgba16Png{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00,
    0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x10, 0x06, 0x00, 0x00, 0x00, 0x4f,
    0x85, 0x18, 0xca, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x9c, 0x63, 0x10, 0x32, 0x59, 0x7d, 0xf6, 0xff,
    0xff, 0x06, 0x06, 0x00, 0x12, 0x01, 0x04, 0x3d, 0x28, 0x51,
    0x37, 0x05, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0xae, 0x42, 0x60, 0x82,
};

} // namespace

TEST_CASE("16-bit glTF PNG is converted before the RGBA8 upload boundary",
          "[gltf][image]") {
    tinygltf::Model model;
    model.images.resize(1);
    EncodedImages encoded(1);
    encoded.front() = EncodedImage{
        .bytes = {rgba16Png.begin(), rgba16Png.end()},
        .requested_width = 1,
        .requested_height = 1,
    };

    decodeImagesInParallel(model, encoded);

    const auto &image = model.images.front();
    REQUIRE(image.width == 1);
    REQUIRE(image.height == 1);
    REQUIRE(image.component == 4);
    REQUIRE(image.bits == 8);
    REQUIRE(image.pixel_type ==
            TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE);
    REQUIRE(image.image ==
            std::vector<unsigned char>{0x12, 0xab, 0xff, 0x80});
    REQUIRE_NOTHROW(
        validateRgba8Image(image, "rgba16.png"));
}

TEST_CASE("RGBA8 upload validation rejects metadata and byte-size drift",
          "[gltf][image]") {
    tinygltf::Image image;
    image.width = 1;
    image.height = 1;
    image.component = 4;
    image.bits = 8;
    image.pixel_type =
        TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image = {1, 2, 3, 4};

    REQUIRE_NOTHROW(validateRgba8Image(image, "valid.png"));

    image.bits = 16;
    REQUIRE_THROWS_WITH(
        validateRgba8Image(image, "wrong-bits.png"),
        Catch::Matchers::ContainsSubstring(
            "required RGBA8 unsigned-byte format"));

    image.bits = 8;
    image.image.pop_back();
    REQUIRE_THROWS_WITH(
        validateRgba8Image(image, "short.png"),
        Catch::Matchers::ContainsSubstring(
            "RGBA8 byte size mismatch"));
}

} // namespace Pelican::GltfInternal
