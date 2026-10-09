#include "../src/core/communication/editorjournal.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/authoringscenedocument.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/userpublic/gameobjects.hpp"
#include "scene_load_test_support.hpp"
#include "../src/project/sceneformat.hpp"
#include "../src/core/log.hpp"
#include "authoringscenetestsupport.hpp"
#include "graph_composition_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/Sink.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
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
        // MG-2b: every version but 2 -- including 1 -- is refused by name.
        REQUIRE(contains(message, "pelican.scene.version_unsupported@1"));
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

TEST_CASE("Scene format fixtures accept only v2 documents", "[scene-format]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto scenario = readJson(fixtureRoot() / file);
            if (scenario.value("mode", std::string{}) != "scene_format") {
                // Another fixture test owns this mode. Say so with an
                // assertion: a silent fall-through reports "passed" for a
                // fixture nobody looked at, which is what -w NoAssertions is
                // here to stop. "project format fixture modes are all covered"
                // is what proves someone does look at it.
                SUCCEED("mode owned by another fixture test");
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
                const auto encoded =
                    test_support::authoring(authored).encodeSemantic();
                const auto fresh = AuthoringSceneDocument::load(encoded, SceneRevision{2});
                REQUIRE(test_support::authoring(fresh).rawJson() ==
                        test_support::authoring(authored).rawJson());
                REQUIRE(test_support::authoring(fresh).encodeSemantic() ==
                        encoded);
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

TEST_CASE("Unnamed runtime identities are stable and cannot collide with authored names",
          "[scene-format][authoring][identity][wp244]") {
    REQUIRE(runtimeObjectIdentityName("main", AuthoringObjectId{7},
                                      "Player") == "Player");
    REQUIRE(runtimeObjectIdentityName("main", AuthoringObjectId{7}, {}) ==
            "pelican://scene/main/authoring-object/7");
    REQUIRE(runtimeObjectIdentityName("main", AuthoringObjectId{7}, {}) !=
            runtimeObjectIdentityName("main", AuthoringObjectId{8}, {}));
    REQUIRE_THROWS_AS(
        runtimeObjectIdentityName("main", AuthoringObjectId{}, {}),
        std::invalid_argument);
}

TEST_CASE("AuthoringSceneDocument preserves every raw scene object and component", "[scene-format][authoring]") {
    const auto source = readJson(authoringFixturePath());
    const auto document = AuthoringSceneDocument::load(source.dump(2), SceneRevision{41});

    REQUIRE(document.revision().value == 41);
    REQUIRE(test_support::authoring(document).rawJson() == source);
    REQUIRE(document.objectCount() == 3);
    REQUIRE(test_support::authoring(document)
                .rawJson()
                .contains("editor_envelope"));

    const auto first_query = test_support::authoring(document).query();
    const auto second_query = test_support::authoring(document).query();
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

    const auto encoded = test_support::authoring(document).encodeSemantic();
    REQUIRE(test_support::authoring(document).encodeSemantic() == encoded);
    const auto fresh = AuthoringSceneDocument::load(encoded, SceneRevision{42}, 100);
    REQUIRE(test_support::authoring(fresh).rawJson() == source);
    REQUIRE(test_support::authoring(fresh).encodeSemantic() == encoded);
}

TEST_CASE("Persistent scene identities survive reorder and do not reuse deleted IDs",
          "[scene-format][authoring][persistent-identity]") {
    const auto source = nlohmann::json::parse(R"({"schema":"pelican.scene","version":2,
        "scenes":{"main":{"objects":[{"name":"First","components":[]},
        {"name":"Owner","components":[]}]},
        "secondary":{"objects":[{"name":"Last","components":[]}]}}})");
    const auto original = AuthoringSceneDocument::load(source.dump(), SceneRevision{1});
    auto stage = AuthoringSceneAuthority::structuralStage(original);
    stage.removeObject(AuthoringObjectId{3});
    stage.reorderObject(AuthoringObjectId{2}, 0);
    stage.renameObject(AuthoringObjectId{2}, "Renamed");
    const auto edited = std::move(stage).finish(SceneRevision{2});
    const auto bytes = AuthoringSceneAuthority::encodePersistent(edited);
    const auto reopened = AuthoringSceneDocument::load(bytes, SceneRevision{1}, 1000);
    const auto scenes = AuthoringSceneAuthority::query(reopened);
    REQUIRE(scenes[0].objects[0].name == "Renamed");
    REQUIRE(scenes[0].objects[0].authoring_object_id == AuthoringObjectId{2});
    REQUIRE(scenes[0].objects[1].authoring_object_id == AuthoringObjectId{1});
    REQUIRE(scenes[1].objects.empty());
    REQUIRE(AuthoringSceneAuthority::encodePersistent(reopened) == bytes);
    auto next = AuthoringSceneAuthority::structuralStage(reopened);
    REQUIRE(next.insertObject("secondary", 0, {{"components", nlohmann::json::array()}}) ==
            AuthoringObjectId{4});

    // Portable snapshots keep the established fresh-identity contract.
    const auto snapshot = AuthoringSceneAuthority::encodeSemantic(edited);
    REQUIRE_FALSE(nlohmann::json::parse(snapshot).contains("next_authoring_object_id"));
    const auto imported = AuthoringSceneDocument::load(snapshot, SceneRevision{1}, 100);
    REQUIRE(AuthoringSceneAuthority::query(imported)[0].objects[0].authoring_object_id ==
            AuthoringObjectId{100});
    const auto explicit_import = AuthoringSceneDocument::load(
        bytes, SceneRevision{1}, 100, AuthoringIdentityPolicy::AllocateFresh);
    REQUIRE(AuthoringSceneAuthority::query(explicit_import)[0].objects[0].authoring_object_id ==
            AuthoringObjectId{100});
    REQUIRE(AuthoringSceneAuthority::encodeSemantic(explicit_import) == snapshot);
}

TEST_CASE("Scene identity metadata rejects ambiguous IDs and protects the allocation boundary",
          "[scene-format][authoring][persistent-identity]") {
    using Json = nlohmann::json;
    const auto source = Json::parse(R"({"schema":"pelican.scene","version":2,
        "next_authoring_object_id":10,"scenes":{
        "main":{"objects":[{"authoring_object_id":2,"components":[]}]},
        "secondary":{"objects":[{"components":[]}]}}})");
    SECTION("unidentified additions allocate above persisted high water in every scene") {
        const auto document = AuthoringSceneDocument::load(source.dump(), SceneRevision{1});
        const auto scenes = AuthoringSceneAuthority::query(document);
        REQUIRE(scenes[0].objects[0].authoring_object_id == AuthoringObjectId{2});
        REQUIRE(scenes[1].objects[0].authoring_object_id == AuthoringObjectId{10});
        REQUIRE(Json::parse(AuthoringSceneAuthority::encodePersistent(document))
                    .at("next_authoring_object_id") == 11);
    }
    SECTION("IDs must be positive exact integers with a representable successor") {
        for (const Json &invalid : std::vector<Json>{0, -1, 2.0, "2", true, nullptr,
                 std::numeric_limits<std::uint64_t>::max()}) {
            auto bad = source;
            bad["scenes"]["main"]["objects"][0]["authoring_object_id"] = invalid;
            CAPTURE(invalid.dump());
            REQUIRE_THROWS(normalizeSceneDataJson(bad));
            REQUIRE_THROWS(AuthoringSceneDocument::load(bad.dump(), SceneRevision{1}));
        }
    }
    SECTION("duplicates across different scenes are rejected") {
        auto bad = source;
        bad["scenes"]["secondary"]["objects"][0]["authoring_object_id"] = 2;
        REQUIRE_THROWS_WITH(normalizeSceneDataJson(bad), "duplicate authoring_object_id: 2");
    }
    SECTION("next identity must be exact and exceed all claimed identities") {
        for (const Json &invalid : std::vector<Json>{0, -1, 1, 2, 10.0, "10", true, nullptr}) {
            auto bad = source;
            bad["next_authoring_object_id"] = invalid;
            CAPTURE(invalid.dump());
            REQUIRE_THROWS(normalizeSceneDataJson(bad));
        }
    }
    SECTION("exhaustion cannot wrap around to a live ID") {
        auto exhausted = source;
        exhausted["scenes"].erase("secondary");
        exhausted["next_authoring_object_id"] = std::numeric_limits<std::uint64_t>::max();
        const auto document = AuthoringSceneDocument::load(exhausted.dump(), SceneRevision{1});
        auto stage = AuthoringSceneAuthority::structuralStage(document);
        REQUIRE_THROWS_AS(stage.insertObject("main", 1, {{"components", Json::array()}}),
                          std::overflow_error);
        exhausted["scenes"]["main"]["objects"].push_back({{"components", Json::array()}});
        REQUIRE_THROWS_AS(normalizeSceneDataJson(exhausted), std::overflow_error);
    }
    SECTION("projection and structural raw edits cannot forge persisted identities") {
        const auto document = AuthoringSceneDocument::load(source.dump(), SceneRevision{1});
        auto raw = AuthoringSceneAuthority::rawView(document).documentJson();
        raw["scenes"]["main"]["objects"][0]["authoring_object_id"] = 99;
        REQUIRE_THROWS_AS(AuthoringSceneAuthority::stage(document, raw, SceneRevision{2}),
                          std::invalid_argument);
        auto stage = AuthoringSceneAuthority::structuralStage(document);
        stage.rawJson() = raw;
        REQUIRE_THROWS_AS(std::move(stage).finish(SceneRevision{2}), std::invalid_argument);
    }
}

TEST_CASE("Saving after removing an earlier object preserves the graph owner on reopen",
          "[scene-format][authoring][save][graph-owner][persistent-identity]") {
    ensureLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        const auto source = nlohmann::json::parse(R"({"schema":"pelican.scene","version":2,
            "scenes":{"main":{"objects":[
            {"name":"Before","components":[{"name":"transform"}]},
            {"name":"Hero","components":[{"name":"transform"}]},
            {"name":"Other","components":[{"name":"transform"}]}]}}})");
        writeText(temp_dir / "scene.json", source.dump());
        writeText(temp_dir / "passes/main_rendering_config.json",
                  R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})");
        const nlohmann::json project{
            {"schema", "pelican.project"}, {"version", 1},
            {"engine_min_version", "0.1.0"},
            {"basic_config", {{"default_scene_id", "main"}, {"scene_data_json", "scene.json"}}}};
        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(temp_dir, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            auto &config = GET_MODULE(ProjectBasicConfig);
            // Even an unchanged legacy scene gets IDs on the first save.
            (void)config.saveSceneDocument();
            REQUIRE(readJson(temp_dir / "scene.json").at("next_authoring_object_id") == 4);
            auto stage = AuthoringSceneAuthority::structuralStage(config.sceneDocument());
            stage.removeObject(AuthoringObjectId{1});
            const auto edited = std::move(stage).finish(
                SceneRevision{config.sceneDocument().revision().value + 1});
            config.updateSceneDocument(AuthoringSceneAuthority::encodePersistent(edited));
            (void)config.saveSceneDocument();
        }
        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(temp_dir, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            GET_MODULE(ECSPredefinedRegistration).reg();
            auto &loader = GET_MODULE(SceneLoader);
            test_support::loadScene(loader, SceneId{"main"});
            const auto hero = loader.objectId("Hero");
            REQUIRE(hero);
            REQUIRE(loader.objectId("Other"));
            REQUIRE(loader.graphOwnerObject(GraphInstanceOwner{"main", 2}) == hero);
            REQUIRE(loader.graphOwnerObject(GraphInstanceOwner{"main", 3}) == loader.objectId("Other"));
            REQUIRE_FALSE(loader.graphOwnerObject(GraphInstanceOwner{"main", 1}));
        }
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }
    std::filesystem::remove_all(temp_dir);
}

// OL-1 acceptance 2. A legacy scene hands out ids by position, and the graph
// owner names Hero by that position. The editor journal then duplicates an
// object declared before Hero and inserts another at the front, SAVE0 writes
// the scene, and a fresh loader reopens it: the owner still resolves to Hero.
// Without the ids in the file, Hero's position (and so its id) would move.
TEST_CASE("OL-1 an owner keeps its object after duplicating and inserting before it, saving and reopening",
          "[ol1][scene-format][authoring][save][graph-owner][persistent-identity]") {
    ensureLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        const auto source = nlohmann::json::parse(R"({"schema":"pelican.scene","version":2,
            "scenes":{"main":{"objects":[
            {"name":"Before","components":[{"name":"transform"}]},
            {"name":"Hero","components":[{"name":"transform"}]},
            {"name":"Other","components":[{"name":"transform"}]}]}}})");
        writeText(temp_dir / "scene.json", source.dump());
        writeText(temp_dir / "passes/main_rendering_config.json",
                  R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})");
        const nlohmann::json project{
            {"schema", "pelican.project"}, {"version", 1},
            {"engine_min_version", "0.1.0"},
            {"basic_config", {{"default_scene_id", "main"}, {"scene_data_json", "scene.json"}}}};
        const GraphInstanceOwner hero_owner{"main", 2};
        std::uint64_t copy_id = 0;
        std::uint64_t inserted_id = 0;
        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(temp_dir, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            auto &config = GET_MODULE(ProjectBasicConfig);
            const auto initial = AuthoringSceneAuthority::query(config.sceneDocument());
            REQUIRE(initial.front().objects.at(1).name == "Hero");
            REQUIRE(initial.front().objects.at(1).authoring_object_id.value == hero_owner.object);

            ProjectBasicConfigProjectionTarget target{config};
            EditorEditCoordinator edits{EditorEditRuntimeDependencies{
                .document = [&]() -> const AuthoringSceneDocument & {
                    return config.sceneDocument();
                },
                .current_scene_id = [] { return std::string{"main"}; },
                .execute = [&](const EditorEditExecutionRequest &request) {
                    EditorProjectionTransaction transaction{target, request.base_revision};
                    std::vector<EditorProjectionAdapter *> adapters;
                    return transaction.commit(request.commands, adapters);
                },
                .gate = [] { return EditorGateObservation{}; },
            }};
            const auto actor =
                edits.openSession({{"display_name", "OL-1 owner"}}).at("actor_id");
            const auto commit = [&](nlohmann::json operation) {
                const auto accepted = edits.enqueue(
                    {{"actor_id", actor},
                     {"base_revision", config.sceneDocument().revision().value},
                     {"operations", nlohmann::json::array({std::move(operation)})}});
                REQUIRE(accepted.at("status") == "accepted");
                edits.commitPending();
                const auto result = edits.getResult({{"ticket", accepted.at("ticket")}});
                INFO(result.dump(2));
                REQUIRE(result.at("status") == "committed");
                return result;
            };
            const auto duplicated = commit({{"op", "duplicate"}, {"object_id", 1}});
            copy_id = duplicated.at("created_objects").at(0).at("object_id").get<std::uint64_t>();
            CHECK(duplicated.at("created_objects").at(0).at("declaration_index") == 1);
            const auto spawned = commit(
                {{"op", "spawn"},
                 {"scene_id", "main"},
                 {"declaration_index", 0},
                 {"object", {{"name", "Inserted"},
                             {"components", nlohmann::json::array({{{"name", "transform"}}})}}}});
            inserted_id = spawned.at("created_objects").at(0).at("object_id").get<std::uint64_t>();
            // Hero is now fourth in the file; by position it would be id 4.
            const auto edited = AuthoringSceneAuthority::query(config.sceneDocument());
            REQUIRE(edited.front().objects.at(3).name == "Hero");
            REQUIRE(edited.front().objects.at(3).authoring_object_id.value == hero_owner.object);

            // The production SAVE0 path, the one save_scene runs.
            (void)config.saveSceneDocument();
            const auto saved = readJson(temp_dir / "scene.json");
            const auto &objects = saved.at("scenes").at("main").at("objects");
            REQUIRE(objects.at(3).at("name") == "Hero");
            CHECK(objects.at(3).at("authoring_object_id") == hero_owner.object);
        }
        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(temp_dir, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            GET_MODULE(ECSPredefinedRegistration).reg();
            auto &loader = GET_MODULE(SceneLoader);
            test_support::loadScene(loader, SceneId{"main"});
            const auto hero = loader.objectId("Hero");
            REQUIRE(hero);
            CHECK(loader.graphOwnerObject(hero_owner) == hero);
            CHECK(loader.graphOwnerObject(GraphInstanceOwner{"main", 1}) ==
                  loader.objectId("Before"));
            CHECK(loader.graphOwnerObject(GraphInstanceOwner{"main", copy_id}) ==
                  loader.objectId("Before_copy1"));
            CHECK(loader.graphOwnerObject(GraphInstanceOwner{"main", inserted_id}) ==
                  loader.objectId("Inserted"));
        }
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }
    std::filesystem::remove_all(temp_dir);
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
            for (const auto &scene : test_support::authoring(first).query()) {
                for (const auto &object : scene.objects) {
                    max_first_object_id = std::max(max_first_object_id, object.authoring_object_id.value);
                }
            }

            auto updated_json = test_support::authoring(first).rawJson();
            updated_json["cache_update_marker"] = true;
            config.updateSceneDocument(updated_json.dump());
            const auto &updated = config.sceneDocument();
            REQUIRE(updated.revision().value > first_revision);
            REQUIRE(test_support::authoring(updated)
                        .rawJson()
                        .at("cache_update_marker") == true);
            REQUIRE(config.sceneDataJson() ==
                    test_support::authoring(updated).encodeSemantic());
            for (const auto &scene : test_support::authoring(updated).query()) {
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
            REQUIRE(test_support::authoring(reloaded).rawJson() == source);
            REQUIRE_FALSE(test_support::authoring(reloaded)
                              .rawJson()
                              .contains("cache_update_marker"));
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
            const auto expected_persisted =
                AuthoringSceneAuthority::encodePersistent(config.sceneDocument());
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
            REQUIRE(saved.byte_count == expected_persisted.size());
            REQUIRE(saved.digest.size() == 64);
            REQUIRE(readText(scene_path) == expected_persisted);
            REQUIRE(config.sceneDataJson() == expected_semantic);
            REQUIRE(config.sceneDocument().revision() == saved.scene_revision);
            REQUIRE(temporarySceneFileCount(temp_dir) == 0);

            // Same-process explicit reload uses the replaced file and rebuilds
            // the whole cache, including non-current scenes and raw components.
            config.invalidateSceneDocument();
            const auto &same_process = config.sceneDocument();
            REQUIRE(test_support::authoring(same_process).encodeSemantic() ==
                    expected_semantic);
            REQUIRE(test_support::authoring(same_process).rawJson() == source);
            REQUIRE(test_support::authoring(same_process).query().size() == 2);

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
            const auto old_resolver_generation =
                config.resolvedScene().resolverGeneration();
            const auto old_semantic =
                test_support::authoring(old_document).encodeSemantic();
            std::uint64_t old_max_object_id = 0;
            for (const auto &scene :
                 test_support::authoring(old_document).query()) {
                for (const auto &object : scene.objects) {
                    old_max_object_id = std::max(
                        old_max_object_id, object.authoring_object_id.value);
                }
            }

            auto snapshot = disk_document;
            snapshot["editor_envelope"]["snapshot_marker"] = "wp168";
            auto &snapshot_light = snapshot["scenes"]["secondary"]["objects"]
                                           [0]["components"][0];
            snapshot_light["position"] = {7, 8, 9};
            snapshot_light["intensity"] = 9.5f;
            const auto snapshot_bytes = snapshot.dump();
            const auto require_old_state = [&] {
                REQUIRE(&config.sceneDocument() == old_cache);
                REQUIRE(config.sceneDocument().revision() == old_revision);
                REQUIRE(test_support::authoring(config.sceneDocument())
                            .encodeSemantic() == old_semantic);
                REQUIRE(config.resolvedScene().revision() == old_revision);
                REQUIRE(config.resolvedScene().resolverGeneration() ==
                        old_resolver_generation);
                const auto *secondary =
                    config.resolvedScene().findScene("secondary");
                REQUIRE(secondary != nullptr);
                REQUIRE_FALSE(secondary->objects.front()
                                  .components.front()
                                  .effective_json.contains("intensity"));
                REQUIRE(readText(scene_path) == disk_bytes);
            };

            std::size_t reload_calls = 0;
            for (const auto point :
                 {SceneImportFaultPoint::AfterCandidatePrepare,
                  SceneImportFaultPoint::AfterPublication}) {
                DYNAMIC_SECTION("fault point " << static_cast<unsigned>(point)) {
                    config.setSceneImportFaultForTesting(point);
                    REQUIRE_THROWS(config.importSceneDocument(
                        snapshot_bytes,
                        [&](SceneLoadRequest &&) -> SceneLoadResult {
                            ++reload_calls;
                            return SceneLoadCommitted{};
                        }));
                    require_old_state();
                    REQUIRE(reload_calls == 0);
                    if (point == SceneImportFaultPoint::AfterPublication) {
                        std::cout << "WP360_SNAPSHOT_AFTER_PUBLICATION_ROLLBACK"
                                     " revision=" << old_revision.value
                                  << " generation="
                                  << old_resolver_generation.value
                                  << " candidate_light_intensity=9.5"
                                     " restored_light_intensity=omitted\n";
                    }
                }
            }
            config.setSceneImportFaultForTesting(std::nullopt);

            std::optional<std::string> loader_error;
            try {
                (void)config.importSceneDocument(
                    snapshot_bytes,
                    [&](SceneLoadRequest &&request) -> SceneLoadResult {
                    ++reload_calls;
                    REQUIRE(test_support::authoring(request.projection().authoring())
                                .rawJson() == snapshot);
                    const auto *secondary =
                        request.projection().resolved().findScene("secondary");
                    REQUIRE(secondary != nullptr);
                    REQUIRE(secondary->objects.front()
                                .components.front()
                                .effective_json.at("intensity") == 9.5f);
                    throw std::runtime_error("injected loader fault");
                });
            } catch (const std::runtime_error &error) {
                loader_error = error.what();
            }
            REQUIRE(loader_error == "injected loader fault");
            REQUIRE(reload_calls == 1);
            require_old_state();

            const auto imported = config.importSceneDocument(
                snapshot_bytes, [&](SceneLoadRequest &&request) {
                ++reload_calls;
                REQUIRE(test_support::authoring(request.projection().authoring())
                            .rawJson() == snapshot);
                return SceneLoadResult{SceneLoadCommitted{}};
            });
            REQUIRE(reload_calls == 2);
            REQUIRE(imported.revision.value == old_revision.value + 1U);
            REQUIRE(config.sceneDocument().revision() == imported.revision);
            REQUIRE(test_support::authoring(config.sceneDocument()).rawJson() ==
                    snapshot);
            REQUIRE(readText(scene_path) == disk_bytes);
            for (const auto &scene :
                 test_support::authoring(config.sceneDocument()).query()) {
                for (const auto &object : scene.objects) {
                    REQUIRE(object.authoring_object_id.value >
                            old_max_object_id);
                }
            }

            const auto effect_import = config.importSceneDocument(
                snapshot_bytes,
                [&](SceneLoadRequest &&) -> SceneLoadResult {
                    return CommittedWithEffectFailure{
                        .effect_errors = {{
                            .api = "saveData",
                            .error_code = "persistence_write_failed@1",
                            .error_fields = {{"path", "slot_c.json"}},
                            .phase = "flush",
                        }},
                    };
                });
            REQUIRE(effect_import.revision.value ==
                    old_revision.value + 2U);
            const auto *effect_failure =
                std::get_if<CommittedWithEffectFailure>(
                    &effect_import.load_result);
            REQUIRE(effect_failure != nullptr);
            REQUIRE(effect_failure->effect_errors.size() == 1);
            REQUIRE(effect_failure->effect_errors.front().api ==
                    "saveData");
            REQUIRE(config.sceneDocument().revision() ==
                    effect_import.revision);
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
    REQUIRE(test_support::authoring(fresh_process).encodeSemantic() ==
            expected_bytes);
    REQUIRE(test_support::authoring(fresh_process).rawJson() ==
            nlohmann::json::parse(expected_bytes));
    REQUIRE(test_support::authoring(fresh_process).query().size() == 2);
}

TEST_CASE("Scene format rejects legacy lights and names the v1 replacement", "[scene-format]") {
    const auto legacy_lights = nlohmann::json::parse(R"json({
  "schema": "pelican.scene",
  "version": 2,
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

TEST_CASE("SceneLoader reports the common typed cause for unknown component names",
          "[scene-format][WP379]") {
    ensureLogger();
    auto temp_dir = makeTempProjectDir();

    try {
        writeText(temp_dir / "scene.json", R"json({
  "schema": "pelican.scene",
  "version": 2,
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

        try {
            test_support::loadScene(GET_MODULE(SceneLoader), "default_scene");
            FAIL("unknown component unexpectedly loaded");
        } catch (const ComponentUnknownError &error) {
            CHECK(std::string{error.code()} == "component_unknown@1");
            CHECK(error.payload() == nlohmann::ordered_json{
                                         {"name", "missingcomponent"}});
            CHECK(std::string{error.what()} ==
                  "component_unknown@1{\"name\":\"missingcomponent\"}");
        }
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }

    std::filesystem::remove_all(temp_dir);
}

namespace {

// The engine logs through quill, so the only honest way to assert "one line"
// is to read the sink.  This one collects the formatted messages of whatever
// logger `Pelican::logger` points at while it is alive.
class CapturingLogSink final : public quill::Sink {
  public:
    void write_log(quill::MacroMetadata const *, std::uint64_t, std::string_view,
                   std::string_view, std::string const &, std::string_view,
                   quill::LogLevel, std::string_view, std::string_view,
                   std::vector<std::pair<std::string, std::string>> const *,
                   std::string_view log_message, std::string_view) override {
        const std::lock_guard<std::mutex> guard{mutex_};
        lines_.emplace_back(log_message);
    }
    void flush_sink() override {}

    std::vector<std::string> lines() const {
        const std::lock_guard<std::mutex> guard{mutex_};
        return lines_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

class CapturedLog {
  public:
    explicit CapturedLog(const std::string &name) {
        sink_ = std::static_pointer_cast<CapturingLogSink>(
            quill::Frontend::create_or_get_sink<CapturingLogSink>(name));
        capture_ = quill::Frontend::create_or_get_logger(name, sink_);
        previous_ = logger;
        logger = capture_;
    }
    ~CapturedLog() { logger = previous_; }
    CapturedLog(const CapturedLog &) = delete;
    CapturedLog &operator=(const CapturedLog &) = delete;

    std::vector<std::string> lines() const {
        capture_->flush_log();
        return sink_->lines();
    }

  private:
    std::shared_ptr<CapturingLogSink> sink_;
    quill::Logger *capture_ = nullptr;
    quill::Logger *previous_ = nullptr;
};

constexpr std::string_view unmatched_owner_message =
    "graph instance owner does not match any object in this scene";

std::size_t countUnmatchedOwnerLines(const CapturedLog &log) {
    std::size_t count = 0;
    for (const auto &line : log.lines())
        count += contains(line, unmatched_owner_message) ? 1 : 0;
    return count;
}

} // namespace

TEST_CASE("an unmatched graph instance owner is named once per graph generation",
          "[scene][graph-owner]") {
    ensureLogger();
    auto temp_dir = makeTempProjectDir();
    try {
        writeText(temp_dir / "scene.json", R"json({
  "schema": "pelican.scene", "version": 2,
  "scenes": {
    "default_scene": {"objects": [
      {"name": "Left", "components": [
        {"name": "transform", "pos": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}]}]},
    "other_scene": {"objects": [
      {"name": "Other", "components": [
        {"name": "transform", "pos": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}]}]}
  }
})json");
        writeText(temp_dir / "passes/main_rendering_config.json",
                  R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})");
        const auto project = nlohmann::json{
            {"schema", "pelican.project"}, {"version", 1},
            {"name", "graph-owner-test"}, {"engine_min_version", "0.1.0"},
            {"basic_config", {{"default_scene_id", "default_scene"},
                              {"scene_data_json", "scene.json"}}}};

        FastModuleContainer modules;
        GET_MODULE(PathResolver).setup(temp_dir, false);
        GET_MODULE(ProjectSource).setProjectData(project.dump());
        GET_MODULE(ECSPredefinedRegistration).reg();
        auto &loader = GET_MODULE(SceneLoader);
        test_support::loadScene(loader, SceneId{"default_scene"});
        const auto left = loader.objectId("Left");
        REQUIRE(left);
        std::uint64_t authored_left = 0;
        for (const auto &binding : loader.runtimeObjectBindings())
            if (binding.object_id == *left)
                authored_left = binding.authoring_object_id.value;
        REQUIRE(authored_left != 0);

        // Each block needs a scope name of its own: the memo that decides
        // "named already" is scene-scoped, so reusing one id would make the
        // second block silent for the first block's reason.
        const auto owned = [&](std::uint64_t left_object,
                               std::uint64_t right_object,
                               const std::string &right_id) {
            auto document = parameterGraphCompositionFixture();
            document["nodes"][4]["id"] = right_id;
            document["outputs"]["right"]["node"] = right_id;
            document["nodes"][3]["owner"] =
                {{"scene", "default_scene"}, {"object", left_object}};
            document["nodes"][4]["owner"] =
                {{"scene", "default_scene"}, {"object", right_object}};
            return std::make_shared<const ValueGraph::Graph>(document.dump());
        };

        // Control: every scope resolves, so the synchronization says nothing.
        {
            const CapturedLog log{"graph_owner_matched"};
            const auto matched =
                owned(authored_left, authored_left, "right_matched");
            for (int frame = 0; frame < 5; ++frame)
                (void)loader.graphParameterValues(matched);
            CHECK(countUnmatchedOwnerLines(log) == 0);
        }

        // One scope points at an object this scene does not have.  It is named
        // once, and stepping frames adds nothing.
        {
            const CapturedLog log{"graph_owner_unmatched"};
            const auto unmatched =
                owned(authored_left, 4242, "right_unmatched");
            (void)loader.graphParameterValues(unmatched);
            const auto after_first = log.lines();
            const auto named = std::count_if(
                after_first.begin(), after_first.end(), [](const auto &line) {
                    return contains(line, unmatched_owner_message);
                });
            REQUIRE(named == 1);
            for (const auto &line : after_first)
                if (contains(line, unmatched_owner_message))
                    CHECK(contains(line, "right_unmatched"));
            for (int frame = 0; frame < 30; ++frame)
                (void)loader.graphParameterValues(unmatched);
            CHECK(countUnmatchedOwnerLines(log) == 1);

            // Republishing the document makes a new graph generation.  The
            // scope is the same one already named, so the new generation adds
            // no second line.
            const auto republished =
                owned(authored_left, 4242, "right_unmatched");
            for (int frame = 0; frame < 30; ++frame)
                (void)loader.graphParameterValues(republished);
            CHECK(countUnmatchedOwnerLines(log) == 1);
        }

        // A scope that resolved once and then stopped is the ordinary state of
        // an object being destroyed and republished.  It stays silent, across
        // graph generations as well as frames.
        {
            const CapturedLog log{"graph_owner_went_quiet"};
            const auto resolving =
                owned(authored_left, authored_left, "right_quiet");
            (void)loader.graphParameterValues(resolving);
            REQUIRE(countUnmatchedOwnerLines(log) == 0);
            REQUIRE(GameObjects::remove(*left));
            for (int frame = 0; frame < 30; ++frame)
                (void)loader.graphParameterValues(resolving);
            CHECK(countUnmatchedOwnerLines(log) == 0);
            const auto republished =
                owned(authored_left, authored_left, "right_quiet");
            for (int frame = 0; frame < 30; ++frame)
                (void)loader.graphParameterValues(republished);
            CHECK(countUnmatchedOwnerLines(log) == 0);

            // A scene load clears the memo, because what the previous scene
            // could bind says nothing about this one.  The same graph is named
            // once for the new scene and then stays quiet for every frame.
            test_support::loadScene(loader, SceneId{"other_scene"});
            for (int frame = 0; frame < 30; ++frame)
                (void)loader.graphParameterValues(resolving);
            CHECK(countUnmatchedOwnerLines(log) == 1);
        }
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }
    std::filesystem::remove_all(temp_dir);
}

} // namespace Pelican

namespace Pelican {


TEST_CASE("sceneformat SV1 preserves scene bytes while editing one component", "[scene-format][sv1]") {
    using Json = nlohmann::ordered_json;
    Json source = {{"version", 2}, {"schema", "pelican.scene"},
        {"scenes", {{"main", {{"objects", Json::array({
            {{"name", "first"}, {"authoring_object_id", 1}, {"components", Json::array({
                {{"name", "transform"}, {"pos", {0.1, 0.2, 0.3}}},
                {{"name", "opaque"}, {"payload", 0.001}}})}},
            {{"name", "second"}, {"authoring_object_id", 2}, {"components", Json::array({
                {{"name", "transform"}, {"pos", {0.1, 0.2, 0.3}}}})}}})}}}}},
        {"next_authoring_object_id", 3}};
    auto pretty = source.dump(2);
    const auto opaque_begin = pretty.find("{\n", pretty.find("0.3"));
    const auto opaque_end = pretty.find('}', opaque_begin) + 1;
    const std::string untouched = R"({ "name" : "opaque", "payload": 1e-03 })";
    pretty.replace(opaque_begin, opaque_end - opaque_begin, untouched);
    std::string bytes = "\xef\xbb\xbf";
    for (char ch : pretty) { if (ch == '\n') bytes += '\r'; bytes += ch; }
    const bool trailing = GENERATE(false, true);
    if (trailing) bytes += "\r\n";
    const auto document = AuthoringSceneDocument::load(bytes, SceneRevision{1});
    REQUIRE(AuthoringSceneAuthority::encodePersistent(document) == bytes);
    auto raw = test_support::authoring(document).rawJson();
    auto component = raw["scenes"]["main"]["objects"][0]["components"][0];
    SECTION("T1 position has an exact byte diff") {
        component["pos"][0] = 0.4;
        auto command = makeSetComponentValueCommand("main", "first", "transform", component);
        command.apply(raw);
        const auto edited = test_support::authoring(document).stage(raw, SceneRevision{2});
        auto expected = bytes;
        expected.replace(expected.find("0.1"), 3, "0.4");
        CHECK(AuthoringSceneAuthority::encodePersistent(edited) == expected);
    }
    SECTION("T2 rotation retains position and omits absent defaults") {
        component["rotation"] = {0.0, 0.0, 0.0, 1.0};
        makeSetComponentValueCommand("main", "first", "transform", component).apply(raw);
        CHECK_FALSE(raw["scenes"]["main"]["objects"][0]["components"][0].contains("scale"));
        const auto saved = AuthoringSceneAuthority::encodePersistent(
            test_support::authoring(document).stage(raw, SceneRevision{2}));
        CHECK(saved.find("0.100000001") == std::string::npos);
        CHECK(saved.find(untouched) != std::string::npos);
        CHECK(saved.find("\"rotation\"") > saved.find("0.3"));
        CHECK(saved.starts_with("\xef\xbb\xbf"));
        CHECK(saved.ends_with("\r\n") == trailing);
        CHECK(saved.find('\n') == saved.find("\r\n") + 1);
    }
    SECTION("T4 new components append and existing components retain their bytes") {
        raw["scenes"]["main"]["objects"][0]["components"].push_back({{"name", "new_component"}});
        const auto saved = AuthoringSceneAuthority::encodePersistent(
            test_support::authoring(document).stage(raw, SceneRevision{2}));
        CHECK(saved.find(untouched) != std::string::npos);
        CHECK(saved.find("new_component") > saved.find(untouched));
        const auto parsed = Json::parse(saved);
        CHECK(parsed.begin().key() == "version");
        CHECK(parsed["scenes"]["main"]["objects"][0]["components"][2]["name"] == "new_component");
    }

    SECTION("T4 successive unsaved edits append new keys in edit order") {
        raw["z_added"] = 1;
        const auto first = test_support::authoring(document).stage(raw, SceneRevision{2});
        raw["a_added"] = 2;
        const auto saved = AuthoringSceneAuthority::encodePersistent(
            test_support::authoring(first).stage(raw, SceneRevision{3}));
        CHECK(saved.find("z_added") < saved.find("a_added"));
        CHECK(saved.find(untouched) != std::string::npos);
    }
    SECTION("structural reorder carries untouched component bytes with its identity") {
        auto stage = AuthoringSceneAuthority::structuralStage(document);
        stage.reorderObject(AuthoringObjectId{1}, 1);
        const auto saved = AuthoringSceneAuthority::encodePersistent(std::move(stage).finish(SceneRevision{2}));
        CHECK(saved.find("second") < saved.find("first"));
        CHECK(saved.find(untouched) != std::string::npos);
    }
}

}
