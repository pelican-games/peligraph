#include "../src/project/materialformat.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
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
    } else if (error_kind == "textures") {
        REQUIRE(contains(message, "textures"));
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

TEST_CASE("material format parses optional render path and exact pass separately",
          "[material-format][routing]") {
    const auto input = nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials", {{{"name", "hero"},
                        {"render_path", "forward"},
                        {"pass", "hero_forward"}}}},
    };
    const auto parsed = parseMaterialFormatJson(input);
    REQUIRE(parsed.materials.front().render_path == MaterialRenderPath::forward);
    REQUIRE(parsed.materials.front().exact_pass == "hero_forward");

    auto invalid = input;
    invalid["materials"][0]["render_path"] = "magic";
    REQUIRE_THROWS_WITH(parseMaterialFormatJson(invalid),
                        Catch::Matchers::ContainsSubstring("auto, deferred, or forward"));
    invalid = input;
    invalid["materials"][0]["pass"] = "forward/pass";
    REQUIRE_THROWS_WITH(parseMaterialFormatJson(invalid),
                        Catch::Matchers::ContainsSubstring("pass must match"));
}

TEST_CASE("material format parses canonical stable draw tags",
          "[material-format][draw-tag][wp206a]") {
    auto input = nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials",
         {{{"name", "hero"},
           {"tags", {"outline", "character"}}}}},
    };
    const auto parsed =
        parseMaterialFormatJson(input);
    REQUIRE(parsed.warnings.empty());
    REQUIRE(parsed.materials.front().tags ==
            std::vector<std::string>{
                "character", "outline"});

    auto invalid = input;
    invalid["materials"][0]["tags"] =
        {"outline", "outline"};
    REQUIRE_THROWS_WITH(
        parseMaterialFormatJson(invalid),
        Catch::Matchers::ContainsSubstring(
            "duplicate tag: outline"));

    invalid = input;
    invalid["materials"][0]["tags"] = {""};
    REQUIRE_THROWS_WITH(
        parseMaterialFormatJson(invalid),
        Catch::Matchers::ContainsSubstring(
            "tag is empty or too long"));

    invalid = input;
    invalid["materials"][0]["tags"] = "outline";
    REQUIRE_THROWS_WITH(
        parseMaterialFormatJson(invalid),
        Catch::Matchers::ContainsSubstring(
            "tags must be an array of strings"));
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

std::filesystem::path bindingFixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
           "material_binding";
}

TEST_CASE("material texture overrides are limited to declared surface textures",
          "[material-format][texture-override]") {
    auto valid = nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials", {{{"name", "lava_override"},
                        {"surface", "project://shaders/lava.surface"},
                        {"textures", {{"flow_map", "project://textures/flow.png"}}}}}},
    };
    const auto parsed = parseMaterialFormatJson(valid, surfaceCatalog());
    REQUIRE(parsed.materials.front().texture_overrides.size() == 1);
    REQUIRE(parsed.materials.front().texture_overrides.front().name == "flow_map");
    REQUIRE(parsed.materials.front().texture_overrides.front().reference ==
            "project://textures/flow.png");

    auto undeclared = valid;
    undeclared["materials"][0]["textures"] = {
        {"missing_texture", "project://textures/missing.png"}};
    REQUIRE_THROWS_WITH(parseMaterialFormatJson(undeclared, surfaceCatalog()),
                        Catch::Matchers::ContainsSubstring("lava_override") &&
                            Catch::Matchers::ContainsSubstring("missing_texture") &&
                            Catch::Matchers::ContainsSubstring("not declared"));

    auto wrong_type = valid;
    wrong_type["materials"][0]["textures"]["flow_map"] =
        nlohmann::json{{"reference", "project://textures/flow.png"}};
    REQUIRE_THROWS_WITH(parseMaterialFormatJson(wrong_type, surfaceCatalog()),
                        Catch::Matchers::ContainsSubstring("lava_override") &&
                            Catch::Matchers::ContainsSubstring("flow_map") &&
                            Catch::Matchers::ContainsSubstring("string reference"));
}

TEST_CASE("material routing parses the six-state selector without overriding render state",
          "[material-format][routing]") {
    auto input = nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials", {{{"name", "masked_shell"},
                        {"surface", "project://shaders/lava.surface"},
                        {"routing", {{"alpha_mode", "mask"},
                                     {"double_sided", true}}}}}},
    };
    const auto parsed = parseMaterialFormatJson(input, surfaceCatalog());
    REQUIRE(parsed.materials.front().routing.has_value());
    REQUIRE(parsed.materials.front().routing->alpha_mode == MaterialAlphaMode::mask);
    REQUIRE(parsed.materials.front().routing->double_sided);
    REQUIRE(std::string{materialVariantName(*parsed.materials.front().routing)} ==
            "mask_double_sided");
}

TEST_CASE("primitive material binding schema rejects duplicate collision fragment and type errors",
          "[material-binding][schema]") {
    const std::array cases{
        std::pair{"invalid/duplicate_usd_path.json", "duplicate material binding USD path"},
        std::pair{"invalid/target_collision.json", "collision at GLB mesh 2 primitive 3"},
        std::pair{"invalid/fragment_model.json", "without a fragment"},
        std::pair{"invalid/routing_type.json", "double_sided must be boolean"},
    };
    for (const auto &[file, expected] : cases) {
        DYNAMIC_SECTION(file) {
            REQUIRE_THROWS_WITH(
                parsePrimitiveMaterialBindingJson(readJson(bindingFixtureRoot() / file)),
                Catch::Matchers::ContainsSubstring(expected));
        }
    }
}

TEST_CASE("six OpenPBR routing states have a byte-stable dump and fixed boundaries",
          "[material-binding][routing][golden]") {
    const auto document = parsePrimitiveMaterialBindingJson(
        readJson(bindingFixtureRoot() / "valid" / "six_variants.json"));
    REQUIRE(document.bindings.size() == 6);
    REQUIRE(dumpPrimitiveMaterialBindings(document) ==
            readText(bindingFixtureRoot() / "six_variants_dump.txt"));

    for (const auto &binding : document.bindings) {
        const auto state = materialVariantRenderState(binding.routing);
        REQUIRE(state.depth_test);
        REQUIRE(state.cull == (binding.routing.double_sided
                                   ? SurfaceCullMode::none
                                   : SurfaceCullMode::back));
        REQUIRE(materialVariantKeepsFace(binding.routing, true));
        REQUIRE(materialVariantKeepsFace(binding.routing, false) ==
                binding.routing.double_sided);
        REQUIRE(state.depth_write ==
                (binding.routing.alpha_mode != MaterialAlphaMode::blend));
        REQUIRE((state.blend == SurfaceBlendMode::blend) ==
                (binding.routing.alpha_mode == MaterialAlphaMode::blend));
    }

    REQUIRE_FALSE(materialMaskKeepsFragment(0.499999, 0.5));
    REQUIRE(materialMaskKeepsFragment(0.5, 0.5));
    REQUIRE(materialMaskKeepsFragment(0.500001, 0.5));
}

} // namespace Pelican
