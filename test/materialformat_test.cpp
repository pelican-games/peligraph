#include "../src/project/materialformat.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "material_format";
}

std::filesystem::path surfaceFixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "surface_format";
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

MaterialSurfaceCatalog surfaceCatalog() {
    const auto source = readText(surfaceFixtureRoot() / "valid" / "full.surface");
    MaterialSurfaceCatalog catalog;
    catalog.emplace("project://shaders/lava.surface", parseSurfaceFormat(source, "lava.surface"));
    return catalog;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

const MaterialValue *findValue(const MaterialDefinition &material, std::string_view name) {
    for (const auto &value : material.values) {
        if (value.name == name) {
            return &value;
        }
    }
    return nullptr;
}

void requireErrorKind(std::string_view message, std::string_view error_kind) {
    if (error_kind == "schema") {
        REQUIRE(contains(message, "schema"));
    } else if (error_kind == "shader_stem") {
        REQUIRE(contains(message, "shader"));
        REQUIRE(contains(message, "extensionless stem"));
    } else if (error_kind == "define") {
        REQUIRE(contains(message, "define"));
    } else if (error_kind == "legacy_declaration") {
        REQUIRE(contains(message, "declarations are not supported"));
        REQUIRE(contains(message, ".surface"));
    } else if (error_kind == "value_type") {
        REQUIRE(contains(message, "value 'tint'"));
        REQUIRE(contains(message, "declared type vec3"));
    } else if (error_kind == "undeclared_value") {
        REQUIRE(contains(message, "missing_param"));
        REQUIRE(contains(message, "not declared"));
    } else if (error_kind == "values") {
        REQUIRE(contains(message, "values"));
        REQUIRE(contains(message, ".surface"));
    } else if (error_kind == "surface_ref") {
        REQUIRE(contains(message, "surface"));
        REQUIRE(contains(message, ".surface"));
    } else if (error_kind == "base") {
        REQUIRE(contains(message, "baseColorFactor"));
    } else if (error_kind == "texture_ref") {
        REQUIRE(contains(message, "project:// or engine://"));
    } else {
        FAIL("unknown material format error_kind: " << error_kind);
    }
}

} // namespace

TEST_CASE("material format fixtures parse and reject expected cases", "[material-format]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");
    const auto surfaces = surfaceCatalog();

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto material_json = readJson(fixtureRoot() / file);
            const auto expected = entry.at("expect").get<std::string>();
            if (expected == "ok") {
                const auto document = parseMaterialFormatJson(material_json, surfaces);
                REQUIRE(document.materials.size() == entry.at("material_count").get<size_t>());
                REQUIRE(document.warnings.size() == entry.at("warning_count").get<size_t>());
            } else {
                const auto error_kind = entry.at("error_kind").get<std::string>();
                std::string message;
                try {
                    (void)parseMaterialFormatJson(material_json, surfaces);
                } catch (const std::exception &ex) {
                    message = ex.what();
                }
                REQUIRE_FALSE(message.empty());
                const bool schema_error = error_kind == "schema";
                REQUIRE((contains(message, "lava") || schema_error));
                requireErrorKind(message, error_kind);
            }
        }
    }
}

TEST_CASE("minimal material format applies glTF-compatible defaults", "[material-format]") {
    const auto document = parseMaterialFormatJson(readJson(fixtureRoot() / "valid" / "minimal.json"));
    REQUIRE(document.warnings.empty());
    REQUIRE(document.materials.size() == 1);

    const auto &material = document.materials.front();
    REQUIRE(material.name == "mat_default");
    REQUIRE_FALSE(material.shader.has_value());
    REQUIRE_FALSE(material.surface.has_value());
    REQUIRE(material.defines.empty());
    REQUIRE(material.values.empty());
    REQUIRE(material.base.base_color_factor == std::array<double, 4>{1.0, 1.0, 1.0, 1.0});
    REQUIRE(material.base.metallic_factor == Catch::Approx(1.0));
    REQUIRE(material.base.roughness_factor == Catch::Approx(1.0));
    REQUIRE(material.base.emissive_factor == std::array<double, 3>{0.0, 0.0, 0.0});
}

TEST_CASE("full material format round-trips into typed fields and warnings", "[material-format]") {
    const auto document =
        parseMaterialFormatJson(readJson(fixtureRoot() / "valid" / "full.json"), surfaceCatalog());
    REQUIRE(document.materials.size() == 1);
    REQUIRE(document.warnings.size() == 3);
    REQUIRE(contains(document.warnings.at(0), "generator"));
    REQUIRE(contains(document.warnings.at(1), "editor"));
    REQUIRE(contains(document.warnings.at(2), "clearcoatFactor"));

    const auto &material = document.materials.front();
    REQUIRE(material.name == "lava");
    REQUIRE_FALSE(material.shader.has_value());
    REQUIRE(material.surface == "project://shaders/lava.surface");
    REQUIRE(material.defines == std::vector<std::string>{"LAVA_FLOW", "USE_EMISSIVE"});
    REQUIRE(material.base.base_color_factor == std::array<double, 4>{1.0, 0.8, 0.6, 1.0});
    REQUIRE(material.base.base_color_texture == "project://textures/lava_albedo.png");
    REQUIRE(material.base.metallic_factor == Catch::Approx(0.0));
    REQUIRE(material.base.roughness_factor == Catch::Approx(0.8));
    REQUIRE(material.base.metallic_roughness_texture == "engine://default_mr.png");
    REQUIRE(material.base.normal_texture == "project://textures/lava_normal.png");
    REQUIRE(material.base.emissive_factor == std::array<double, 3>{2.0, 0.5, 0.1});
    REQUIRE(material.base.emissive_texture == "project://textures/lava_emissive.png");

    const auto *flow_speed = findValue(material, "flow_speed");
    REQUIRE(flow_speed != nullptr);
    REQUIRE(flow_speed->value.type == SurfaceParamType::floating);
    REQUIRE(flow_speed->value.values[0] == Catch::Approx(0.35));

    const auto *distortion = findValue(material, "distortion");
    REQUIRE(distortion != nullptr);
    REQUIRE(distortion->value.type == SurfaceParamType::vec2);
    REQUIRE(distortion->value.values[0] == Catch::Approx(0.1));
    REQUIRE(distortion->value.values[1] == Catch::Approx(0.2));

    const auto *tint = findValue(material, "tint");
    REQUIRE(tint != nullptr);
    REQUIRE(tint->value.type == SurfaceParamType::vec3);
    REQUIRE(tint->value.values[2] == Catch::Approx(0.2));

    const auto *layer_count = findValue(material, "layer_count");
    REQUIRE(layer_count != nullptr);
    REQUIRE(layer_count->value.type == SurfaceParamType::integer);
    REQUIRE(layer_count->value.integer_value == 4);
}

} // namespace Pelican
