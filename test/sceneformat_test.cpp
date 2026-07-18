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
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

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

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open text fixture: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
}

std::filesystem::path makeTempProjectDir() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path() / ("pelican_sceneformat_" + suffix);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path currentTestExecutable() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
    if (size == 0 || size == buffer.size()) {
        throw std::runtime_error("failed to resolve current test executable");
    }
    buffer.resize(size);
    return std::filesystem::path{buffer};
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        throw std::runtime_error("failed to resolve current test executable: " +
                                 error.message());
    }
    return path;
#endif
}

void setProcessEnvironment(const char *name, const std::string &value) {
#ifdef _WIN32
    if (_putenv_s(name, value.c_str()) != 0) {
        throw std::runtime_error("failed to set child-process environment");
    }
#else
    if (setenv(name, value.c_str(), 1) != 0) {
        throw std::runtime_error("failed to set child-process environment");
    }
#endif
}

void clearProcessEnvironment(const char *name) noexcept {
#ifdef _WIN32
    (void)_putenv_s(name, "");
#else
    (void)unsetenv(name);
#endif
}

int runNewProcessProbe(const std::filesystem::path &executable) {
#ifdef _WIN32
    auto command_line = L"\"" + executable.wstring() +
                        L"\" \"[wp166-new-process-probe]\" -r compact";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr,
                        nullptr, TRUE, 0, nullptr, nullptr, &startup,
                        &process)) {
        throw std::runtime_error("failed to launch new-process reload probe: " +
                                 std::to_string(GetLastError()));
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    (void)GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
#else
    const auto command = "\"" + executable.string() +
                         "\" \"[wp166-new-process-probe]\" -r compact";
    return std::system(command.c_str());
#endif
}

std::size_t temporarySceneFileCount(const std::filesystem::path &directory) {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().filename().string().find(".pelican-save-") !=
            std::string::npos) {
            ++count;
        }
    }
    return count;
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

TEST_CASE("SAVE0 is failure-atomic at every prepare point and reloads semantically",
          "[scene-format][authoring][save][wp166]") {
    ensureLogger();
    auto temp_dir = makeTempProjectDir();

    try {
        auto source = readJson(authoringFixturePath());
        const auto baseline_bytes = source.dump(2);
        const auto scene_path = temp_dir / "scene.json";
        writeText(scene_path, baseline_bytes);
        const auto project = nlohmann::json{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "save0-atomic-test"},
            {"engine_min_version", "0.1.0"},
            {"basic_config",
             {{"default_scene_id", "main"},
              {"scene_data_json", "scene.json"}}},
        };

        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(temp_dir, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            auto &config = GET_MODULE(ProjectBasicConfig);
            (void)config.sceneDocument();

            source["editor_envelope"]["save_marker"] = "wp166";
            config.updateSceneDocument(source.dump());
            const auto expected_semantic = config.sceneDataJson();
            const auto old_revision = config.sceneDocument().revision();
            const auto *old_cache = &config.sceneDocument();

            // A byte-only external edit is still an external modification:
            // SAVE0 must not silently replace formatting or comments/metadata
            // from a source it did not load.
            const auto externally_modified = baseline_bytes + "\n";
            writeText(scene_path, externally_modified);
            std::optional<SceneSaveErrorCode> external_error;
            try {
                (void)config.saveSceneDocument();
            } catch (const SceneSaveError &error) {
                external_error = error.code();
            }
            REQUIRE(external_error == SceneSaveErrorCode::ExternalModification);
            REQUIRE(readText(scene_path) == externally_modified);
            REQUIRE(&config.sceneDocument() == old_cache);
            REQUIRE(config.sceneDocument().revision() == old_revision);
            REQUIRE(config.sceneDataJson() == expected_semantic);
            writeText(scene_path, baseline_bytes);

            constexpr std::array fault_points{
                SceneSaveFaultPoint::AfterEncode,
                SceneSaveFaultPoint::AfterDiskDigest,
                SceneSaveFaultPoint::AfterTemporaryWrite,
                SceneSaveFaultPoint::AfterTemporaryValidation,
                SceneSaveFaultPoint::AfterCachePrepare,
                SceneSaveFaultPoint::BeforeReplace,
            };
            for (const auto point : fault_points) {
                DYNAMIC_SECTION("fault point " << static_cast<unsigned>(point)) {
                    config.setSceneSaveFaultForTesting(point);
                    REQUIRE_THROWS(config.saveSceneDocument());
                    REQUIRE(readText(scene_path) == baseline_bytes);
                    REQUIRE(&config.sceneDocument() == old_cache);
                    REQUIRE(config.sceneDocument().revision() == old_revision);
                    REQUIRE(config.sceneDataJson() == expected_semantic);
                    REQUIRE(temporarySceneFileCount(temp_dir) == 0);
                }
            }
            config.setSceneSaveFaultForTesting(std::nullopt);

            const auto saved = config.saveSceneDocument();
            REQUIRE(saved.scene_revision.value == old_revision.value + 1U);
            REQUIRE(saved.byte_count == expected_semantic.size());
            REQUIRE(saved.digest.size() == 64);
            REQUIRE(readText(scene_path) == expected_semantic);
            REQUIRE(config.sceneDataJson() == expected_semantic);
            REQUIRE(config.sceneDocument().revision() == saved.scene_revision);
            REQUIRE(temporarySceneFileCount(temp_dir) == 0);

            // Same-process explicit reload uses the replaced file and rebuilds
            // the whole cache, including non-current scenes and raw components.
            config.invalidateSceneDocument();
            const auto &same_process = config.sceneDocument();
            REQUIRE(same_process.encodeSemantic() == expected_semantic);
            REQUIRE(same_process.rawJson() == source);
            REQUIRE(same_process.query().size() == 2);

            const auto expected_path = temp_dir / "expected-semantic.json";
            writeText(expected_path, expected_semantic);
            setProcessEnvironment("PELICAN_WP166_SCENE_PATH",
                                  scene_path.string());
            setProcessEnvironment("PELICAN_WP166_EXPECTED_PATH",
                                  expected_path.string());
            const auto executable = currentTestExecutable();
            const auto child_result = runNewProcessProbe(executable);
            clearProcessEnvironment("PELICAN_WP166_SCENE_PATH");
            clearProcessEnvironment("PELICAN_WP166_EXPECTED_PATH");
            REQUIRE(child_result == 0);
        }
    } catch (...) {
        clearProcessEnvironment("PELICAN_WP166_SCENE_PATH");
        clearProcessEnvironment("PELICAN_WP166_EXPECTED_PATH");
        std::filesystem::remove_all(temp_dir);
        throw;
    }

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("SNAPSHOT0 cache replacement is rollback-safe and never writes the scene file",
          "[scene-format][authoring][snapshot][import][fault][wp168]") {
    ensureLogger();
    auto temp_dir = makeTempProjectDir();

    try {
        const auto disk_document = readJson(authoringFixturePath());
        const auto disk_bytes = disk_document.dump(2);
        const auto scene_path = temp_dir / "scene.json";
        writeText(scene_path, disk_bytes);
        const auto project = nlohmann::json{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "snapshot0-atomic-test"},
            {"engine_min_version", "0.1.0"},
            {"basic_config",
             {{"default_scene_id", "main"},
              {"scene_data_json", "scene.json"}}},
        };

        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(temp_dir, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            auto &config = GET_MODULE(ProjectBasicConfig);
            const auto &old_document = config.sceneDocument();
            const auto *old_cache = &old_document;
            const auto old_revision = old_document.revision();
            const auto old_semantic = old_document.encodeSemantic();
            std::uint64_t old_max_object_id = 0;
            for (const auto &scene : old_document.query()) {
                for (const auto &object : scene.objects) {
                    old_max_object_id = std::max(
                        old_max_object_id, object.authoring_object_id.value);
                }
            }

            auto snapshot = disk_document;
            snapshot["editor_envelope"]["snapshot_marker"] = "wp168";
            const auto snapshot_bytes = snapshot.dump();
            const auto require_old_state = [&] {
                REQUIRE(&config.sceneDocument() == old_cache);
                REQUIRE(config.sceneDocument().revision() == old_revision);
                REQUIRE(config.sceneDocument().encodeSemantic() == old_semantic);
                REQUIRE(readText(scene_path) == disk_bytes);
            };

            std::size_t reload_calls = 0;
            for (const auto point :
                 {SceneImportFaultPoint::AfterCandidatePrepare,
                  SceneImportFaultPoint::AfterPublication}) {
                DYNAMIC_SECTION("fault point " << static_cast<unsigned>(point)) {
                    config.setSceneImportFaultForTesting(point);
                    REQUIRE_THROWS(config.importSceneDocument(
                        snapshot_bytes, [&] { ++reload_calls; }));
                    require_old_state();
                    REQUIRE(reload_calls == 0);
                }
            }
            config.setSceneImportFaultForTesting(std::nullopt);

            std::optional<std::string> loader_error;
            try {
                (void)config.importSceneDocument(snapshot_bytes, [&] {
                    ++reload_calls;
                    REQUIRE(config.sceneDocument().rawJson() == snapshot);
                    throw std::runtime_error("injected loader fault");
                });
            } catch (const std::runtime_error &error) {
                loader_error = error.what();
            }
            REQUIRE(loader_error == "injected loader fault");
            REQUIRE(reload_calls == 1);
            require_old_state();

            const auto imported = config.importSceneDocument(snapshot_bytes, [&] {
                ++reload_calls;
                REQUIRE(config.sceneDocument().rawJson() == snapshot);
            });
            REQUIRE(reload_calls == 2);
            REQUIRE(imported.value == old_revision.value + 1U);
            REQUIRE(config.sceneDocument().revision() == imported);
            REQUIRE(config.sceneDocument().rawJson() == snapshot);
            REQUIRE(readText(scene_path) == disk_bytes);
            for (const auto &scene : config.sceneDocument().query()) {
                for (const auto &object : scene.objects) {
                    REQUIRE(object.authoring_object_id.value >
                            old_max_object_id);
                }
            }
        }
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("SAVE0 new-process semantic reload probe",
          "[.][wp166-new-process-probe]") {
    const auto *scene_path = std::getenv("PELICAN_WP166_SCENE_PATH");
    const auto *expected_path = std::getenv("PELICAN_WP166_EXPECTED_PATH");
    if (scene_path == nullptr || expected_path == nullptr) {
        SKIP("new-process probe is launched by the SAVE0 parent fixture");
    }
    const auto disk_bytes = readText(scene_path);
    const auto expected_bytes = readText(expected_path);
    const auto fresh_process = AuthoringSceneDocument::load(
        disk_bytes, SceneRevision{1});
    REQUIRE(fresh_process.encodeSemantic() == expected_bytes);
    REQUIRE(fresh_process.rawJson() == nlohmann::json::parse(expected_bytes));
    REQUIRE(fresh_process.query().size() == 2);
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
