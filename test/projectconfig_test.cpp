#include "../src/core/container.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
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

        writeText(root / "scenes" / "main.scene.json", R"json({"scene-ok":true})json");
        writeText(root / "assets" / "model.glb", "fake model");
        writeText(root / "assets" / "image.png", "fake image");
        writeText(root / "assets" / "assets.json", R"json({
  "models": [
    {"name": "model", "path": "assets/model.glb"}
  ]
})json");
        writeText(root / "shaders" / "fullscreen.vert.spv", "fake vertex shader");
        writeText(root / "shaders" / "fullscreen.frag.spv", "fake fragment shader");
        writeText(root / "passes" / "main.json", R"json({
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "fullscreen",
          "type": "fullscreen",
          "shader": {
            "vertex": "shaders/fullscreen.vert.spv",
            "fragment": "shaders/fullscreen.frag.spv"
          }
        }
      ]
    }
  ]
})json");
        writeText(root / "ui" / "ui.json", R"json({
  "images": [
    {"name": "image", "file": "assets/image.png"}
  ]
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
    const auto shader =
        rendering.at("rendering_passes").at(0).at("passes").at(0).at("shader");
    REQUIRE(shader.at("vertex").get<std::string>() == "shaders/fullscreen.vert.spv");
    REQUIRE(shader.at("fragment").get<std::string>() == "shaders/fullscreen.frag.spv");

    const auto ui = nlohmann::json::parse(config.uiConfigJson());
    REQUIRE(ui.at("images").at(0).at("file").get<std::string>() ==
            weaklyCanonical(sandbox.root / "assets" / "image.png").string());
}

void requireErrorKind(std::string_view message, std::string_view error_kind) {
    if (error_kind == "engine_version") {
        REQUIRE(contains(message, "engine_min_version"));
    } else if (error_kind == "schema") {
        REQUIRE(contains(message, "schema"));
    } else if (error_kind == "version") {
        REQUIRE(contains(message, "version"));
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
            }
        }
    }
}

} // namespace Pelican
