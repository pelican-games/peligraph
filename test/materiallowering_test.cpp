#include "../src/project/materiallowering.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

std::string readFixture() {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
                      "surface_format" / "valid" / "wp76.surface";
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

template <typename T>
T readAt(const std::vector<std::byte> &bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

} // namespace

TEST_CASE("surface values use declaration-order std140 offsets", "[material-lowering]") {
    const auto source = readFixture();
    const auto surface = parseSurfaceFormat(source, "wp76.surface");
    REQUIRE(source.substr(surface.code_offset) == surface.code);

    const auto layout = makeSurfaceStd140Layout(surface);
    REQUIRE(layout.size == 64);
    REQUIRE(layout.members.size() == 6);
    REQUIRE(layout.members[0].offset == 0);
    REQUIRE(layout.members[1].offset == 8);
    REQUIRE(layout.members[2].offset == 16);
    REQUIRE(layout.members[3].offset == 28);
    REQUIRE(layout.members[4].offset == 32);
    REQUIRE(layout.members[5].offset == 48);

    const auto bytes = bindSurfaceValues(surface);
    REQUIRE(readAt<float>(bytes, 0) == Catch::Approx(2.0f));
    REQUIRE(readAt<float>(bytes, 28) == Catch::Approx(8.0f));
    REQUIRE(readAt<std::int32_t>(bytes, 32) == 9);
    REQUIRE(readAt<float>(bytes, 48) == Catch::Approx(0.21404114f));
    REQUIRE(readAt<float>(bytes, 52) == Catch::Approx(0.05087609f));
    REQUIRE(readAt<float>(bytes, 56) == Catch::Approx(1.0f));
    REQUIRE(readAt<float>(bytes, 60) == Catch::Approx(0.75f));
}

TEST_CASE("material overrides bind by name without changing declaration layout",
          "[material-lowering]") {
    const auto surface = parseSurfaceFormat(readFixture(), "wp76.surface");
    MaterialDefinition material;
    material.name = "named_override";
    material.surface = "project://wp76.surface";
    SurfaceParamValue scalar;
    scalar.type = SurfaceParamType::floating;
    scalar.values[0] = 42.0;
    material.values.push_back(MaterialValue{"packed_after_vec3", scalar});

    const auto lowered = lowerMaterial(material, surface);
    REQUIRE(readAt<float>(lowered.values, 28) == Catch::Approx(42.0f));
    REQUIRE(readAt<float>(lowered.values, 0) == Catch::Approx(2.0f));
    REQUIRE(lowered.textures.size() == 3);
    REQUIRE(lowered.textures[0].binding == 7);
    REQUIRE(lowered.textures[0].view == LoweredTextureView::srgb);
    REQUIRE(lowered.textures[1].binding == 8);
    REQUIRE(lowered.textures[1].view == LoweredTextureView::unorm);
    REQUIRE(lowered.textures[1].missing_default == MaterialDummyTexture::flat_normal);
    REQUIRE(lowered.textures[2].missing_default == MaterialDummyTexture::black);
    REQUIRE(lowered.render_state.blend == SurfaceBlendMode::additive);
    REQUIRE(lowered.render_state.cull == SurfaceCullMode::none);
    REQUIRE(lowered.render_state.depth_test);
    REQUIRE_FALSE(lowered.render_state.depth_write);
    REQUIRE(lowered.render_state.depth_compare == SurfaceDepthCompare::greater_equal);
}

TEST_CASE("type color supports explicit linear encoding without decoding alpha",
          "[material-lowering]") {
    const std::string source =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! params:\n"
        "//!   - { name: tint, type: color, default: [0.5, 0.25, 1.0, 0.4], encoding: linear }\n"
        "\nvoid pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) {}\n";
    const auto surface = parseSurfaceFormat(source, "linear_color.surface");
    REQUIRE(source.substr(surface.code_offset) == surface.code);
    const auto values = bindSurfaceValues(surface);
    REQUIRE(readAt<float>(values, 0) == Catch::Approx(0.5f));
    REQUIRE(readAt<float>(values, 4) == Catch::Approx(0.25f));
    REQUIRE(readAt<float>(values, 8) == Catch::Approx(1.0f));
    REQUIRE(readAt<float>(values, 12) == Catch::Approx(0.4f));
}

TEST_CASE("lowering capability errors identify the material and requested feature",
          "[material-lowering]") {
    const auto surface = parseSurfaceFormat(readFixture(), "wp76.surface");
    MaterialDefinition material;
    material.name = "too_many_slots";
    material.surface = "project://wp76.surface";
    MaterialLoweringCapabilities capabilities;
    capabilities.max_custom_textures = 2;

    REQUIRE_THROWS_WITH(lowerMaterial(material, surface, capabilities),
                        Catch::Matchers::ContainsSubstring("too_many_slots") &&
                            Catch::Matchers::ContainsSubstring("custom textures"));
}

TEST_CASE("dump includes defines bindings values and render state", "[material-lowering]") {
    const auto surface = parseSurfaceFormat(readFixture(), "wp76.surface");
    const auto text = dumpLoweredMaterial(lowerSurfaceDefaults(surface, "wp76.surface"));
    REQUIRE(text.find("defines: []") != std::string::npos);
    REQUIRE(text.find("binding=6 type=storage_buffer name=MaterialBuffer") != std::string::npos);
    REQUIRE(text.find("binding=7") != std::string::npos);
    REQUIRE(text.find("tint type=color offset=48") != std::string::npos);
    REQUIRE(text.find("blend=additive cull=none") != std::string::npos);
}

TEST_CASE("B surface lowering has a byte-stable public C description golden",
          "[material-lowering][golden]") {
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto source = readText(root / "test" / "fixtures" / "surface_format" / "valid" /
                                 "wp78.surface");
    const auto surface = parseSurfaceFormat(source, "wp78.surface");
    const auto actual = dumpLoweredMaterial(lowerSurfaceDefaults(surface, "wp78.surface"));
    const auto expected = readText(root / "test" / "fixtures" / "material_lowering" /
                                   "wp78_dump.txt");
    REQUIRE(actual == expected);
}

} // namespace Pelican
