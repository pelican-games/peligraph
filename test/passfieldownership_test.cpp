#include "../src/project/passfieldownership.hpp"
#include "../src/project/projectformat.hpp"
#include "../src/project/projectpathresolver.hpp"
#include "../src/project/renderfeatureoverlay.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {
namespace {

bool owns(const PassFieldOwnershipEntry &entry,
          std::string_view field) {
    return std::find(entry.fields.begin(), entry.fields.end(), field) !=
           entry.fields.end();
}

std::filesystem::path sourceRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error(
            "failed to open corpus file: " + path.string());
    }
    return {std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{}};
}

std::string loadEngineText(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error(
            "expected engine resource reference: " +
            std::string{reference});
    }
    return readText(sourceRoot() / "src/core/resources" /
                    reference.substr(prefix.size()));
}

RenderEnvironmentCapabilities corpusCapabilities() {
    return {
        .runtime_shader_compiler_enabled =
            PELICAN_RUNTIME_SHADER_COMPILER != 0,
        .graph_variant = RenderPipelineGraphVariant::flat,
        .pass_field_ownership = {
            .imgui_enabled = PELICAN_WITH_IMGUI != 0,
        },
    };
}

nlohmann::json featureCorpusConfig(
    const nlohmann::json &feature,
    const std::string &reference) {
    auto config = nlohmann::json::parse(
        loadEngineText(
            "engine://render_pipelines/hybrid_v1.json"))
                      .at("config");
    nlohmann::json parameters = nlohmann::json::object();
    if (feature.contains("parameters") &&
        feature.at("parameters").contains("render_targets")) {
        for (const auto &declaration :
             feature.at("parameters").at("render_targets")) {
            const auto parameter_name =
                declaration.at("name").get<std::string>();
            const auto target_name =
                declaration.value(
                    "default", "wp311_" + parameter_name);
            if (!declaration.contains("default")) {
                parameters[parameter_name] = target_name;
            }

            auto &targets = config.at("render_targets");
            auto target = std::find_if(
                targets.begin(), targets.end(),
                [&](const auto &candidate) {
                    return candidate.value(
                               "name", std::string{}) == target_name;
                });
            if (target == targets.end()) {
                const bool depth = declaration.contains("usage") &&
                    std::find(
                        declaration.at("usage").begin(),
                        declaration.at("usage").end(),
                        "DEPTH_STENCIL_ATTACHMENT") !=
                        declaration.at("usage").end();
                targets.push_back({
                    {"name", target_name},
                    {"format", depth ? "D32_SFLOAT"
                                     : "R16G16B16A16_SFLOAT"},
                    {"usage", nlohmann::json::array()},
                });
                target = std::prev(targets.end());
            }
            for (const auto field : {
                     "role", "format_class", "extent_scale",
                     "width", "height", "sample_count"}) {
                if (declaration.contains(field)) {
                    (*target)[field] = declaration.at(field);
                }
            }
            if (declaration.contains("usage")) {
                auto &usage = (*target)["usage"];
                if (!usage.is_array()) {
                    usage = nlohmann::json::array();
                }
                for (const auto &required : declaration.at("usage")) {
                    if (std::find(usage.begin(), usage.end(), required) ==
                        usage.end()) {
                        usage.push_back(required);
                    }
                }
            }
        }
    }
    config["features"] = nlohmann::json::array({
        {
            {"ref", reference},
            {"parameters", std::move(parameters)},
        },
    });
    return config;
}

template <class Loader>
bool configRequiresRuntimeShaderCompiler(
    const nlohmann::json &config,
    const Loader &loader) {
    if (config.contains("pipeline")) {
        const auto reference =
            config.at("pipeline").at("preset").get<std::string>();
        const auto preset = nlohmann::json::parse(loader(reference));
        if (configRequiresRuntimeShaderCompiler(
                preset.at("config"), loader)) {
            return true;
        }
    }
    if (!config.contains("features")) {
        return false;
    }
    for (const auto &instance : config.at("features")) {
        const auto reference = instance.is_string()
                                   ? instance.get<std::string>()
                                   : instance.at("ref").get<std::string>();
        const auto feature = nlohmann::json::parse(loader(reference));
        if (feature.at("schema") == "pelican.render_feature") {
            if (feature.value(
                    "runtime_shader_compiler",
                    std::string{"required"}) != "optional") {
                return true;
            }
            continue;
        }
        if (feature.at("schema") ==
            "pelican.render_feature_overlay") {
            nlohmann::json overlay_config{
                {"features", feature.at("features")},
            };
            if (configRequiresRuntimeShaderCompiler(
                    overlay_config, loader)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE(
    "WP311 pass field ownership table accepts every owner and rejects a non-owner",
    "[wp311][pass-field-ownership]") {
    const auto table = passFieldOwnershipTable();
    REQUIRE_FALSE(table.empty());
    const PassFieldOwnershipCapabilities all_capabilities{
        .imgui_enabled = true,
    };

    for (const auto &owner : table) {
        for (const auto field : owner.fields) {
            DYNAMIC_SECTION(owner.type_name << " owns " << field) {
                const nlohmann::json positive{
                    {"name", "owner_positive"},
                    {"type", owner.type_name},
                    {std::string{field}, nullptr},
                };
                CHECK(validatePassFieldOwnership(
                          positive, all_capabilities) == owner.type);

                const auto non_owner = std::find_if(
                    table.begin(), table.end(),
                    [&](const auto &candidate) {
                        return !owns(candidate, field);
                    });
                REQUIRE(non_owner != table.end());
                const nlohmann::json negative{
                    {"name", "owner_negative"},
                    {"type", non_owner->type_name},
                    {std::string{field}, nullptr},
                };
                CHECK_THROWS_WITH(
                    validatePassFieldOwnership(
                        negative, all_capabilities),
                    Catch::Matchers::ContainsSubstring(
                        "Pass 'owner_negative' type '") &&
                        Catch::Matchers::ContainsSubstring(
                            "does not own field '" +
                            std::string{field} + "'"));
            }
        }
    }
}

TEST_CASE(
    "WP311 dynamically resolves shipped projects pipelines and features",
    "[wp311][pass-field-ownership][corpus]") {
    std::size_t project_count = 0;
    const auto projects = sourceRoot() / "projects";
    for (const auto &entry :
         std::filesystem::directory_iterator{projects}) {
        const auto project_file = entry.path() / "project.json";
        if (!entry.is_directory() ||
            !std::filesystem::is_regular_file(project_file)) {
            continue;
        }
        ++project_count;
        DYNAMIC_SECTION(
            "project startup: " << entry.path().filename().string()) {
            const auto project = parseProjectEnvelopeText(
                readText(project_file));
            ProjectPathResolver paths;
            paths.setup(entry.path(), false, project.envelope);
            const auto config_reference =
                project.envelope.basic_config.at(
                    "rendering_config_json").get<std::string>();
            const auto authored = nlohmann::json::parse(
                paths.loadText(config_reference));
            const auto loader = [&](std::string_view reference) {
                return reference.starts_with("engine://")
                           ? loadEngineText(reference)
                           : paths.loadText(reference);
            };
            const auto resolve = [&] {
                return resolveRenderPipeline(
                    RenderPipelineRequest{
                        authored,
                        entry.path().filename().string()},
                    corpusCapabilities(),
                    RenderPipelineResolveDependencies{
                        .load_feature_json = loader,
                        .load_pipeline_json = loader,
                    });
            };
#if !PELICAN_RUNTIME_SHADER_COMPILER
            if (configRequiresRuntimeShaderCompiler(
                    authored, loader)) {
                CHECK_THROWS_WITH(
                    resolve(),
                    Catch::Matchers::ContainsSubstring(
                        "runtime shader compiler is required"));
                continue;
            }
#endif
            const auto resolved = resolve();
            CHECK_NOTHROW(compileRenderPipeline(resolved));
        }
    }
    REQUIRE(project_count > 0);

    std::size_t pipeline_count = 0;
    const auto pipeline_root =
        sourceRoot() / "src/core/resources/render_pipelines";
    for (const auto &entry :
         std::filesystem::directory_iterator{pipeline_root}) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".json") {
            continue;
        }
        ++pipeline_count;
        DYNAMIC_SECTION(
            "built-in pipeline: " << entry.path().filename().string()) {
            const auto reference =
                "engine://render_pipelines/" +
                entry.path().filename().generic_string();
            const nlohmann::json authored{
                {"pipeline", {{"preset", reference}}},
            };
            const auto resolve = [&] {
                return resolveRenderPipeline(
                    RenderPipelineRequest{authored, reference},
                    corpusCapabilities(),
                    RenderPipelineResolveDependencies{
                        .load_feature_json = loadEngineText,
                        .load_pipeline_json = loadEngineText,
                    });
            };
#if !PELICAN_RUNTIME_SHADER_COMPILER
            if (configRequiresRuntimeShaderCompiler(
                    authored, loadEngineText)) {
                CHECK_THROWS_WITH(
                    resolve(),
                    Catch::Matchers::ContainsSubstring(
                        "runtime shader compiler is required"));
                continue;
            }
#endif
            const auto resolved = resolve();
            CHECK_NOTHROW(compileRenderPipeline(resolved));
        }
    }
    REQUIRE(pipeline_count > 0);

    std::size_t feature_count = 0;
    const auto feature_root =
        sourceRoot() / "src/core/resources/features";
    for (const auto &entry :
         std::filesystem::directory_iterator{feature_root}) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".json") {
            continue;
        }
        ++feature_count;
        DYNAMIC_SECTION(
            "built-in feature: " << entry.path().filename().string()) {
            const auto envelope = nlohmann::json::parse(
                readText(entry.path()));
            const auto schema =
                envelope.value("schema", std::string{});
            REQUIRE((schema == "pelican.render_feature" ||
                     schema == "pelican.render_feature_overlay"));
            if (schema == "pelican.render_feature" &&
                envelope.contains("passes")) {
                REQUIRE(envelope.at("passes").is_array());
                for (const auto &insertion :
                     envelope.at("passes")) {
                    REQUIRE(insertion.is_object());
                    REQUIRE(insertion.contains("pass"));
                    CHECK_NOTHROW(validatePassFieldOwnership(
                        insertion.at("pass"),
                        corpusCapabilities()
                            .pass_field_ownership,
                        entry.path().filename().string()));
                }
            }
            const auto reference =
                "engine://features/" +
                entry.path().filename().generic_string();
            auto authored = schema == "pelican.render_feature"
                                ? featureCorpusConfig(
                                      envelope, reference)
                                : nlohmann::json::parse(
                                      loadEngineText(
                                          "engine://render_pipelines/"
                                          "hybrid_v1.json"))
                                      .at("config");
            if (schema == "pelican.render_feature_overlay") {
                authored = applyRenderFeatureOverlays(
                    authored,
                    std::array<std::string, 1>{reference},
                    loadEngineText);
            }
            const auto resolve = [&] {
                return resolveRenderPipeline(
                    RenderPipelineRequest{authored, reference},
                    corpusCapabilities(),
                    RenderPipelineResolveDependencies{
                        .load_feature_json = loadEngineText,
                        .load_pipeline_json = loadEngineText,
                    });
            };
#if !PELICAN_RUNTIME_SHADER_COMPILER
            if (configRequiresRuntimeShaderCompiler(
                    authored, loadEngineText)) {
                CHECK_THROWS_WITH(
                    resolve(),
                    Catch::Matchers::ContainsSubstring(
                        "runtime shader compiler is required"));
                continue;
            }
#endif
            const auto resolved = resolve();
            CHECK_NOTHROW(compileRenderPipeline(resolved));
        }
    }
    REQUIRE(feature_count > 0);
}

TEST_CASE(
    "WP311 pass type validation covers missing non-string unknown and ImGui capability",
    "[wp311][pass-field-ownership][type]") {
    const PassFieldOwnershipCapabilities all_capabilities{
        .imgui_enabled = true,
    };
    CHECK_THROWS_WITH(
        validatePassFieldOwnership(
            nlohmann::json{{"name", "missing_type"}},
            all_capabilities),
        "Pass 'missing_type' field 'type' must be a string");
    CHECK_THROWS_WITH(
        validatePassFieldOwnership(
            nlohmann::json{{"name", "numeric_type"}, {"type", 7}},
            all_capabilities),
        "Pass 'numeric_type' field 'type' must be a string");
    CHECK_THROWS_WITH(
        validatePassFieldOwnership(
            nlohmann::json{{"name", "unknown_type"},
                           {"type", "compute"}},
            all_capabilities),
        "Pass 'unknown_type' has unknown type 'compute'");
    CHECK(validatePassFieldOwnership(
              nlohmann::json{{"name", "imgui_on"},
                             {"type", "imgui"}},
              all_capabilities) == RenderPassType::imgui);
    CHECK_THROWS_WITH(
        validatePassFieldOwnership(
            nlohmann::json{{"name", "imgui_off"},
                           {"type", "imgui"}},
            PassFieldOwnershipCapabilities{
                .imgui_enabled = false}),
        "Pass 'imgui_off' has unknown type 'imgui'");
}

} // namespace Pelican
