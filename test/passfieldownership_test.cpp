#include "../src/project/passfieldownership.hpp"
#include "../src/project/projectformat.hpp"
#include "../src/project/projectpathresolver.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/rasterpass.hpp"
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
#include <string_view>
#include <utility>
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

struct FullscreenManifestEntry {
    std::string identity;
    nlohmann::json pass;
};

void collectFullscreenManifestEntries(
    const nlohmann::json &node,
    std::string_view relative_path,
    std::vector<FullscreenManifestEntry> &entries) {
    if (node.is_object()) {
        if (node.value("type", std::string{}) ==
                "fullscreen" &&
            node.contains("name") &&
            node.at("name").is_string()) {
            entries.push_back({
                std::string{relative_path} + "::" +
                    node.at("name").get<std::string>(),
                node,
            });
        }
        for (const auto &[key, value] : node.items()) {
            (void)key;
            collectFullscreenManifestEntries(
                value, relative_path, entries);
        }
        return;
    }
    if (node.is_array()) {
        for (const auto &value : node) {
            collectFullscreenManifestEntries(
                value, relative_path, entries);
        }
    }
}

std::vector<FullscreenManifestEntry>
shippingFullscreenManifestEntries() {
    std::vector<FullscreenManifestEntry> result;
    for (const auto relative_root : {
             "projects",
             "src/core/resources/render_pipelines",
             "src/core/resources/features"}) {
        const auto root = sourceRoot() / relative_root;
        for (const auto &entry :
             std::filesystem::recursive_directory_iterator{
                 root}) {
            if (!entry.is_regular_file() ||
                entry.path().extension() != ".json") {
                continue;
            }
            const auto relative_path =
                std::filesystem::relative(
                    entry.path(), sourceRoot())
                    .generic_string();
            collectFullscreenManifestEntries(
                nlohmann::json::parse(
                    readText(entry.path())),
                relative_path, result);
        }
    }
    std::sort(
        result.begin(), result.end(),
        [](const auto &lhs, const auto &rhs) {
            return lhs.identity < rhs.identity;
        });
    return result;
}

nlohmann::json explicitDefaultFullscreenRasterState() {
    return {
        {"topology", "triangle_list"},
        {"cull", "none"},
        {"front_face", "counter_clockwise"},
        {"color_attachments",
         nlohmann::json::array(
             {{{"blend", "opaque"},
               {"write_mask", "rgba"}}})},
    };
}

void materializeExplicitFullscreenDefaults(
    nlohmann::json &node) {
    if (node.is_object()) {
        if (node.value("type", std::string{}) ==
                "fullscreen" &&
            !node.contains("raster_state")) {
            node["raster_state"] =
                explicitDefaultFullscreenRasterState();
        }
        for (auto &[key, value] : node.items()) {
            (void)key;
            materializeExplicitFullscreenDefaults(value);
        }
        return;
    }
    if (node.is_array()) {
        for (auto &value : node) {
            materializeExplicitFullscreenDefaults(value);
        }
    }
}

struct CanonicalFullscreenSemanticDescriptor {
    std::string vertex_shader_reference;
    std::string fragment_shader_reference;
    RasterFixedFunctionState fixed_function;

    bool operator==(
        const CanonicalFullscreenSemanticDescriptor &) const =
        default;
};

std::string stableShaderReference(
    const nlohmann::json &shader,
    std::string_view stage) {
    const auto &reference =
        shader.at(std::string{stage});
    return reference.is_string()
               ? reference.get<std::string>()
               : reference.dump();
}

CanonicalFullscreenSemanticDescriptor
canonicalFullscreenSemanticDescriptor(
    const nlohmann::json &pass) {
    const auto &shader = pass.at("shader");
    return {
        .vertex_shader_reference =
            stableShaderReference(shader, "vertex"),
        .fragment_shader_reference =
            stableShaderReference(shader, "fragment"),
        .fixed_function =
            parseRasterFixedFunctionState(
                pass, 1,
                "canonical fullscreen descriptor '" +
                    pass.at("name").get<std::string>() +
                    "'"),
    };
}

std::vector<std::pair<
    std::string,
    CanonicalFullscreenSemanticDescriptor>>
canonicalFullscreenDescriptorsInConfig(
    const nlohmann::json &config) {
    std::vector<std::pair<
        std::string,
        CanonicalFullscreenSemanticDescriptor>> result;
    const auto collect = [&result](
                             const auto &self,
                             const nlohmann::json &node) -> void {
        if (node.is_object()) {
            const auto type =
                node.value("type", std::string{});
            if ((type == "fullscreen" ||
                 type == "output_transform") &&
                node.contains("name") &&
                node.at("name").is_string()) {
                result.emplace_back(
                    node.at("name").get<std::string>(),
                    canonicalFullscreenSemanticDescriptor(
                        node));
            }
            for (const auto &[key, value] : node.items()) {
                (void)key;
                self(self, value);
            }
            return;
        }
        if (node.is_array()) {
            for (const auto &value : node) {
                self(self, value);
            }
        }
    };
    collect(collect, config);
    std::sort(
        result.begin(), result.end(),
        [](const auto &lhs, const auto &rhs) {
            return lhs.first < rhs.first;
        });
    return result;
}

std::vector<nlohmann::json> generatedOutputTransformSet() {
    const auto composed = composeRenderFeatureConfig(
        nlohmann::json{
            {"render_targets", nlohmann::json::array()},
            {"rendering_passes",
             nlohmann::json::array({
                 {
                     {"name", "main"},
                     {"passes",
                      nlohmann::json::array({
                          {
                              {"name", "present"},
                              {"type", "fullscreen"},
                              {"output",
                               {{"color", "swapchain"},
                                {"depth", nullptr}}},
                              {"shader",
                               {{"vertex", "engine://fullscreen"},
                                {"fragment", "engine://scene_present"}}},
                          },
                      })},
                 },
             })},
        },
        RenderFeatureComposeDependencies{
            .runtime_shader_compiler_enabled = true,
        });
    std::vector<nlohmann::json> result;
    const auto &passes =
        composed.config.at("rendering_passes")
            .front()
            .at("passes");
    for (const auto &pass : passes) {
        if (pass.value("type", std::string{}) ==
            "output_transform") {
            result.push_back(pass);
        }
    }
    return result;
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
    "WP325 fullscreen authoring projection schema owns its expected JSON keys",
    "[wp325][pass-field-ownership][authoring-projection]") {
    const auto fields =
        passAuthoringProjectionFields(RenderPassType::fullscreen);
    std::vector<std::string> actual;
    actual.reserve(fields.size());
    for (const auto field : fields) {
        actual.emplace_back(field);
    }
    const std::vector<std::string> expected{
        "name", "type", "input", "output", "shader", "raster_state"};

    REQUIRE(actual == expected);
    REQUIRE_THROWS_WITH(
        passAuthoringProjectionFields(RenderPassType::material),
        "No pass authoring projection schema for type 'material'");
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
    "WP357 shipping fullscreen manifest keeps only the three bloom upsample blends explicit",
    "[wp353][wp357][fullscreen][manifest][canonical]") {
    const auto entries =
        shippingFullscreenManifestEntries();
    REQUIRE(entries.size() == 35);

    std::vector<std::string> identities;
    identities.reserve(entries.size());
    for (const auto &entry : entries) {
        identities.push_back(entry.identity);
    }
    const std::vector<std::string> expected_identities{
        "projects/example/passes/example_renderingpass_data.json::lighting_pass",
        "projects/example/passes/main_rendering_config.json::FinalBloomComposite",
        "projects/example/passes/main_rendering_config.json::HighLuminanceExtraction",
        "projects/example/passes/main_rendering_config.json::HorizontalBlur_0",
        "projects/example/passes/main_rendering_config.json::HorizontalBlur_1",
        "projects/example/passes/main_rendering_config.json::HorizontalBlur_2",
        "projects/example/passes/main_rendering_config.json::HorizontalBlur_3",
        "projects/example/passes/main_rendering_config.json::UpsampleBlend_1",
        "projects/example/passes/main_rendering_config.json::UpsampleBlend_2",
        "projects/example/passes/main_rendering_config.json::UpsampleBlend_3",
        "projects/example/passes/main_rendering_config.json::VerticalBlur_0",
        "projects/example/passes/main_rendering_config.json::VerticalBlur_1",
        "projects/example/passes/main_rendering_config.json::VerticalBlur_2",
        "projects/example/passes/main_rendering_config.json::VerticalBlur_3",
        "projects/example/passes/main_rendering_config.json::lighting_pass",
        "projects/example/passes/main_rendering_config.json::ssao_blur_pass",
        "projects/example/passes/main_rendering_config.json::ssao_pass",
        "projects/sprite_demo/passes/main.json::lighting_pass",
        "projects/vrm_xr_demo/passes/main.json::lighting_pass",
        "projects/vrm_xr_demo/passes/main.json::ssao_clear",
        "src/core/resources/features/cube_capture.json::cube_capture_lighting",
        "src/core/resources/features/cube_capture.json::cube_capture_ssao",
        "src/core/resources/features/cube_capture.json::cube_capture_ssao_blur",
        "src/core/resources/features/hdr.json::hdr_tonemap",
        "src/core/resources/features/planar_reflection.json::planar_reflection_lighting",
        "src/core/resources/features/planar_reflection.json::planar_reflection_ssao",
        "src/core/resources/features/planar_reflection.json::planar_reflection_ssao_blur",
        "src/core/resources/features/rt_shadow_mask.json::rt_shadow_mask",
        "src/core/resources/features/sky_ambient.json::sky_background",
        "src/core/resources/features/taa.json::taa_composite",
        "src/core/resources/features/taa.json::taa_resolve",
        "src/core/resources/render_pipelines/hybrid_v1.json::deferred_lighting",
        "src/core/resources/render_pipelines/hybrid_v1.json::scene_present",
        "src/core/resources/render_pipelines/hybrid_v1.json::ssao_blur_pass",
        "src/core/resources/render_pipelines/hybrid_v1.json::ssao_pass",
    };
    REQUIRE(identities == expected_identities);

    const std::array<std::string_view, 3> bloom_upsample_identities{
        "projects/example/passes/main_rendering_config.json::UpsampleBlend_1",
        "projects/example/passes/main_rendering_config.json::UpsampleBlend_2",
        "projects/example/passes/main_rendering_config.json::UpsampleBlend_3",
    };
    REQUIRE(
        std::count_if(
            entries.begin(), entries.end(),
            [](const auto &entry) {
                return entry.pass.contains("raster_state");
            }) == 3);
    REQUIRE(
        std::count_if(
            entries.begin(), entries.end(),
            [](const auto &entry) {
                return !entry.pass.contains("raster_state");
            }) == 32);
    for (const auto &entry : entries) {
        DYNAMIC_SECTION(entry.identity) {
            const bool bloom_upsample =
                std::find(
                    bloom_upsample_identities.begin(),
                    bloom_upsample_identities.end(),
                    entry.identity) !=
                bloom_upsample_identities.end();
            if (bloom_upsample) {
                REQUIRE(entry.pass.contains("raster_state"));
                REQUIRE_FALSE(entry.pass.contains("color_load_op"));
                REQUIRE(
                    entry.pass.at("shader").at("fragment") ==
                    "engine://bloom_upsample");
                REQUIRE(entry.pass.at("input").size() == 1);

                const auto &color_output =
                    entry.pass.at("output").at("color");
                REQUIRE(color_output.size() == 1);
                REQUIRE(color_output.at(0).is_object());
                REQUIRE(
                    color_output.at(0).at("load_op") == "Load");

                const auto expected_blend = nlohmann::json{
                    {"color",
                     {{"src", "one"},
                      {"dst", "one"},
                      {"op", "add"}}},
                    {"alpha",
                     {{"src", "one"},
                      {"dst", "one"},
                      {"op", "add"}}},
                };
                REQUIRE(
                    entry.pass.at("raster_state")
                        .at("color_attachments").at(0)
                        .at("blend") == expected_blend);

                const auto fixed =
                    parseRasterFixedFunctionState(
                        entry.pass, 1, entry.identity);
                REQUIRE(fixed.color_attachments.size() == 1);
                const auto &blend =
                    fixed.color_attachments.front().blend;
                REQUIRE(blend.enabled);
                REQUIRE(
                    blend.color.source ==
                    MaterialOutputBlendFactor::one);
                REQUIRE(
                    blend.color.destination ==
                    MaterialOutputBlendFactor::one);
                REQUIRE(
                    blend.color.operation ==
                    MaterialOutputBlendOperation::add);
                REQUIRE(
                    blend.alpha.source ==
                    MaterialOutputBlendFactor::one);
                REQUIRE(
                    blend.alpha.destination ==
                    MaterialOutputBlendFactor::one);
                REQUIRE(
                    blend.alpha.operation ==
                    MaterialOutputBlendOperation::add);

                // The load-op negative control removes only the attachment
                // field; retaining the object, target and raster state makes
                // accidental pass-wide fallback impossible.
                auto no_load = entry.pass;
                no_load["output"]["color"][0].erase("load_op");
                REQUIRE_FALSE(
                    no_load.at("output").at("color").at(0)
                        .contains("load_op"));
                REQUIRE_FALSE(no_load.contains("color_load_op"));
                REQUIRE(
                    no_load.at("output").at("color").at(0)
                        .at("target") ==
                    color_output.at(0).at("target"));
                REQUIRE(
                    no_load.at("raster_state") ==
                    entry.pass.at("raster_state"));
                continue;
            }

            REQUIRE_FALSE(
                entry.pass.contains("raster_state"));
            auto explicit_defaults = entry.pass;
            explicit_defaults["raster_state"] =
                explicitDefaultFullscreenRasterState();
            REQUIRE(
                canonicalFullscreenSemanticDescriptor(
                    entry.pass) ==
                canonicalFullscreenSemanticDescriptor(
                    explicit_defaults));

            auto explicit_non_default = entry.pass;
            explicit_non_default["raster_state"] = {
                {"color_attachments",
                 nlohmann::json::array(
                     {{{"blend", "additive"},
                       {"write_mask", "rg"}}})},
            };
            CHECK_FALSE(
                canonicalFullscreenSemanticDescriptor(
                    entry.pass) ==
                canonicalFullscreenSemanticDescriptor(
                    explicit_non_default));
        }
    }

    // output_transform is engine-generated and deliberately remains a
    // separate invariant set from the 35 authored fullscreen entries.
    const auto output_transforms =
        generatedOutputTransformSet();
    REQUIRE(output_transforms.size() == 1);
    REQUIRE(
        output_transforms.front().at("name") ==
        "output_transform");
    REQUIRE_FALSE(
        output_transforms.front().contains(
            "raster_state"));
    auto explicit_output_transform =
        output_transforms.front();
    explicit_output_transform["raster_state"] =
        explicitDefaultFullscreenRasterState();
    REQUIRE(
        canonicalFullscreenSemanticDescriptor(
            output_transforms.front()) ==
        canonicalFullscreenSemanticDescriptor(
            explicit_output_transform));
}

TEST_CASE(
    "WP357 color migration ledger fixes the shader accumulation and three pass mappings",
    "[wp357][color][manifest]") {
    const auto manifest = nlohmann::json::parse(readText(
        sourceRoot() / "docs/color_migration_manifest.json"));
    REQUIRE(manifest.at("schema") ==
            "pelican.color_migration_manifest");

    const auto &migrations = manifest.at("implemented_migrations");
    const auto migration = std::find_if(
        migrations.begin(), migrations.end(),
        [](const auto &entry) {
            return entry.value("work_package", std::string{}) == "WP357";
        });
    REQUIRE(migration != migrations.end());
    REQUIRE(std::count_if(
                migrations.begin(), migrations.end(),
                [](const auto &entry) {
                    return entry.value("work_package", std::string{}) ==
                           "WP357";
                }) == 1);
    REQUIRE(migration->at("decision") ==
            "accept_standard_form_visual_change");
    const auto expected_shader = nlohmann::json{
        {"path", "src/core/resources/bloom_upsample.frag"},
        {"embedded_ids", nlohmann::json::array(
             {"bloom_upsample.frag", "bloom_upsample.frag.spv"})},
        {"sampled_inputs", 1},
        {"operation", "bilinear_sample"},
    };
    REQUIRE(migration->at("shader") == expected_shader);
    REQUIRE(migration->at("accumulation").at("old") ==
            "2*V3 + 4*(V2+V1+V0)");
    REQUIRE(migration->at("accumulation").at("new") ==
            "2*(V3+V2+V1+V0)");

    const auto expected_blend = nlohmann::json{
        {"src", "one"}, {"dst", "one"}, {"op", "add"}};
    const std::array<std::array<std::string_view, 3>, 3> expected{
        std::array<std::string_view, 3>{
            "UpsampleBlend_3", "Bloom_Upsample_V_3_RT",
            "Bloom_Upsample_V_2_RT"},
        std::array<std::string_view, 3>{
            "UpsampleBlend_2", "Bloom_Upsample_V_2_RT",
            "Bloom_Upsample_V_1_RT"},
        std::array<std::string_view, 3>{
            "UpsampleBlend_1", "Bloom_Upsample_V_1_RT",
            "Bloom_Upsample_V_0_RT"},
    };
    const auto &passes = migration->at("passes");
    REQUIRE(passes.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto &pass = passes.at(index);
        REQUIRE(pass.at("name").get<std::string>() ==
                std::string{expected[index][0]});
        REQUIRE(pass.at("input").get<std::string>() ==
                std::string{expected[index][1]});
        REQUIRE(pass.at("loaded_output").get<std::string>() ==
                std::string{expected[index][2]});
        REQUIRE(pass.at("load_op") == "Load");
        REQUIRE(pass.at("color_blend") == expected_blend);
        REQUIRE(pass.at("alpha_blend") == expected_blend);
    }
    REQUIRE(migration->at("golden_approval_cases") ==
            nlohmann::json::array(
                {"skeletal_toon", "usd0b_static_geometry"}));
    REQUIRE(migration->at("oracle_api") ==
            "evaluateBloomUpsampleOracle");
}

TEST_CASE(
    "WP353 canonical fullscreen semantics and named failures match with the runtime compiler on and off",
    "[wp353][fullscreen][canonical][runtime-compiler]") {
    constexpr std::string_view feature_reference =
        "engine://features/taa.json";
    const auto feature = nlohmann::json::parse(
        loadEngineText(feature_reference));
    const auto omitted_config =
        featureCorpusConfig(
            feature, std::string{feature_reference});
    auto explicit_config = omitted_config;
    materializeExplicitFullscreenDefaults(
        explicit_config);

    const auto omitted_loader =
        [](std::string_view reference) {
            return loadEngineText(reference);
        };
    const auto explicit_loader =
        [](std::string_view reference) {
            auto document = nlohmann::json::parse(
                loadEngineText(reference));
            materializeExplicitFullscreenDefaults(
                document);
            return document.dump();
        };

    auto compiler_on = corpusCapabilities();
    compiler_on.runtime_shader_compiler_enabled = true;
    const auto omitted_resolved = resolveRenderPipeline(
        RenderPipelineRequest{
            omitted_config, "wp353 omitted compiler-on"},
        compiler_on,
        RenderPipelineResolveDependencies{
            .load_feature_json = omitted_loader,
            .load_pipeline_json = omitted_loader,
        });
    const auto explicit_resolved = resolveRenderPipeline(
        RenderPipelineRequest{
            explicit_config, "wp353 explicit compiler-on"},
        compiler_on,
        RenderPipelineResolveDependencies{
            .load_feature_json = explicit_loader,
            .load_pipeline_json = explicit_loader,
        });
    const auto omitted_descriptors =
        canonicalFullscreenDescriptorsInConfig(
            omitted_resolved.normalized_config);
    const auto explicit_descriptors =
        canonicalFullscreenDescriptorsInConfig(
            explicit_resolved.normalized_config);
    REQUIRE(omitted_descriptors.size() == 7);
    REQUIRE(
        omitted_descriptors ==
        explicit_descriptors);

    auto compiler_off = compiler_on;
    compiler_off.runtime_shader_compiler_enabled = false;
    const auto capture_failure =
        [&](const nlohmann::json &config,
            const RenderPipelineResolveDependencies &dependencies,
            std::string_view source) {
            try {
                (void)resolveRenderPipeline(
                    RenderPipelineRequest{
                        config, std::string{source}},
                    compiler_off, dependencies);
            } catch (const std::runtime_error &error) {
                return std::string{error.what()};
            }
            return std::string{"<resolution unexpectedly succeeded>"};
        };
    const auto omitted_failure = capture_failure(
        omitted_config,
        RenderPipelineResolveDependencies{
            .load_feature_json = omitted_loader,
            .load_pipeline_json = omitted_loader,
        },
        "wp353 omitted compiler-off");
    const auto explicit_failure = capture_failure(
        explicit_config,
        RenderPipelineResolveDependencies{
            .load_feature_json = explicit_loader,
            .load_pipeline_json = explicit_loader,
        },
        "wp353 explicit compiler-off");
    REQUIRE(
        omitted_failure.find(
            "runtime shader compiler is required") !=
        std::string::npos);
    REQUIRE(omitted_failure == explicit_failure);
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
