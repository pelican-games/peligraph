#include "../src/project/materialformat.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
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

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

const MaterialParam *findParam(const MaterialDefinition &material, std::string_view name) {
    for (const auto &param : material.params) {
        if (param.name == name) {
            return &param;
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
    } else if (error_kind == "param") {
        REQUIRE(contains(message, "param"));
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

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto material_json = readJson(fixtureRoot() / file);
            const auto expected = entry.at("expect").get<std::string>();
            if (expected == "ok") {
                const auto document = parseMaterialFormatJson(material_json);
                REQUIRE(document.materials.size() == entry.at("material_count").get<size_t>());
                REQUIRE(document.warnings.size() == entry.at("warning_count").get<size_t>());
            } else {
                const auto error_kind = entry.at("error_kind").get<std::string>();
                std::string message;
                try {
                    (void)parseMaterialFormatJson(material_json);
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
    REQUIRE(material.defines.empty());
    REQUIRE(material.params.empty());
    REQUIRE(material.base.base_color_factor == std::array<double, 4>{1.0, 1.0, 1.0, 1.0});
    REQUIRE(material.base.metallic_factor == Catch::Approx(1.0));
    REQUIRE(material.base.roughness_factor == Catch::Approx(1.0));
    REQUIRE(material.base.emissive_factor == std::array<double, 3>{0.0, 0.0, 0.0});
}

TEST_CASE("full material format round-trips into typed fields and warnings", "[material-format]") {
    const auto document = parseMaterialFormatJson(readJson(fixtureRoot() / "valid" / "full.json"));
    REQUIRE(document.materials.size() == 1);
    REQUIRE(document.warnings.size() == 3);
    REQUIRE(contains(document.warnings.at(0), "generator"));
    REQUIRE(contains(document.warnings.at(1), "editor"));
    REQUIRE(contains(document.warnings.at(2), "clearcoatFactor"));

    const auto &material = document.materials.front();
    REQUIRE(material.name == "lava");
    REQUIRE(material.shader == "project://shaders/lava");
    REQUIRE(material.defines == std::vector<std::string>{"LAVA_FLOW", "USE_EMISSIVE"});
    REQUIRE(material.base.base_color_factor == std::array<double, 4>{1.0, 0.8, 0.6, 1.0});
    REQUIRE(material.base.base_color_texture == "project://textures/lava_albedo.png");
    REQUIRE(material.base.metallic_factor == Catch::Approx(0.0));
    REQUIRE(material.base.roughness_factor == Catch::Approx(0.8));
    REQUIRE(material.base.metallic_roughness_texture == "engine://default_mr.png");
    REQUIRE(material.base.normal_texture == "project://textures/lava_normal.png");
    REQUIRE(material.base.emissive_factor == std::array<double, 3>{2.0, 0.5, 0.1});
    REQUIRE(material.base.emissive_texture == "project://textures/lava_emissive.png");

    const auto *flow_speed = findParam(material, "flow_speed");
    REQUIRE(flow_speed != nullptr);
    REQUIRE(flow_speed->value.kind == MaterialParamKind::scalar);
    REQUIRE(flow_speed->value.values[0] == Catch::Approx(0.35));

    const auto *distortion = findParam(material, "distortion");
    REQUIRE(distortion != nullptr);
    REQUIRE(distortion->value.kind == MaterialParamKind::vec2);
    REQUIRE(distortion->value.values[0] == Catch::Approx(0.1));
    REQUIRE(distortion->value.values[1] == Catch::Approx(0.2));

    const auto *tint = findParam(material, "tint");
    REQUIRE(tint != nullptr);
    REQUIRE(tint->value.kind == MaterialParamKind::vec4);
    REQUIRE(tint->value.values[3] == Catch::Approx(1.0));
}

} // namespace Pelican
