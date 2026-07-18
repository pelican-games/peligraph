#include "../src/core/container.hpp"
#include "../src/core/loader/authoringscenedocument.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/project/sceneformat.hpp"
#include "../src/core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "project_format";
}

std::filesystem::path authoringFixturePath() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "authoring_scene" /
           "multi_scene_roundtrip.json";
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
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

size_t lightComponentCount(const nlohmann::json &scene) {
    size_t count = 0;
    for (const auto &object : scene.at("objects")) {
        for (const auto &component : object.at("components")) {
            if (component.at("name").get<std::string>() == "light") {
                ++count;
            }
        }
    }
    return count;
}

std::vector<std::string> lightObjectNames(const nlohmann::json &scene) {
    std::vector<std::string> names;
    for (const auto &object : scene.at("objects")) {
        for (const auto &component : object.at("components")) {
            if (component.at("name").get<std::string>() == "light") {
                names.push_back(object.value("name", std::string{}));
            }
        }
    }
    return names;
}

void requireErrorKind(std::string_view message, std::string_view error_kind) {
    if (error_kind == "schema") {
        REQUIRE(contains(message, "schema"));
        REQUIRE(contains(message, "pelican.scene"));
    } else if (error_kind == "version") {
        REQUIRE(contains(message, "version"));
        REQUIRE(contains(message, "1"));
    } else if (error_kind == "legacy_lights") {
        REQUIRE(contains(message, "'lights'"));
        REQUIRE(contains(message, "'light'"));
    } else if (error_kind == "duplicate_name") {
        REQUIRE(contains(message, "duplicate object name"));
    } else if (error_kind == "unknown_parent") {
        REQUIRE(contains(message, "unknown parent"));
        REQUIRE(contains(message, "MissingParent"));
        REQUIRE(contains(message, "Child"));
    } else if (error_kind == "ambiguous_parent") {
        REQUIRE(contains(message, "ambiguous parent"));
        REQUIRE(contains(message, "Parent"));
        REQUIRE(contains(message, "Child"));
    } else if (error_kind == "parent_cycle") {
        REQUIRE(contains(message, "parent cycle"));
        REQUIRE(contains(message, "NodeA"));
        REQUIRE(contains(message, "NodeB"));
        REQUIRE(contains(message, "NodeC"));
    } else {
        FAIL("unknown scene format error_kind: " << error_kind);
    }
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    file << text;
}

std::filesystem::path makeTempProjectDir() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path() / ("pelican_sceneformat_" + suffix);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("Scene format fixtures accept only v1 documents", "[scene-format]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto scenario = readJson(fixtureRoot() / file);
            if (scenario.value("mode", std::string{}) != "scene_format") {
                continue;
            }

            const auto expected = entry.at("expect").get<std::string>();
            if (expected == "ok") {
                const auto document = normalizeSceneDataJson(scenario.at("document"));
                const auto scene_id = scenario.at("scene_id").get<std::string>();
                REQUIRE(document.scenes.contains(scene_id));
                REQUIRE(document.warnings.size() == scenario.value("expected_warnings", 0));

                const auto &scene = document.scenes.at(scene_id);
                REQUIRE(scene.at("objects").size() == scenario.at("expected_object_count").get<size_t>());
                REQUIRE(lightComponentCount(scene) == scenario.at("expected_light_count").get<size_t>());

                if (scenario.contains("expected_light_object_names")) {
                    REQUIRE(lightObjectNames(scene) ==
                            scenario.at("expected_light_object_names").get<std::vector<std::string>>());
                }

                const auto authored =
                    AuthoringSceneDocument::load(scenario.at("document").dump(), SceneRevision{1});
                const auto encoded = authored.encodeSemantic();
                const auto fresh = AuthoringSceneDocument::load(encoded, SceneRevision{2});
                REQUIRE(fresh.rawJson() == authored.rawJson());
                REQUIRE(fresh.encodeSemantic() == encoded);
            } else {
                std::string message;
                try {
                    (void)normalizeSceneDataJson(scenario.at("document"));
                } catch (const std::exception &ex) {
                    message = ex.what();
                }
                REQUIRE_FALSE(message.empty());
                requireErrorKind(message, entry.at("error_kind").get<std::string>());
            }
        }
    }
}

TEST_CASE("AuthoringSceneDocument preserves every raw scene object and component", "[scene-format][authoring]") {
    const auto source = readJson(authoringFixturePath());
    const auto document = AuthoringSceneDocument::load(source.dump(2), SceneRevision{41});

    REQUIRE(document.revision().value == 41);
    REQUIRE(document.rawJson() == source);
    REQUIRE(document.objectCount() == 3);
    REQUIRE(document.rawJson().contains("editor_envelope"));

    const auto first_query = document.query();
    const auto second_query = document.query();
    REQUIRE(first_query.size() == 2);
    REQUIRE(second_query.size() == first_query.size());

    std::unordered_set<std::uint64_t> object_ids;
    std::unordered_set<std::string> component_names;
    std::size_t unnamed_count = 0;
    for (std::size_t scene_index = 0; scene_index < first_query.size(); ++scene_index) {
        REQUIRE(first_query[scene_index].scene_id == second_query[scene_index].scene_id);
        REQUIRE(first_query[scene_index].authoredJson() == second_query[scene_index].authoredJson());
        for (std::size_t object_index = 0; object_index < first_query[scene_index].objects.size(); ++object_index) {
            const auto &object = first_query[scene_index].objects[object_index];
            const auto &second_object = second_query[scene_index].objects[object_index];
            REQUIRE(object.authoring_object_id == second_object.authoring_object_id);
            REQUIRE(object.authoring_object_id.value != 0);
            REQUIRE(object_ids.insert(object.authoring_object_id.value).second);
            REQUIRE_FALSE(object.runtime_entity_id.has_value());
            if (!object.name) {
                ++unnamed_count;
            }
            for (const auto &component : object.components) {
                const auto component_name = component.authoredJson().at("name").get<std::string>();
                component_names.insert(component_name);
                if (component_name == "unknown_read_only") {
                    REQUIRE(component.codec.state == ComponentCodecState::Missing);
                    REQUIRE_FALSE(component.codec.editable);
                    REQUIRE(component.codec.codec_name.empty());
                } else if (component_name == "transform" || component_name == "light" ||
                           component_name == "collider") {
                    REQUIRE(component.codec.state == ComponentCodecState::Registered);
                    REQUIRE(component.codec.editable);
                    REQUIRE(std::string{component.codec.codec_name} == component_name);
                }
            }
        }
    }
    REQUIRE(unnamed_count == 1);
    REQUIRE(component_names.contains("light"));
    REQUIRE(component_names.contains("collider"));
    REQUIRE(component_names.contains("unknown_read_only"));

    const auto encoded = document.encodeSemantic();
    REQUIRE(document.encodeSemantic() == encoded);
    const auto fresh = AuthoringSceneDocument::load(encoded, SceneRevision{42}, 100);
    REQUIRE(fresh.rawJson() == source);
    REQUIRE(fresh.encodeSemantic() == encoded);
}

TEST_CASE("ProjectBasicConfig has one authoring document cache authority", "[scene-format][authoring]") {
    ensureLogger();
    auto temp_dir = makeTempProjectDir();

    try {
        const auto source = readJson(authoringFixturePath());
        writeText(temp_dir / "scene.json", source.dump(2));
        const auto project = nlohmann::json{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "authoring-cache-test"},
            {"engine_min_version", "0.1.0"},
            {"basic_config",
             {
                 {"default_scene_id", "main"},
                 {"scene_data_json", "scene.json"},
             }},
        };

        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(temp_dir, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            auto &config = GET_MODULE(ProjectBasicConfig);

            const auto &first = config.sceneDocument();
            REQUIRE(&config.sceneDocument() == &first);
            const auto first_revision = first.revision().value;
            std::uint64_t max_first_object_id = 0;
            for (const auto &scene : first.query()) {
                for (const auto &object : scene.objects) {
                    max_first_object_id = std::max(max_first_object_id, object.authoring_object_id.value);
                }
            }

            auto updated_json = first.rawJson();
            updated_json["cache_update_marker"] = true;
            config.updateSceneDocument(updated_json.dump());
            const auto &updated = config.sceneDocument();
            REQUIRE(updated.revision().value > first_revision);
            REQUIRE(updated.rawJson().at("cache_update_marker") == true);
            REQUIRE(config.sceneDataJson() == updated.encodeSemantic());
            for (const auto &scene : updated.query()) {
                for (const auto &object : scene.objects) {
                    REQUIRE(object.authoring_object_id.value > max_first_object_id);
                }
            }

            const auto updated_revision = updated.revision().value;
            const auto *updated_address = &updated;
            REQUIRE_THROWS_AS(config.updateSceneDocument("{not-json"), nlohmann::json::parse_error);
            REQUIRE(&config.sceneDocument() == updated_address);
            REQUIRE(config.sceneDocument().revision().value == updated_revision);

            config.invalidateSceneDocument();
            const auto &reloaded = config.sceneDocument();
            REQUIRE(reloaded.revision().value > updated_revision);
            REQUIRE(reloaded.rawJson() == source);
            REQUIRE_FALSE(reloaded.rawJson().contains("cache_update_marker"));
        }
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Scene format rejects legacy lights and names the v1 replacement", "[scene-format]") {
    const auto legacy_lights = nlohmann::json::parse(R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "lights": [{"name": "BlueSpot", "type": "spot"}],
      "objects": [
        {
          "name": "Hero",
          "components": [
            {"name": "transform", "pos": [0, 0, 0]}
          ]
        }
      ]
    }
  }
})json");

    std::string message;
    try {
        (void)normalizeSceneDataJson(legacy_lights);
    } catch (const std::exception &ex) {
        message = ex.what();
    }
    REQUIRE(contains(message, "'lights'"));
    REQUIRE(contains(message, "'light'"));
}

TEST_CASE("SceneLoader reports object name for unknown component names", "[scene-format]") {
    ensureLogger();
    auto temp_dir = makeTempProjectDir();

    try {
        writeText(temp_dir / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "NamedObject",
          "components": [
            {"name": "missingcomponent"}
          ]
        }
      ]
    }
  }
})json");

        const auto project = nlohmann::json{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "scene-loader-error-test"},
            {"engine_min_version", "0.1.0"},
            {"basic_config",
             {
                 {"default_scene_id", "default_scene"},
                 {"scene_data_json", "scene.json"},
             }},
        };

        FastModuleContainer modules;
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(project.dump());

        std::string message;
        try {
            GET_MODULE(SceneLoader).load("default_scene");
        } catch (const std::exception &ex) {
            message = ex.what();
        }
        REQUIRE(contains(message, "missingcomponent"));
        REQUIRE(contains(message, "NamedObject"));
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }

    std::filesystem::remove_all(temp_dir);
}

} // namespace Pelican
