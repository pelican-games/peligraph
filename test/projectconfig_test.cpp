#include "../src/core/container.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/renderingpassconfigloader.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/project/assetdataformat.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "../src/project/renderfeatureoverlay.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/project/sceneformat.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <map>
#include <vector>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "project_format";
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    file << text;
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

std::filesystem::path weaklyCanonical(const std::filesystem::path &path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    REQUIRE_FALSE(ec);
    return canonical;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

struct ProjectSandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    ProjectSandbox() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("pelican_projectconfig_" + suffix);
        root = base / "project";

        writeText(root / "scenes" / "main.scene.json", R"json({
  "schema": "pelican.scene",
  "version": 2,
  "scene-ok": true,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
})json");
        writeText(root / "assets" / "model.glb", "fake model");
        writeText(root / "assets" / "image.png", "fake image");
        writeText(root / "assets" / "assets.json", R"json({
  "schema": "pelican.asset_data",
  "version": 2,
  "models": [
    {"name": "model", "path": "assets/model.glb"}
  ]
})json");
        writeText(root / "passes" / "main.json", R"json({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "render_targets": [],
    "rendering_passes": [
      {
        "name": "main",
        "passes": [
          {
            "name": "fullscreen",
            "type": "fullscreen",
            "shader": {
              "vertex": "shaders/fullscreen",
              "fragment": "shaders/fullscreen"
            }
          }
        ]
      }
    ]
  }
})json");
        writeText(root / "ui" / "ui.json", R"json({
  "schema":"pelican.ui","version":1,"key":"fixture",
  "root":{"id":"root","type":"panel","children":[
    {"id":"image","type":"image","sprite":"assets/ui.atlas.json#sprite/image"}
  ]}
})json");
    }

    ~ProjectSandbox() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
};

std::string projectConfigErrorMessage(const nlohmann::json &project, const ProjectSandbox &sandbox,
                                      bool ignore_engine_version) {
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    auto &source = GET_MODULE(ProjectSource);
    source.setProjectData(project.dump());
    source.setIgnoreEngineVersion(ignore_engine_version);

    try {
        (void)GET_MODULE(ProjectBasicConfig);
    } catch (const std::exception &ex) {
        return ex.what();
    }
    return {};
}

void requireProjectConfigOk(const nlohmann::json &project, const ProjectSandbox &sandbox) {
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());

    const auto &config = GET_MODULE(ProjectBasicConfig);
    REQUIRE(config.windowTitle() == "Fixture Project");
    REQUIRE(config.initialWindowSize().width == 320);
    REQUIRE(config.initialWindowSize().height == 180);
    REQUIRE(contains(config.sceneDataJson(), "scene-ok"));

    const auto assets = nlohmann::json::parse(config.assetDataJson());
    REQUIRE(assets.at("models").at(0).at("path").get<std::string>() ==
            weaklyCanonical(sandbox.root / "assets" / "model.glb").string());

    const auto rendering = nlohmann::json::parse(config.renderingConfigJson());
    const auto shader = rendering.at("rendering")
                            .at("rendering_passes").at(0).at("passes").at(0).at("shader");
    REQUIRE(shader.at("vertex").get<std::string>() == "shaders/fullscreen");
    REQUIRE(shader.at("fragment").get<std::string>() == "shaders/fullscreen");

    const auto ui = nlohmann::json::parse(config.uiConfigJson());
    REQUIRE(ui.at("root").at("children").at(0).at("sprite").get<std::string>() ==
            "assets/ui.atlas.json#sprite/image");
}

void requireErrorKind(std::string_view message, std::string_view error_kind) {
    if (error_kind == "engine_version") {
        REQUIRE(contains(message, "engine_min_version"));
    } else if (error_kind == "schema") {
        REQUIRE(contains(message, "schema"));
    } else if (error_kind == "version") {
        REQUIRE(contains(message, "version"));
        REQUIRE(contains(message, "1"));
    } else {
        FAIL("unknown project config error_kind: " << error_kind);
    }
}

} // namespace

TEST_CASE("ProjectBasicConfig loads project.json fixtures", "[project-format]") {
    ensureLogger();
    const auto expectations = readJson(fixtureRoot() / "expectations.json");
    ProjectSandbox sandbox;

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto scenario = readJson(fixtureRoot() / file);
            if (scenario.value("mode", std::string{}) == "project_config") {
                const auto &project = scenario.at("project");
                const auto expected = entry.at("expect").get<std::string>();
                if (expected == "ok") {
                    requireProjectConfigOk(project, sandbox);
                } else {
                    const auto error_kind = entry.at("error_kind").get<std::string>();
                    const auto message = projectConfigErrorMessage(project, sandbox, false);
                    REQUIRE_FALSE(message.empty());
                    requireErrorKind(message, error_kind);

                    if (error_kind == "engine_version") {
                        const auto ignored_message = projectConfigErrorMessage(project, sandbox, true);
                        REQUIRE(ignored_message.empty());
                    }
                }
            } else {
                SUCCEED("mode owned by another fixture test");
            }
        }
    }
}

// MG-2b acceptance 2: v0 ships one version of each format. Every shipped
// project is read through the production parsers -- the scene and asset_data
// its project.json names, and its rendering config through the loader that
// reads the authored file -- and the count of documents in an older form
// (a scene or asset_data other than version 2, a rendering root that is not
// `pelican.graph`) is zero. The scan also covers documents a project ships
// without naming them, so a stray older document cannot hide.
TEST_CASE("MG-2b every shipped project is a pelican.graph root with scene 2 and asset_data 2",
          "[project-config][mg2b][catalog]") {
    const auto projects =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects";
    std::vector<std::string> named;
    std::size_t graph_roots = 0;
    for (const auto &entry : std::filesystem::directory_iterator{projects}) {
        const auto manifest_path = entry.path() / "project.json";
        if (!entry.is_directory() || !std::filesystem::exists(manifest_path)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        CAPTURE(name);
        named.push_back(name);
        const auto basic = readJson(manifest_path).at("basic_config");

        const auto scene = readJson(
            entry.path() / basic.at("scene_data_json").get<std::string>());
        CHECK(scene.at("version") == 2);
        CHECK(normalizeSceneDataJson(scene).version == 2);

        const auto assets = readJson(
            entry.path() / basic.at("asset_data_json").get<std::string>());
        CHECK(assets.at("version") == 2);
        CHECK(parseAssetDataFormatJson(assets).version == 2);

        const auto rendering_path =
            entry.path() / basic.at("rendering_config_json").get<std::string>();
        std::ifstream rendering_file{rendering_path, std::ios::binary};
        const std::string rendering_bytes{
            std::istreambuf_iterator<char>{rendering_file},
            std::istreambuf_iterator<char>{}};
        const auto rendering = loadRenderingPassConfigJsonFromString(
            rendering_bytes, rendering_path.generic_string());
        CHECK(isRenderGraphRoot(rendering));
        CHECK(renderingSectionOf(rendering).is_object());
        if (isRenderGraphRoot(rendering)) ++graph_roots;
    }
    std::sort(named.begin(), named.end());
    CHECK(named == std::vector<std::string>{
                       "animgraph_demo", "example", "pathtrace_demo",
                       "posegraph_demo", "raytracing_demo", "sprite_demo",
                       "vrm_xr_demo"});
    CHECK(graph_roots == named.size());

    // Every JSON document shipped under projects/, named or not.
    std::map<std::string, std::size_t> older_documents;
    std::size_t scenes = 0;
    std::size_t asset_data = 0;
    std::size_t rendering_roots = 0;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator{projects}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        // Tool state (.pelican/ and the like) is not a shipped document.
        const auto relative_path = std::filesystem::relative(entry.path(), projects);
        if (std::any_of(relative_path.begin(), relative_path.end(),
                        [](const auto &part) {
                            return part.string().starts_with(".");
                        })) {
            continue;
        }
        const auto document = nlohmann::json::parse(
            std::ifstream{entry.path(), std::ios::binary}, nullptr, false);
        if (!document.is_object()) continue;
        const auto relative =
            std::filesystem::relative(entry.path(), projects).generic_string();
        const auto schema = document.value("schema", std::string{});
        if (schema == "pelican.scene") {
            ++scenes;
            if (document.value("version", 0) != 2) ++older_documents[relative];
        } else if (schema == "pelican.asset_data") {
            ++asset_data;
            if (document.value("version", 0) != 2) ++older_documents[relative];
        } else if (schema == "pelican.graph") {
            if (document.contains("rendering")) ++rendering_roots;
        } else if (schema.empty() &&
                   (document.contains("rendering_passes") ||
                    document.contains("pipeline") ||
                    document.contains("render_targets"))) {
            // The older rendering root: a config's own keys at the root.
            ++older_documents[relative];
        }
    }
    INFO("older documents: " << nlohmann::json(older_documents).dump());
    CHECK(older_documents.empty());
    CHECK(scenes == 7);
    CHECK(asset_data == 7);
    // The seven roots the projects name, and the example's second config.
    CHECK(rendering_roots == 8);
}

// MG-2b acceptance 1: a rendering root that is not a `pelican.graph` document
// is refused by name at every reader of an authored root -- the file loader,
// the section locator, the startup overlay, the ray tracing sidecar reader and
// the pipeline resolver. The same config under a graph root is read.
TEST_CASE("MG-2b a rendering root that is not pelican.graph is refused by name",
          "[project-config][mg2b][render-config][legacy-root]") {
    const nlohmann::json legacy{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features", nlohmann::json::array()},
    };
    nlohmann::json graph_root{
        {"schema", "pelican.graph"},     {"version", 1},
        {"definitions", nlohmann::json::object()},
        {"nodes", nlohmann::json::array()},
        {"outputs", nlohmann::json::object()},
        {"rendering", legacy},
    };
    const auto named = Catch::Matchers::StartsWith("render_config.legacy_root@1");

    CHECK_THROWS_WITH(loadRenderingPassConfigJsonFromString(legacy.dump(), "legacy.json"),
                      named);
    CHECK(loadRenderingPassConfigJsonFromString(graph_root.dump(), "graph.json") ==
          graph_root);

    auto legacy_copy = legacy;
    CHECK_THROWS_WITH(renderingSectionOf(legacy_copy), named);
    CHECK_THROWS_WITH(renderingSectionOrEmpty(legacy), named);
    CHECK(renderingSectionOf(graph_root) == legacy);

    const std::array<std::string, 1> overlays{"project://overlay.json"};
    const auto overlay_loader = [](std::string_view) {
        return std::string{
            R"json({"schema":"pelican.render_feature_overlay","version":1,"name":"tool","features":[]})json"};
    };
    CHECK_THROWS_WITH(applyRenderFeatureOverlays(legacy, overlays, overlay_loader), named);
    CHECK_NOTHROW(applyRenderFeatureOverlays(graph_root, overlays, overlay_loader));

    std::size_t sidecar_loads = 0;
    const auto sidecar_loader = [&](std::string_view) -> std::string {
        ++sidecar_loads;
        throw std::runtime_error("no sidecar in this fixture");
    };
    CHECK_THROWS_WITH(loadRayTracingProjectConfig(legacy, sidecar_loader, true), named);
    CHECK(loadRayTracingProjectConfig(graph_root, sidecar_loader, true) == nullptr);
    CHECK(sidecar_loads == 0);

    CHECK_THROWS_WITH(
        resolveRenderPipeline(RenderPipelineRequest{legacy, "legacy root"},
                              RenderEnvironmentCapabilities{}),
        named);
}

} // namespace Pelican
