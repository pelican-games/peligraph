#include "../src/project/surfaceformat.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "surface_format";
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

std::filesystem::path openPbrSurfaceRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src" / "core" / "resources" /
           "surfaces" / "openpbr";
}

void requireSameOpenPbrDeclarations(const SurfaceFormatDocument &expected,
                                    const SurfaceFormatDocument &actual) {
    REQUIRE(actual.params.size() == expected.params.size());
    for (std::size_t index = 0; index < expected.params.size(); ++index) {
        const auto &left = expected.params[index];
        const auto &right = actual.params[index];
        REQUIRE(right.name == left.name);
        REQUIRE(right.type == left.type);
        REQUIRE(right.default_value.type == left.default_value.type);
        REQUIRE(right.default_value.values == left.default_value.values);
        REQUIRE(right.default_value.integer_value == left.default_value.integer_value);
        REQUIRE(right.default_value.component_count == left.default_value.component_count);
        REQUIRE(right.min == left.min);
        REQUIRE(right.max == left.max);
        REQUIRE(right.encoding == left.encoding);
    }
    REQUIRE(actual.textures.size() == expected.textures.size());
    for (std::size_t index = 0; index < expected.textures.size(); ++index) {
        const auto &left = expected.textures[index];
        const auto &right = actual.textures[index];
        REQUIRE(right.name == left.name);
        REQUIRE(right.default_reference == left.default_reference);
        REQUIRE(right.color_space == left.color_space);
        REQUIRE(right.role == left.role);
    }
}

} // namespace

TEST_CASE("surface format accepts minimal GLSL, HLSL, and Slang headers", "[surface-format]") {
    const auto cases = GENERATE(table<std::string, SurfaceLanguage>({
        {"valid/minimal.surface", SurfaceLanguage::glsl},
        {"valid/minimal_hlsl.surface", SurfaceLanguage::hlsl},
        {"valid/minimal_slang.surface", SurfaceLanguage::slang},
    }));
    const auto &[file, language] = cases;
    const auto source = readText(fixtureRoot() / file);
    const auto document = parseSurfaceFormat(source, file);

    REQUIRE(document.language == language);
    REQUIRE(document.params.empty());
    REQUIRE(document.textures.empty());
    REQUIRE(document.screen_inputs.empty());
    REQUIRE(document.warnings.empty());
    REQUIRE(source.substr(document.code_offset) == document.code);
    REQUIRE(contains(document.code, "pelican_surface_v1"));
}

TEST_CASE("surface format preserves ordered declarations and the code split offset",
          "[surface-format]") {
    const auto source = readText(fixtureRoot() / "valid" / "full.surface");
    const auto document = parseSurfaceFormat(source, "full.surface");

    REQUIRE(document.language == SurfaceLanguage::glsl);
    REQUIRE(document.params.size() == 5);
    REQUIRE(document.params.at(0).name == "flow_speed");
    REQUIRE(document.params.at(1).name == "distortion");
    REQUIRE(document.params.at(2).name == "tint");
    REQUIRE(document.params.at(3).name == "clip_plane");
    REQUIRE(document.params.at(4).name == "layer_count");
    REQUIRE(document.params.at(0).type == SurfaceParamType::floating);
    REQUIRE(document.params.at(0).default_value.values[0] == Catch::Approx(0.35));
    REQUIRE(document.params.at(0).min.value() == Catch::Approx(0.0));
    REQUIRE(document.params.at(0).max.value() == Catch::Approx(2.0));
    REQUIRE(document.params.at(1).type == SurfaceParamType::vec2);
    REQUIRE(document.params.at(2).type == SurfaceParamType::vec3);
    REQUIRE(document.params.at(2).hint == "color");
    REQUIRE(document.params.at(3).type == SurfaceParamType::vec4);
    REQUIRE(document.params.at(4).type == SurfaceParamType::integer);
    REQUIRE(document.params.at(4).default_value.integer_value == 3);

    REQUIRE(document.textures.size() == 1);
    REQUIRE(document.textures.front().name == "flow_map");
    REQUIRE(document.textures.front().default_reference == "engine://textures/flat_gray");
    REQUIRE(document.textures.front().color_space == "linear");
    REQUIRE(document.screen_inputs == std::vector<std::string>{"opaque_color", "opaque_depth"});

    REQUIRE(document.warnings.size() == 3);
    REQUIRE(contains(document.warnings.at(0), "editor_group"));
    REQUIRE(contains(document.warnings.at(1), "sampler"));
    REQUIRE(contains(document.warnings.at(2), "capabilities"));
    REQUIRE(source.substr(document.code_offset) == document.code);
    REQUIRE(document.code.starts_with("\nvoid pelican_surface_v1"));
}

TEST_CASE("surface format errors identify the source and offending declaration",
          "[surface-format]") {
    const auto cases = GENERATE(table<std::string, std::string, std::string>({
        {"invalid/bad_magic.surface", "pelican.surface v1", ""},
        {"invalid/unknown_language.surface", "unknown language", ""},
        {"invalid/missing_type.surface", "explicit type", "flow_speed"},
        {"invalid/missing_default.surface", "requires default", "tint"},
        {"invalid/duplicate_name.surface", "duplicate name", "shared"},
        {"invalid/default_type.surface", "declared type vec3", "tint"},
        {"invalid/bad_texture_default.surface", "project:// or engine://", "flow_map"},
        {"invalid/missing_color_space.surface", "requires color_space", "flow_map"},
        {"invalid/unknown_hook.surface", "unknown pelican_ function", "pelican_surafce_v1"},
        {"invalid/exclusive_terminal.surface", "mutually exclusive terminal hooks", "pelican_lighting_v1"},
        {"invalid/empty_code.surface", "empty code snippet", ""},
        {"invalid/no_hook.surface", "no recognized pelican_*_v1 hook", ""},
    }));
    const auto &[file, expected_error, declaration_name] = cases;
    const auto source = readText(fixtureRoot() / file);

    std::string message;
    try {
        (void)parseSurfaceFormat(source, file);
    } catch (const std::exception &ex) {
        message = ex.what();
    }

    REQUIRE_FALSE(message.empty());
    REQUIRE(contains(message, file));
    REQUIRE(contains(message, expected_error));
    if (!declaration_name.empty()) {
        REQUIRE(contains(message, declaration_name));
    }
}

TEST_CASE("surface format reflects versioned hooks without changing code identity",
          "[surface-format][surface-hooks]") {
    const auto source = readText(fixtureRoot() / "valid" / "wp78.surface");
    const auto document = parseSurfaceFormat(source, "wp78.surface");
    REQUIRE(source.substr(document.code_offset) == document.code);
    REQUIRE(document.code_line == 7);
    REQUIRE(document.hooks.vertex_displace_v1);
    REQUIRE(document.hooks.surface_v1);
    REQUIRE_FALSE(document.hooks.brdf_v1);
    REQUIRE_FALSE(document.hooks.lighting_v1);
    const auto hooks = surfaceHookNames(document.hooks);
    REQUIRE(hooks.size() == 2);
    REQUIRE(static_cast<bool>(hooks[0] == "pelican_vertex_displace_v1"));
    REQUIRE(static_cast<bool>(hooks[1] == "pelican_surface_v1"));
}

TEST_CASE("surface format declares typed material resource ports",
          "[surface-format][resource-port][wp207b]") {
    constexpr std::string_view source = R"surface(//! pelican.surface v1
//! language: glsl
//! resource_ports:
//!   - { name: displacement, kind: buffer, element: vec4, stage: vertex }
//!   - { name: simulation_color, kind: image, stage: fragment }

void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {
    vertex.position += vec3(0.0);
}
)surface";

    const auto document =
        parseSurfaceFormat(source, "resource_ports.surface");
    REQUIRE(document.resource_ports.size() == 2);
    REQUIRE(document.resource_ports[0].name == "displacement");
    REQUIRE(document.resource_ports[0].kind ==
            SurfaceResourcePortKind::buffer);
    REQUIRE(document.resource_ports[0].stage ==
            SurfaceResourcePortStage::vertex);
    REQUIRE(document.resource_ports[0].element ==
            ShaderResourceBufferElement::vec4);
    REQUIRE(document.resource_ports[1].name ==
            "simulation_color");
    REQUIRE(document.resource_ports[1].kind ==
            SurfaceResourcePortKind::image);
    REQUIRE(document.resource_ports[1].stage ==
            SurfaceResourcePortStage::fragment);
}

TEST_CASE("surface buffer resource ports require an explicit element",
          "[surface-format][resource-port][wp207b]") {
    constexpr std::string_view source = R"surface(//! pelican.surface v1
//! language: glsl
//! resource_ports:
//!   - { name: displacement, kind: buffer, stage: vertex }

void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {
    vertex.position += vec3(0.0);
}
)surface";

    REQUIRE_THROWS_WITH(
        parseSurfaceFormat(source, "bad_resource_port.surface"),
        Catch::Matchers::ContainsSubstring(
            "buffer requires explicit element"));
}

TEST_CASE("OpenPBR wrapper-B variants preserve one declaration ABI",
          "[surface-format][openpbr][variants]") {
    struct VariantExpectation {
        const char *file;
        SurfaceBlendMode blend;
        SurfaceCullMode cull;
        bool depth_write;
        int alpha_mode;
        int double_sided;
    };
    constexpr std::array variants{
        VariantExpectation{"opaque_single.surface", SurfaceBlendMode::opaque,
                           SurfaceCullMode::back, true, 0, 0},
        VariantExpectation{"opaque_double.surface", SurfaceBlendMode::opaque,
                           SurfaceCullMode::none, true, 0, 1},
        VariantExpectation{"mask_single.surface", SurfaceBlendMode::opaque,
                           SurfaceCullMode::back, true, 1, 0},
        VariantExpectation{"mask_double.surface", SurfaceBlendMode::opaque,
                           SurfaceCullMode::none, true, 1, 1},
        VariantExpectation{"blend_single.surface", SurfaceBlendMode::blend,
                           SurfaceCullMode::back, false, 2, 0},
        VariantExpectation{"blend_double.surface", SurfaceBlendMode::blend,
                           SurfaceCullMode::none, false, 2, 1},
    };

    const auto baseline_source = readText(openPbrSurfaceRoot() / variants.front().file);
    const auto baseline = parseSurfaceFormat(baseline_source, variants.front().file);
    REQUIRE(baseline.params.size() == 19);
    REQUIRE(baseline.textures.size() == 18);
    REQUIRE(baseline.params.front().name == "base_weight");
    REQUIRE(baseline.params.at(7).name == "specular_ior");
    REQUIRE(baseline.params.at(8).name == "coat_weight");
    REQUIRE(baseline.params.at(13).name == "emission_luminance");
    REQUIRE(baseline.params.at(15).name == "geometry_opacity");
    REQUIRE(baseline.params.back().name == "alpha_cutoff");

    for (const auto &variant : variants) {
        DYNAMIC_SECTION(variant.file) {
            const auto source = readText(openPbrSurfaceRoot() / variant.file);
            const auto document = parseSurfaceFormat(source, variant.file);
            requireSameOpenPbrDeclarations(baseline, document);
            REQUIRE(document.hooks.surface_v1);
            REQUIRE(document.hooks.lighting_v1);
            REQUIRE(document.render_state.blend == variant.blend);
            REQUIRE(document.render_state.cull == variant.cull);
            REQUIRE(document.render_state.depth_test);
            REQUIRE(document.render_state.depth_write == variant.depth_write);
            REQUIRE(document.code.find("#include \"shaders/material/openpbr_lighting.glsl\"") !=
                    std::string::npos);
            REQUIRE(document.code.find("#define PELICAN_OPENPBR_ALPHA_MODE " +
                                       std::to_string(variant.alpha_mode)) != std::string::npos);
            REQUIRE(document.code.find("#define PELICAN_OPENPBR_DOUBLE_SIDED " +
                                       std::to_string(variant.double_sided)) != std::string::npos);
        }
    }
}

} // namespace Pelican
