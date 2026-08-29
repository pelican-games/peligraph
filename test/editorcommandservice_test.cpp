#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/project/sceneformat.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <picosha2.h>
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

std::filesystem::path exampleScenePath() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example" /
           "scenes" / "main.scene.json";
}

EditorRuntimeObjectState fakeRuntime(const ResolvedSceneView &scene,
                                     const ResolvedObject &object) {
    EditorRuntimeObjectState result;
    result.component_runtime_json.resize(object.components.size());
    if (scene.scene_id != "main" || !object.name || *object.name != "MixedObject") return result;
    result.entity_id = GameObjectId{17, 4};
    const auto transform = std::find_if(object.components.begin(), object.components.end(), [](const auto &component) {
        return component.name == "transform";
    });
    if (transform != object.components.end()) {
        const auto index = static_cast<std::size_t>(transform - object.components.begin());
        result.component_runtime_json[index] = nlohmann::ordered_json{
            {"local_trs", transform->source_json_exact},
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

DigestV1 digestFor(std::string_view bytes) {
    return DigestV1{
        .algorithm = "sha256",
        .hex = picosha2::hash256_hex_string(bytes.begin(), bytes.end()),
    };
}

ImportSceneSnapshotRequestV1 importRequest(
    const ExportSceneSnapshotResponseV1 &snapshot) {
    return ImportSceneSnapshotRequestV1{
        .schema_version = snapshot.schema_version,
        .semantic_scene_bytes = snapshot.semantic_scene_bytes,
        .digest = snapshot.digest,
        .current_scene_id = snapshot.current_scene_id,
    };
}

nlohmann::json importRequestJson(
    const ImportSceneSnapshotRequestV1 &request) {
    return {
        {"schema_version", request.schema_version},
        {"semantic_scene_bytes", request.semantic_scene_bytes},
        {"digest", {{"algorithm", request.digest.algorithm},
                    {"hex", request.digest.hex}}},
        {"current_scene_id", request.current_scene_id},
    };
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
    REQUIRE(first_rpc.at("objects").at(0).at("declaration_index") == 0);
    const auto &mixed = first_rpc.at("objects").at(1);
    REQUIRE(mixed.at("name") == "MixedObject");
    REQUIRE(mixed.at("declaration_index") == 1);
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
    REQUIRE(by_id.at("declaration_index") == 1);
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

TEST_CASE("RPC declaration indices match the direct project view for unnamed example objects",
          "[editor-command][query][wp258]") {
    const auto source = readJson(exampleScenePath());
    const auto direct = normalizeSceneDataJson(source);
    const auto &direct_objects =
        direct.scenes.at("default_scene").at("objects");
    const auto document =
        AuthoringSceneDocument::load(source.dump(), SceneRevision{258});
    const auto service = makeQueryService(document);
    const EditorCommandRpcAdapter rpc{service};

    const auto tree = rpc.sceneTree({{"scene_id", "default_scene"}});
    REQUIRE(direct_objects.size() == 47);
    REQUIRE(tree.at("objects").size() == direct_objects.size());

    std::size_t unnamed_count = 0;
    for (std::size_t index = 0; index < direct_objects.size(); ++index) {
        const auto &direct_object = direct_objects.at(index);
        const auto &rpc_object = tree.at("objects").at(index);
        REQUIRE(rpc_object.at("declaration_index") == index);
        if (!direct_object.contains("name")) {
            ++unnamed_count;
            REQUIRE_FALSE(rpc_object.contains("name"));
        }
    }
    REQUIRE(unnamed_count == 32);
}

TEST_CASE("RPC declaration indices follow order after authoring id diverges on insertion",
          "[editor-command][query][wp258]") {
    auto document = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{40});
    auto stage = test_support::authoring(document).structuralStage();
    const auto inserted_id = stage.insertObject(
        "main", 1,
        nlohmann::json{{"components", nlohmann::json::array()}});
    document = std::move(stage).finish(SceneRevision{41});

    const auto service = makeQueryService(document);
    const EditorCommandRpcAdapter rpc{service};
    const auto tree = rpc.sceneTree(nlohmann::json::object());
    const auto &objects = tree.at("objects");

    REQUIRE(objects.size() == 3);
    for (std::size_t index = 0; index < objects.size(); ++index) {
        REQUIRE(objects.at(index).at("declaration_index") == index);
    }
    REQUIRE(objects.at(1).at("authoring_object_id") == inserted_id.value);
    REQUIRE(objects.at(1).at("authoring_object_id") !=
            objects.at(1).at("declaration_index"));

    const auto components = rpc.getComponents(
        {{"authoring_object_id", inserted_id.value}});
    REQUIRE(components.at("authoring_object_id") == inserted_id.value);
    REQUIRE(components.at("declaration_index") == 1);
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
    REQUIRE(typed_response.semantic_scene_bytes ==
            test_support::authoring(document).encodeSemantic());
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

TEST_CASE("ImportSceneSnapshot V1 round trips all semantic bytes with fresh identities",
          "[editor-command][snapshot][import][roundtrip][wp168]") {
    const auto source = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{42});
    const EditorCommandService source_service{EditorCommandServiceDependencies{
        .document = [&source]() -> const AuthoringSceneDocument & { return source; },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    const auto exported = source_service.exportSceneSnapshot({});

    auto target = AuthoringSceneDocument::load(
        R"({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[]}}})",
        SceneRevision{7});
    std::string target_scene = "main";
    std::size_t import_calls = 0;
    EditorCommandService target_service{EditorCommandServiceDependencies{
        .document = [&target]() -> const AuthoringSceneDocument & { return target; },
        .current_scene_id = [&target_scene] { return target_scene; },
        .import_scene_snapshot =
            [&](std::string_view bytes, std::string_view current_scene_id) {
                ++import_calls;
                target = AuthoringSceneDocument::load(
                    bytes, SceneRevision{target.revision().value + 1U}, 1000);
                target_scene = std::string{current_scene_id};
                return target.revision();
            },
    }};
    EditorCommandRpcAdapter target_rpc{target_service};

    const auto response =
        target_rpc.importSceneSnapshot(importRequestJson(importRequest(exported)));
    REQUIRE(response.at("status") == "imported");
    REQUIRE(response.at("scene_revision") == 8);
    REQUIRE(response.at("current_scene_id") == "main");
    REQUIRE(import_calls == 1);

    const auto round_tripped = target_service.exportSceneSnapshot({});
    REQUIRE(round_tripped.semantic_scene_bytes == exported.semantic_scene_bytes);
    REQUIRE(round_tripped.digest.algorithm == exported.digest.algorithm);
    REQUIRE(round_tripped.digest.hex == exported.digest.hex);
    REQUIRE(test_support::authoring(target).rawJson() ==
            test_support::authoring(source).rawJson());
    REQUIRE(test_support::authoring(target)
                .rawJson()
                .at("scenes")
                .at("secondary") ==
            test_support::authoring(source)
                .rawJson()
                .at("scenes")
                .at("secondary"));
    REQUIRE(test_support::authoring(target)
                .rawJson()
                .at("scenes")
                .at("main")
                .at("objects")
                .at(1)
                .at("components")
                .at(1)
                .at("name") == "unknown_read_only");

    const auto source_objects = test_support::authoring(source).query().at(0).objects;
    const auto target_objects = test_support::authoring(target).query().at(0).objects;
    REQUIRE(source_objects.size() == target_objects.size());
    REQUIRE(source_objects.front().authoring_object_id.value < 1000);
    REQUIRE(target_objects.front().authoring_object_id.value == 1000);
}

TEST_CASE("ImportSceneSnapshot V1 enforces five ordered gates without publication",
          "[editor-command][snapshot][import][negative][fault][wp168]") {
    auto target = AuthoringSceneDocument::load(
        R"({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[]}}})",
        SceneRevision{11});
    const auto source = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{21});
    const EditorCommandService source_service{EditorCommandServiceDependencies{
        .document = [&source]() -> const AuthoringSceneDocument & { return source; },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    auto valid = importRequest(source_service.exportSceneSnapshot({}));

    bool inject_fault = false;
    std::size_t import_calls = 0;
    EditorCommandService target_service{EditorCommandServiceDependencies{
        .document = [&target]() -> const AuthoringSceneDocument & { return target; },
        .current_scene_id = [] { return std::string{"main"}; },
        .import_scene_snapshot =
            [&](std::string_view bytes, std::string_view) {
                ++import_calls;
                if (inject_fault) {
                    throw std::runtime_error("injected import publication fault");
                }
                target = AuthoringSceneDocument::load(
                    bytes, SceneRevision{target.revision().value + 1U}, 500);
                return target.revision();
            },
    }};
    EditorCommandRpcAdapter rpc{target_service};
    const auto baseline_bytes =
        test_support::authoring(target).encodeSemantic();
    const auto baseline_revision = target.revision();
    const auto require_unpublished = [&] {
        REQUIRE(test_support::authoring(target).encodeSemantic() ==
                baseline_bytes);
        REQUIRE(target.revision() == baseline_revision);
    };

    auto unsupported = valid;
    unsupported.schema_version = 2;
    unsupported.semantic_scene_bytes.assign(maxSceneSnapshotBytes + 1U, 'x');
    REQUIRE(errorCode([&] { (void)target_service.importSceneSnapshot(unsupported); }) ==
            EditorCommandErrorCode::UnsupportedSnapshotVersion);
    require_unpublished();

    auto oversized = valid;
    oversized.semantic_scene_bytes.assign(maxSceneSnapshotBytes + 1U, 'x');
    oversized.digest.hex = "wrong";
    REQUIRE(errorCode([&] { (void)target_service.importSceneSnapshot(oversized); }) ==
            EditorCommandErrorCode::SnapshotTooLarge);
    require_unpublished();

    auto mismatched = valid;
    mismatched.digest.hex = std::string(64, '0');
    REQUIRE(errorCode([&] { (void)target_service.importSceneSnapshot(mismatched); }) ==
            EditorCommandErrorCode::DigestMismatch);
    require_unpublished();

    auto invalid = valid;
    invalid.semantic_scene_bytes = "{not-json";
    invalid.digest = digestFor(invalid.semantic_scene_bytes);
    std::optional<std::string> invalid_detail;
    try {
        (void)target_service.importSceneSnapshot(invalid);
    } catch (const EditorCommandError &error) {
        REQUIRE(error.code() == EditorCommandErrorCode::SnapshotInvalid);
        invalid_detail = error.detail();
    }
    REQUIRE(invalid_detail.has_value());
    REQUIRE_FALSE(invalid_detail->empty());
    require_unpublished();

    auto missing_scene = valid;
    missing_scene.current_scene_id = "absent";
    REQUIRE(errorCode([&] { (void)target_service.importSceneSnapshot(missing_scene); }) ==
            EditorCommandErrorCode::SceneNotFound);
    require_unpublished();
    REQUIRE(import_calls == 0);

    inject_fault = true;
    std::optional<std::string> fault_detail;
    try {
        (void)target_service.importSceneSnapshot(valid);
    } catch (const EditorCommandError &error) {
        REQUIRE(error.code() == EditorCommandErrorCode::SnapshotInvalid);
        fault_detail = error.detail();
    }
    REQUIRE(fault_detail == "injected import publication fault");
    REQUIRE(import_calls == 1);
    require_unpublished();

    // Adapter ordering is also fixed: version and size gates do not inspect
    // later required fields.
    REQUIRE(errorCode([&] {
                (void)rpc.importSceneSnapshot({{"schema_version", 2}});
            }) == EditorCommandErrorCode::UnsupportedSnapshotVersion);
    REQUIRE(errorCode([&] {
                (void)rpc.importSceneSnapshot(
                    {{"schema_version", 1},
                     {"semantic_scene_bytes",
                      std::string(maxSceneSnapshotBytes + 1U, 'x')}});
            }) == EditorCommandErrorCode::SnapshotTooLarge);
    require_unpublished();
}

TEST_CASE("ImportSceneSnapshot design example is strict JSON and reaches semantic validation",
          "[editor-command][snapshot][import][schema][wp168]") {
    auto example = readJson(fixturePath("import_scene_snapshot_v1.json"));
    const auto &bytes = example.at("semantic_scene_bytes").get_ref<const std::string &>();
    example["digest"]["hex"] = digestFor(bytes).hex;

    const auto document = AuthoringSceneDocument::load(
        R"({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[]}}})",
        SceneRevision{1});
    std::size_t import_calls = 0;
    EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .import_scene_snapshot =
            [&](std::string_view, std::string_view) {
                ++import_calls;
                return SceneRevision{2};
            },
    }};
    EditorCommandRpcAdapter rpc{service};
    REQUIRE(errorCode([&] { (void)rpc.importSceneSnapshot(example); }) ==
            EditorCommandErrorCode::SnapshotInvalid);
    REQUIRE(import_calls == 0);
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
