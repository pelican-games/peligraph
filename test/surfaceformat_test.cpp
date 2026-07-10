#include "../src/project/surfaceformat.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
    REQUIRE(contains(document.code, "pelican_surface"));
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
    REQUIRE(document.code.starts_with("\nvoid pelican_surface"));
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

} // namespace Pelican
