#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/loader/basicconfig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios_base::binary};
    if (!input.is_open()) throw std::runtime_error("failed to open fixture: " + path.string());
    return nlohmann::json::parse(input);
}

std::filesystem::path fixturePath(std::string_view name) {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
           "editor_command_service" / name;
}

std::filesystem::path authoringFixturePath() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
           "authoring_scene" / "multi_scene_roundtrip.json";
}

EditorRuntimeObjectState fakeRuntime(const AuthoringSceneView &scene,
                                     const AuthoringObjectView &object) {
    EditorRuntimeObjectState result;
    result.component_runtime_json.resize(object.components.size());
    if (scene.scene_id != "main" || !object.name || *object.name != "MixedObject") return result;
    result.entity_id = GameObjectId{17, 4};
    const auto transform = std::find_if(object.components.begin(), object.components.end(), [](const auto &component) {
        return component.authoredJson().at("name") == "transform";
    });
    if (transform != object.components.end()) {
        const auto index = static_cast<std::size_t>(transform - object.components.begin());
        result.component_runtime_json[index] = nlohmann::ordered_json{
            {"local_trs", transform->authoredJson()},
            {"world_trs", {{"name", "transform"},
                           {"pos", {11, 2, 3}},
                           {"rotation", {0, 0, 0, 1}},
                           {"scale", {1, 1, 1}}}},
        };
    }
    return result;
}

EditorCommandService makeQueryService(const AuthoringSceneDocument &document) {
    return EditorCommandService{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .current_scene_id = [] { return std::string{"main"}; },
        .runtime_query = fakeRuntime,
        .assets = [] {
            return std::vector<EditorAssetQueryResult>{
                {.id = "crate", .kind = "model", .path = "assets/crate.glb", .store = "assets", .status = "loaded"},
                {.id = "missing", .kind = "texture", .path = "assets/missing.png", .store = "assets", .status = "missing"},
            };
        },
        .snapshot_state = [] { return EditorSnapshotState{.preview_epoch = 3}; },
    }};
}

template <class Invoke> EditorCommandErrorCode errorCode(Invoke &&invoke) {
    std::optional<EditorCommandErrorCode> result;
    try {
        std::forward<Invoke>(invoke)();
    } catch (const EditorCommandError &error) {
        result = error.code();
    }
    REQUIRE(result.has_value());
    return *result;
}

} // namespace

TEST_CASE("editor query is deterministic and RPC and ImGui fake adapters are equivalent",
          "[editor-command][query][wp154]") {
    const auto document =
        AuthoringSceneDocument::load(readJson(authoringFixturePath()).dump(), SceneRevision{41});
    const auto service = makeQueryService(document);
    const EditorCommandRpcAdapter rpc{service};
    const EditorCommandImGuiFakeAdapter imgui{service};

    const auto first_rpc = rpc.sceneTree(nlohmann::json::object());
    const auto second_rpc = rpc.sceneTree(nlohmann::json::object());
    const auto imgui_json = editorQueryJson(imgui.sceneTree());
    REQUIRE(first_rpc.dump() == second_rpc.dump());
    REQUIRE(first_rpc.dump() == imgui_json.dump());

    REQUIRE(first_rpc.at("scene_revision") == 41);
    REQUIRE(first_rpc.at("scene_id") == "main");
    REQUIRE(first_rpc.at("objects").size() == 2);
    REQUIRE(first_rpc.at("objects").at(0).at("name") == "ColliderOnly");
    const auto &mixed = first_rpc.at("objects").at(1);
    REQUIRE(mixed.at("name") == "MixedObject");
    REQUIRE(mixed.at("entity_id") == nlohmann::ordered_json{{"index", 17}, {"gen", 4}});
    REQUIRE(mixed.at("components").at(0).at("name") == "transform");
    REQUIRE(mixed.at("components").at(1).at("name") == "unknown_read_only");

    const auto &transform = mixed.at("components").at(0);
    REQUIRE(transform.at("editable") == true);
    REQUIRE(transform.at("codec").at("state") == "registered");
    REQUIRE(transform.at("schema").at("state") == "available");
    REQUIRE(transform.at("runtime_json").contains("local_trs"));
    REQUIRE(transform.at("runtime_json").contains("world_trs"));
    REQUIRE(transform.at("runtime_json").at("world_trs").at("pos").at(0) == 11);
    REQUIRE(transform.at("authored_json").at("pos").at(0) == 1);
    REQUIRE(transform.at("pending") == false);

    const auto &unknown = mixed.at("components").at(1);
    REQUIRE(unknown.at("editable") == false);
    REQUIRE(unknown.at("codec").at("state") == "missing");
    REQUIRE(unknown.at("schema").at("state") == "missing");
    REQUIRE_FALSE(unknown.contains("runtime_json"));

    const auto by_name = rpc.getComponents({{"name", "MixedObject"}});
    const auto by_id = rpc.getComponents({{"authoring_object_id", mixed.at("authoring_object_id")}});
    REQUIRE(by_name.dump() == by_id.dump());
    REQUIRE(by_name.dump() == editorQueryJson(imgui.getComponents(
                                  EditorGetComponentsRequest{.name = "MixedObject"}))
                                  .dump());

    const auto rpc_error = errorCode([&] { (void)rpc.sceneTree({{"scene_id", "absent"}}); });
    const auto imgui_error = errorCode([&] {
        (void)imgui.sceneTree(EditorSceneTreeRequest{.scene_id = "absent"});
    });
    REQUIRE(rpc_error == EditorCommandErrorCode::SceneNotFound);
    REQUIRE(rpc_error == imgui_error);

    const auto assets = rpc.listAssets({{"store", "assets"}});
    REQUIRE(assets.dump() == editorQueryJson(imgui.listAssets({.store = "assets"})).dump());
    REQUIRE(assets.at("assets").size() == 2);
}

TEST_CASE("ExportSceneSnapshot V1 fixture uses semantic bytes and their real sha256",
          "[editor-command][snapshot][wp154]") {
    const auto fixture = readJson(fixturePath("export_scene_snapshot_v1.json"));
    const auto document = AuthoringSceneDocument::load(fixture.at("document").dump(), SceneRevision{42});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .current_scene_id = [] { return std::string{"main"}; },
        .snapshot_state = [] { return EditorSnapshotState{.preview_epoch = 7}; },
    }};
    const EditorCommandRpcAdapter rpc{service};
    const EditorCommandImGuiFakeAdapter imgui{service};

    const auto rpc_response = rpc.exportSceneSnapshot(fixture.at("request"));
    const auto typed_response = imgui.exportSceneSnapshot({.schema_version = 1, .allow_pending = false});
    REQUIRE(nlohmann::json(rpc_response) == fixture.at("expected_response"));
    REQUIRE(rpc_response.dump() == editorQueryJson(typed_response).dump());
    REQUIRE(typed_response.semantic_scene_bytes == document.encodeSemantic());
    REQUIRE(typed_response.digest.algorithm == "sha256");
    REQUIRE(typed_response.digest.hex.size() == 64);
    REQUIRE(std::all_of(typed_response.digest.hex.begin(), typed_response.digest.hex.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
    }));
    REQUIRE(maxSceneSnapshotBytes == 64U * 1024U * 1024U);
}

TEST_CASE("ExportSceneSnapshot V1 validates strict request fields and pending state",
          "[editor-command][snapshot][negative][wp154]") {
    const auto fixture = readJson(fixturePath("export_scene_snapshot_v1.json"));
    const auto document = AuthoringSceneDocument::load(fixture.at("document").dump(), SceneRevision{8});
    const EditorCommandService busy_service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .current_scene_id = [] { return std::string{"main"}; },
        .snapshot_state = [] {
            return EditorSnapshotState{.pending_ticket_ids = {"ticket-7"},
                                       .open_preview_lease = true,
                                       .preview_epoch = 9};
        },
    }};
    const EditorCommandRpcAdapter rpc{busy_service};

    REQUIRE(errorCode([&] { (void)rpc.exportSceneSnapshot({{"schema_version", 1}}); }) ==
            EditorCommandErrorCode::SnapshotBusy);
    const auto allowed = rpc.exportSceneSnapshot({{"schema_version", 1}, {"allow_pending", true}});
    REQUIRE(allowed.at("pending_ticket_ids") == nlohmann::ordered_json::array({"ticket-7"}));
    REQUIRE(allowed.at("preview_epoch") == 9);

    REQUIRE(errorCode([&] { (void)rpc.exportSceneSnapshot({{"schema_version", 2}}); }) ==
            EditorCommandErrorCode::UnsupportedSnapshotVersion);
    REQUIRE(errorCode([&] { (void)rpc.exportSceneSnapshot({{"schema_version", 1.0}}); }) ==
            EditorCommandErrorCode::InvalidParams);
    REQUIRE(errorCode([&] {
                (void)rpc.exportSceneSnapshot({{"schema_version", 1}, {"allow_pending", nullptr}});
            }) == EditorCommandErrorCode::InvalidParams);
    REQUIRE(errorCode([&] {
                (void)rpc.exportSceneSnapshot({{"schema_version", 1}, {"unknown", false}});
            }) == EditorCommandErrorCode::InvalidParams);
}

TEST_CASE("ExportSceneSnapshot V1 rejects semantic payloads over 64 MiB",
          "[editor-command][snapshot][negative][wp154]") {
    auto oversized_document = readJson(fixturePath("export_scene_snapshot_v1.json")).at("document");
    oversized_document["padding"] = std::string(maxSceneSnapshotBytes, 'x');
    const auto document = AuthoringSceneDocument::load(oversized_document.dump(), SceneRevision{9});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .current_scene_id = [] { return std::string{"main"}; },
        .snapshot_state = [] { return EditorSnapshotState{}; },
    }};

    const ExportSceneSnapshotRequestV1 request{.schema_version = 1};
    const auto code = errorCode([&] { (void)service.exportSceneSnapshot(request); });
    REQUIRE(code == EditorCommandErrorCode::SnapshotTooLarge);
}

TEST_CASE("SAVE0 typed RPC and ImGui surfaces publish the same result",
          "[editor-command][save][wp166]") {
    const auto document = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{12});
    std::size_t save_calls = 0;
    EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .snapshot_state = [] { return EditorSnapshotState{}; },
        .save_scene = [&save_calls] {
            ++save_calls;
            return SaveSceneResult{
                .scene_revision = SceneRevision{13},
                .digest = {.algorithm = "sha256", .hex = std::string(64, 'a')},
                .byte_count = 1234,
                .scene_hot_reload = false,
            };
        },
    }};
    const EditorCommandRpcAdapter rpc{service};
    const EditorCommandImGuiFakeAdapter imgui{service};

    const auto rpc_result = rpc.saveScene(nlohmann::json::object());
    const auto typed_result = imgui.saveScene();
    REQUIRE(rpc_result.dump() == editorQueryJson(typed_result).dump());
    REQUIRE(rpc_result.at("status") == "saved");
    REQUIRE(rpc_result.at("scene_revision") == 13);
    REQUIRE(rpc_result.at("scene_hot_reload") == false);
    REQUIRE(save_calls == 2);

    REQUIRE(errorCode([&] { (void)rpc.saveScene({{"unknown", true}}); }) ==
            EditorCommandErrorCode::InvalidParams);
}

TEST_CASE("SAVE0 rejects pending work and exposes stable hard error codes",
          "[editor-command][save][negative][wp166]") {
    const auto document = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{20});
    std::size_t save_calls = 0;
    EditorCommandService busy{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .snapshot_state = [] {
            return EditorSnapshotState{.pending_ticket_ids = {"ticket-1"},
                                       .open_preview_lease = true};
        },
        .save_scene = [&save_calls] {
            ++save_calls;
            return SaveSceneResult{};
        },
    }};
    REQUIRE(errorCode([&] { (void)busy.saveScene(); }) ==
            EditorCommandErrorCode::SaveBusy);
    REQUIRE(save_calls == 0);

    EditorCommandService external{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .save_scene = []() -> SaveSceneResult {
            throw SceneSaveError{SceneSaveErrorCode::ExternalModification,
                                 "external change"};
        },
    }};
    REQUIRE(errorCode([&] { (void)external.saveScene(); }) ==
            EditorCommandErrorCode::ExternalModification);

    EditorCommandService runtime_only{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .save_scene = []() -> SaveSceneResult {
            throw EditorCommandError{EditorCommandErrorCode::RuntimeOnlyData,
                                     "runtime-only data"};
        },
    }};
    REQUIRE(errorCode([&] { (void)runtime_only.saveScene(); }) ==
            EditorCommandErrorCode::RuntimeOnlyData);
}

} // namespace Pelican
