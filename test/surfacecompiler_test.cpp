#include "../src/core/shader/surfacecompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/project/materialformat.hpp"
#include "../src/project/materiallowering.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
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

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
           "surface_format";
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

SurfaceFormatDocument engineLightingSurface(std::string_view resource_name) {
    const auto source = std::string{"//! pelican.surface v1\n//! language: glsl\n\n"} +
                        engineResourceOrThrow(resource_name);
    return parseSurfaceFormat(source, std::string{"engine://"} + std::string{resource_name});
}

void requireCompiled(const SurfaceCompileResult &result) {
    INFO("vertex log: " << result.vertex.log);
    INFO("fragment log: " << result.fragment.log);
    REQUIRE(result.vertex.ok);
    REQUIRE(result.fragment.ok);
    REQUIRE_FALSE(result.vertex.spirv.empty());
    REQUIRE_FALSE(result.fragment.spirv.empty());
}

} // namespace

TEST_CASE("surface source composition compiles main depth and velocity variants",
          "[surface-compiler]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto source = readText(fixtureRoot() / "valid" / "wp78.surface");
    const auto surface = parseSurfaceFormat(source, "wp78.surface");
    ShaderCompiler compiler;
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface", SurfacePass::main));
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface", SurfacePass::depth));
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface", SurfacePass::velocity));
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface", SurfacePass::depth,
                                          {"PELICAN_FEATURE_SHADOW"}));

    const auto depth = composeSurfaceShaders(surface, "wp78.surface", SurfacePass::depth);
    REQUIRE(std::find(depth.defines.begin(), depth.defines.end(), "PELICAN_PASS_DEPTH") !=
            depth.defines.end());
    REQUIRE(std::find(depth.defines.begin(), depth.defines.end(),
                      "PELICAN_HAS_VERTEX_DISPLACE_V1") != depth.defines.end());
    const auto shadow_depth = composeSurfaceShaders(surface, "wp78.surface", SurfacePass::depth,
                                                    {"PELICAN_FEATURE_SHADOW"});
    REQUIRE(std::find(shadow_depth.defines.begin(), shadow_depth.defines.end(),
                      "PELICAN_FEATURE_SHADOW") != shadow_depth.defines.end());
#endif
}

TEST_CASE("standard and toon lighting dogfood only the public surface library",
          "[surface-compiler][dogfood]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    for (const auto resource : {"shaders/material/standard_lighting.glsl",
                                "shaders/material/toon_lighting.glsl"}) {
        const auto surface = engineLightingSurface(resource);
        REQUIRE(surface.hooks.lighting_v1);
        requireCompiled(compileSurfaceShaders(compiler, surface,
                                              std::string{"engine://"} + resource));
    }
#endif
}

TEST_CASE("surface compile diagnostics report the authored snippet line",
          "[surface-compiler][diagnostics]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto path = fixtureRoot() / "invalid" / "compile_line.surface";
    const auto surface = parseSurfaceFormat(readText(path), "compile_line.surface");
    ShaderCompiler compiler;
    const auto result = compileSurfaceShaders(compiler, surface, "compile_line.surface");
    REQUIRE_FALSE(result.fragment.ok);
    REQUIRE_THAT(result.fragment.log,
                 Catch::Matchers::ContainsSubstring("compile_line.surface:6"));
    REQUIRE_THAT(result.fragment.log,
                 Catch::Matchers::ContainsSubstring("not_a_declared_symbol"));
#endif
}

TEST_CASE("M3a source composition rejects non-GLSL surfaces by name", "[surface-compiler]") {
    const auto source = readText(fixtureRoot() / "valid" / "minimal_hlsl.surface");
    const auto surface = parseSurfaceFormat(source, "minimal_hlsl.surface");
    REQUIRE_THROWS_WITH(composeSurfaceShaders(surface, "minimal_hlsl.surface"),
                        Catch::Matchers::ContainsSubstring("minimal_hlsl.surface") &&
                            Catch::Matchers::ContainsSubstring("GLSL only"));
}

TEST_CASE("example material is a one-line B surface reference that lowers and compiles",
          "[surface-compiler][example]") {
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example";
    const auto surface_source = readText(root / "shaders" / "toon.surface");
    const auto surface = parseSurfaceFormat(surface_source,
                                            "project://shaders/toon.surface");
    MaterialSurfaceCatalog catalog;
    catalog.emplace("project://shaders/toon.surface", surface);
    const auto material_json = nlohmann::json::parse(
        readText(root / "materials" / "toon.material.json"));
    const auto document = parseMaterialFormatJson(material_json, catalog);
    REQUIRE(document.materials.size() == 1);
    REQUIRE(document.materials.front().surface == "project://shaders/toon.surface");
    const auto lowered = lowerMaterial(document.materials.front(), surface);
    REQUIRE(lowered.hooks.surface_v1);
    REQUIRE(lowered.hooks.lighting_v1);
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    requireCompiled(compileSurfaceShaders(compiler, surface,
                                          "project://shaders/toon.surface"));
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto bundles = library.loadFromSurface(surface, "project://shaders/toon.surface");
    REQUIRE_FALSE(library.get(bundles.vertex).reflection.vertex_inputs.empty());
    REQUIRE_FALSE(library.get(bundles.fragment).reflection.bindings.empty());
    REQUIRE(std::find(library.get(bundles.fragment).defines.begin(),
                      library.get(bundles.fragment).defines.end(),
                      "PELICAN_HAS_LIGHTING_V1") !=
            library.get(bundles.fragment).defines.end());
#endif
}

TEST_CASE("unchanged example toon surface compiles all skinned template variants",
          "[surface-compiler][skeletal]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example" /
                      "shaders" / "toon.surface";
    const auto authored = readText(path);
    const auto surface = parseSurfaceFormat(authored, "project://shaders/toon.surface");
    REQUIRE(authored.substr(surface.code_offset) == surface.code);
    ShaderCompiler compiler;
    for (const auto pass : {SurfacePass::main, SurfacePass::depth, SurfacePass::velocity}) {
        const auto composition = composeSurfaceShaders(surface, "project://shaders/toon.surface", pass,
                                                       {"PELICAN_SKINNED"});
        REQUIRE(std::find(composition.defines.begin(), composition.defines.end(), "PELICAN_SKINNED") !=
                composition.defines.end());
        requireCompiled(compileSurfaceShaders(compiler, surface, "project://shaders/toon.surface", pass,
                                              {"PELICAN_SKINNED"}));
    }
#endif
}

} // namespace Pelican
