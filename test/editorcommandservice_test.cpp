#include "../src/core/shader/raygrouplayout.hpp"
#include <cstring>
#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/communication/editorassetqueryruntime.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/prefabprovider.hpp"
#include "../src/core/registrationownerhost.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"
#include "../src/core/userpublic/details/behavior/registerer.hpp"
#include "../src/core/userpublic/details/ecs/componentdeclare.hpp"
#include "../src/project/sceneformat.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <picosha2.h>
#include <stdexcept>
#include <sstream>
#include <string>
#include <utility>

namespace wp382a_catalog_fixture {

enum class Flag : std::uint32_t { A = 1u, B = 2u };

struct Row {
    std::string key;
    std::optional<std::uint8_t> mask;
    std::optional<Pelican::Flags<Flag>> flags;
    bool row_enabled = false;

    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("wp382a_rpc_row"),
        Pelican::required(Pelican::field<&Row::key>("key")),
        Pelican::optionalOverride(Pelican::field<&Row::mask>(
            "mask", Pelican::urange(0, 255))),
        Pelican::optionalOverride(
            Pelican::field<&Row::flags>("flags"),
            Pelican::enumValues(Pelican::enumValue("A", Flag::A),
                                Pelican::enumValue("B", Flag::B))),
        Pelican::defaulted(
            Pelican::field<&Row::row_enabled>("row_enabled"), false));
};

struct Component {
    Pelican::List<Row> rows;
    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("wp382a_rpc_component"),
        Pelican::defaulted(Pelican::field<&Component::rows>("rows")));
};

} // namespace wp382a_catalog_fixture

DECLARE_COMPONENT(wp382a_catalog_fixture::Component, 62);

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

struct CatalogBehaviorParams {
    float speed = 0.0f;
    static constexpr auto schema = structFields(behaviorParamsPolicy,
        defaulted(field<&CatalogBehaviorParams::speed>("speed"), 2.0f));
};

class CatalogBehavior final : public Behavior {
  public:
    using Params = CatalogBehaviorParams;
    static inline int constructions = 0;
    CatalogBehavior() { ++constructions; }
};

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

std::function<const ResolvedScene &()>
explicitResolvedProvider(const AuthoringSceneDocument &document) {
    auto cached = std::make_shared<std::optional<ResolvedScene>>();
    return [&document, cached]() -> const ResolvedScene & {
        if (!*cached || (*cached)->revision() != document.revision()) {
            *cached = ResolvedSceneResolver::resolve(
                document, SceneResolverGeneration{document.revision().value});
        }
        return **cached;
    };
}

EditorCommandService makeQueryService(const AuthoringSceneDocument &document,
                                    std::vector<EditorAssetQueryResult> extra_assets = {}) {
    return EditorCommandService{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .runtime_query = fakeRuntime,
        .assets = [extra_assets = std::move(extra_assets)] {
            auto assets = std::vector<EditorAssetQueryResult>{
                {.id = "crate", .kind = "model", .path = "assets/crate.glb", .store = "assets", .status = "loaded"},
                {.id = "missing", .kind = "texture", .path = "assets/missing.png", .store = "assets", .status = "missing"},
            };
            assets.insert(assets.end(), extra_assets.begin(), extra_assets.end());
            return assets;
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

// MG-2b: SnapshotV1 is gone; these tests run the snapshot contract through
// SnapshotV2, the one form, which keeps SNAPSHOT0's gates.
ImportSceneSnapshotRequestV2 importRequest(
    const ExportSceneSnapshotResponseV2 &snapshot) {
    return ImportSceneSnapshotRequestV2{
        .schema_version = snapshot.schema_version,
        .semantic_scene_bytes = snapshot.semantic_scene_bytes,
        .digest = snapshot.digest,
        .current_scene_id = snapshot.current_scene_id,
        .prefab_closure = snapshot.prefab_closure,
    };
}

// The wire form of an import request built from the wire form of an export.
nlohmann::json importRequestJson(const nlohmann::ordered_json &exported) {
    return {
        {"schema_version", exported.at("schema_version")},
        {"semantic_scene_bytes", exported.at("semantic_scene_bytes")},
        {"digest", exported.at("digest")},
        {"current_scene_id", exported.at("current_scene_id")},
        {"prefab_closure", exported.at("prefab_closure")},
    };
}

} // namespace

TEST_CASE("component addition catalog uses codec defaults and excludes attached slots",
          "[editor-command][query][component-addition][pg1]") {
    const auto document = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{41});
    const auto service = makeQueryService(document);
    const EditorCommandRpcAdapter rpc{service};
    const auto response = rpc.getComponentAddCandidates({{"name", "MixedObject"}});
    CHECK(response.at("scene_revision") == 41);
    const auto &candidates = response.at("candidates");
    CHECK(std::none_of(candidates.begin(), candidates.end(), [](const auto &candidate) {
        return candidate.at("name") == "transform";
    }));
    const auto collider = std::find_if(candidates.begin(), candidates.end(), [](const auto &candidate) {
        return candidate.at("name") == "collider";
    });
    REQUIRE(collider != candidates.end());
    CHECK(collider->at("key") == "collider");
    const auto &codec = requireComponentCodec("collider");
    const auto decoded = codec.decodeAuthored(collider->at("component_template"));
    REQUIRE(decoded.hasValue());
    CHECK(codec.encodeCanonical(decoded.value()) == collider->at("component_template"));
#if PELICAN_WITH_PHYSICS
    REQUIRE(collider->at("available") == true);
    CHECK(collider->at("component_template") == collider->at("component"));
#else
    // PG-1 acceptance 2: listed, with the name add_component would be refused
    // under, and nothing to add.
    REQUIRE(collider->at("available") == false);
    CHECK(collider->at("unavailable_reason") == "physics.module_unavailable@1");
    CHECK_FALSE(collider->contains("component"));
    std::cout << "PG1_ADD_CANDIDATE name=collider available=false reason="
              << collider->at("unavailable_reason").get<std::string>() << '\n';
#endif
    const auto model = std::find_if(candidates.begin(), candidates.end(), [](const auto &candidate) {
        return candidate.at("name") == "simplemodelview";
    });
    REQUIRE(model != candidates.end());
    CHECK(model->at("asset_selection").at("field") == "model");
    REQUIRE(model->at("asset_selection").at("choices").size() == 1);
    CHECK(model->at("asset_selection").at("choices")[0].at("id") == "crate");
    CHECK(model->at("available") == false);
    CHECK_FALSE(model->at("unavailable_reason").get<std::string>().empty());
    CHECK_FALSE(model->contains("component"));
    CHECK(model->at("component_template") == nlohmann::ordered_json{{"name", "simplemodelview"}});
    // MT-2: the object's own material rows beside the model.
    REQUIRE(model->at("schema_fields").size() == 2);
    CHECK(model->at("schema_fields")[0].at("name") == "model");
    CHECK(model->at("schema_fields")[1].at("name") == "materials");
    auto authored_model = model->at("component_template");
    authored_model["model"] = "character";
    const auto model_decoded = requireComponentCodec("simplemodelview").decodeAuthored(authored_model);
    REQUIRE(model_decoded.hasValue());
    CHECK(requireComponentCodec("simplemodelview").encodeCanonical(model_decoded.value()) == authored_model);
}

// SC-1: the component schema the Inspector builds its rows from names the
// controller types and, beside each controller field, the types that read it,
// the types that refuse its absence, and what an absent field reads as.
TEST_CASE("SC-1 get_components describes the camera controller by type",
          "[editor-command][query][camera][controller][sc1]") {
    const nlohmann::json source{
        {"schema", "pelican.scene"},
        {"version", 2},
        {"scenes",
         {{"main",
           {{"objects",
             nlohmann::json::array(
                 {{{"name", "Target"},
                   {"components", nlohmann::json::array({{{"name", "transform"}}})}},
                  {{"name", "OrbitCam"},
                   {"components",
                    nlohmann::json::array(
                        {{{"name", "transform"}},
                         {{"name", "camera"},
                          {"controller",
                           {{"type", "orbit"}, {"target", "Target"}, {"distance", 4.0}}}}})}}})}}}}}};
    const auto document = AuthoringSceneDocument::load(source.dump(), SceneRevision{91});
    const auto service = makeQueryService(document);
    const EditorCommandRpcAdapter rpc{service};
    const auto response = rpc.getComponents({{"name", "OrbitCam"}});
    const auto &components = response.at("components");
    const auto camera = std::find_if(components.begin(), components.end(), [](const auto &component) {
        return component.at("name") == "camera";
    });
    REQUIRE(camera != components.end());
    REQUIRE_FALSE(camera->contains("resolution_error"));
    const auto &fields = camera->at("schema").at("fields");
    const auto field = [&](std::string_view name) {
        const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto &entry) {
            return entry.at("name") == std::string{name};
        });
        REQUIRE(found != fields.end());
        return *found;
    };
    CHECK(field("controller.type") ==
          nlohmann::ordered_json{{"name", "controller.type"},
                                 {"type", "enum"},
                                 {"enum", {"orbit", "follow", "fly"}}});
    CHECK(field("controller.distance") ==
          nlohmann::ordered_json{{"name", "controller.distance"},
                                 {"type", "f32"},
                                 {"range", {0.0, 1.0e12}},
                                 {"discriminant", "controller.type"},
                                 {"applies_to", {"orbit"}},
                                 {"required_for", {"orbit"}}});
    CHECK(field("controller.sensitivity") ==
          nlohmann::ordered_json{{"name", "controller.sensitivity"},
                                 {"type", "f32"},
                                 {"range", {0.0, 1.0e12}},
                                 {"default", 1.0},
                                 {"discriminant", "controller.type"},
                                 {"applies_to", {"orbit", "fly"}},
                                 {"required_for", {"fly"}}});
    CHECK(field("controller.yaw") ==
          nlohmann::ordered_json{{"name", "controller.yaw"},
                                 {"type", "f32"},
                                 {"unit", "rad"},
                                 {"default", 0.0},
                                 {"discriminant", "controller.type"},
                                 {"applies_to", {"orbit"}}});
    CHECK(field("controller.offset").at("type") == "vec3");
    CHECK_FALSE(field("controller.offset").contains("default"));
    CHECK(field("controller.target").at("default") == "");
    CHECK(field("controller.damping").at("applies_to") == nlohmann::ordered_json{"orbit", "follow"});
    // A field outside the controller is unchanged.
    CHECK_FALSE(field("yfov").contains("applies_to"));
    CHECK_FALSE(field("yfov").contains("default"));

    // The addition catalog offers the camera with the same field list.
    const auto candidates = rpc.getComponentAddCandidates({{"name", "Target"}}).at("candidates");
    const auto candidate = std::find_if(candidates.begin(), candidates.end(), [](const auto &entry) {
        return entry.at("name") == "camera";
    });
    REQUIRE(candidate != candidates.end());
    CHECK(candidate->at("schema_fields") == fields);
    std::cout << "SC1_CAMERA_CONTROLLER_SCHEMA ";
    for (const auto &entry : fields)
        if (entry.at("name").get<std::string>().starts_with("controller.")) std::cout << entry.dump() << ' ';
    std::cout << '\n';
}

// PG-1: the builtin ray instance used to be cut off the end of the codec
// catalog outside a layout-v2 ray tracing project, so the Studio could not
// tell "not here" from "not in this project". It is listed now, unavailable,
// under the name scene prepare refuses it with.
TEST_CASE("PG-1 the ray instance row is listed unavailable outside a layout-v2 project",
          "[editor-command][query][component-addition][pg1]") {
    FastModuleContainer modules;
    const auto document = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{41});
    const auto service = makeQueryService(document);
    const EditorCommandRpcAdapter rpc{service};
    const auto response = rpc.getComponentAddCandidates({{"name", "MixedObject"}});
    const auto &candidates = response.at("candidates");
    const auto ray = std::find_if(candidates.begin(), candidates.end(), [](const auto &candidate) {
        return candidate.at("name") == "pelican_ray_instance";
    });
    REQUIRE(ray != candidates.end());
    CHECK(ray->at("available") == false);
    CHECK(ray->at("unavailable_reason") == "pelican.ray_instance_requires_layout_v2@1");
    CHECK_FALSE(ray->contains("component"));
    // The decoding catalog is unchanged: outside layout v2 the name may belong
    // to an external ECS registrar.
    CHECK(findComponentCodec("pelican_ray_instance") == nullptr);
}

TEST_CASE("component asset choices include inline procedural assets and retain missing texture status",
          "[editor-command][query][component-addition][asset-picker]") {
    const auto document = AuthoringSceneDocument::load(readJson(authoringFixturePath()).dump(), SceneRevision{41});
    const auto service = makeQueryService(document, {
        {.id = "left", .kind = "procedural_geometry", .store = "project", .status = "declared"},
        {.id = "right", .kind = "procedural_geometry", .store = "project", .status = "declared"}});
    const auto catalog = service.getComponentAddCandidates({.name = "MixedObject"});
    const auto &candidates = catalog.at("candidates");
    const auto find = [&](std::string_view name) -> const nlohmann::ordered_json & {
        const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto &candidate) {
            return candidate.at("name").template get<std::string>() == name;
        });
        REQUIRE(found != candidates.end());
        return *found;
    };
    const auto &selection = find("pelican_procedural_geometry").at("asset_selection");
    CHECK(selection.at("field") == "assets");
    CHECK(selection.at("multiple") == true);
    CHECK(selection.at("single_field") == "asset");
    REQUIRE(selection.at("choices").size() == 2);
    CHECK(selection.at("choices")[0].at("id") == "left");
    CHECK(selection.at("choices")[0].at("path") == "");
    CHECK(selection.at("choices")[0].at("status") == "declared");
    const auto &texture = find("sprite_view").at("asset_selection");
    REQUIRE(texture.at("choices").size() == 1);
    CHECK(texture.at("choices")[0].at("status") == "missing");
}

TEST_CASE("editor asset collection includes inline geometry without requiring a file",
          "[editor-command][asset-picker]") {
    PathResolver resolver;
    const auto assets = collectEditorAssets(nlohmann::json{
        {"procedural_geometry", {{{"name", "sphere"}}}},
        {"models", {{{"name", "existing"}, {"path", authoringFixturePath().string()}},
                    {{"name", "no_file"}}}}}.dump(), resolver);
    // GP-5b: the three engine primitives lead every list.
    REQUIRE(assets.size() == 5);
    const auto geometry = std::find_if(assets.begin(), assets.end(), [](const auto &asset) {
        return asset.id == "sphere";
    });
    REQUIRE(geometry != assets.end());
    CHECK(geometry->kind == "procedural_geometry");
    CHECK(geometry->path.empty());
    CHECK(geometry->status == "declared");
    const auto model = std::find_if(assets.begin(), assets.end(), [](const auto &asset) {
        return asset.id == "existing";
    });
    REQUIRE(model != assets.end());
    CHECK(model->kind == "model");
    CHECK(model->status == "loaded");
}

namespace {

const std::vector<std::string> engine_primitive_ids{
    "engine://primitives/cube", "engine://primitives/plane", "engine://primitives/sphere"};

EditorCommandService makeCollectedAssetService(const AuthoringSceneDocument &document,
                                               std::string asset_data_json) {
    return EditorCommandService{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .runtime_query = fakeRuntime,
        .assets = [asset_data_json = std::move(asset_data_json)] {
            PathResolver resolver;
            return collectEditorAssets(asset_data_json, resolver);
        },
    }};
}

} // namespace

TEST_CASE("list_assets names the engine primitives as builtin in a project without asset data",
          "[editor-command][asset-picker][gp5b]") {
    const auto document =
        AuthoringSceneDocument::load(readJson(authoringFixturePath()).dump(), SceneRevision{41});
    const auto service = makeCollectedAssetService(document, "{}");
    const EditorCommandRpcAdapter rpc{service};
    const auto listed = rpc.listAssets(nlohmann::json::object());
    const auto &assets = listed.at("assets");
    REQUIRE(assets.size() == 3);
    for (std::size_t index = 0; index < engine_primitive_ids.size(); ++index) {
        CAPTURE(index);
        CHECK(assets[index] == nlohmann::ordered_json{{"id", engine_primitive_ids[index]},
                                                      {"kind", "model"},
                                                      {"path", engine_primitive_ids[index]},
                                                      {"store", "engine"},
                                                      {"status", "builtin"}});
    }
    CHECK(rpc.listAssets({{"store", "engine"}}).at("assets").size() == 3);
    CHECK(rpc.listAssets({{"store", "project"}}).at("assets").empty());
}

TEST_CASE("list_assets does not call a model declared as an engine primitive missing",
          "[editor-command][asset-picker][gp5b]") {
    PathResolver resolver;
    const auto assets = collectEditorAssets(nlohmann::json{
        {"models", {{{"name", "box"}, {"path", "engine://primitives/cube"}},
                    {{"name", "ball"}, {"path", "engine://primitives/sphere"}},
                    {{"name", "cone"}, {"path", "engine://primitives/cone"}}}}}.dump(), resolver);
    const auto find = [&](std::string_view id) -> const EditorAssetQueryResult & {
        const auto found = std::find_if(assets.begin(), assets.end(), [&](const auto &asset) {
            return asset.id == id;
        });
        REQUIRE(found != assets.end());
        return *found;
    };
    for (const auto id : {"box", "ball"}) {
        CAPTURE(id);
        const auto &asset = find(id);
        CHECK(asset.kind == "model");
        CHECK(asset.store == "engine");
        CHECK(asset.status == "builtin");
    }
    CHECK(find("box").path == "engine://primitives/cube");
    // An unknown primitive name does not resolve, so it stays missing.
    CHECK(find("cone").status == "missing");
    CHECK(std::count_if(assets.begin(), assets.end(), [](const auto &asset) {
              return asset.status == "missing";
          }) == 1);
}

// EB-1: a model's path is listed as asset_data writes it (the published copy
// holds it resolved, which goes to resolved_path), and a material document is
// listed once per material it holds, by name; one that does not read is
// listed by its path as invalid, one that is not there as missing.
TEST_CASE("EB-1 list_assets lists project-relative paths beside the resolved file, and materials by name",
          "[editor-command][asset-picker][eb1]") {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_eb1_list_assets_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};
    const auto write = [&](const std::string &relative, const std::string &contents) {
        std::filesystem::create_directories((root / relative).parent_path());
        std::ofstream{root / relative, std::ios::binary} << contents;
    };
    write("models/box.glb", "glTF");
    write("materials/paint.material.json",
          R"({"schema":"pelican.material","version":1,"materials":[{"name":"paint"},{"name":"coat"}]})");
    write("materials/broken.material.json", "not json");
    PathResolver resolver;
    resolver.setup(root, false);

    const auto absolute = (root / "models/box.glb").generic_string();
    const auto materials = nlohmann::json::array({{{"path", "materials/paint.material.json"}},
                                                  {{"path", "materials/broken.material.json"}},
                                                  {{"path", "materials/gone.material.json"}}});
    const auto published =
        nlohmann::json{{"models", {{{"name", "box"}, {"path", absolute}}}}, {"materials", materials}}.dump();
    const auto authored =
        nlohmann::json{{"models", {{{"name", "box"}, {"path", "project://models/box.glb"}}}}, {"materials", materials}}
            .dump();

    const auto find = [](const std::vector<EditorAssetQueryResult> &assets,
                         std::string_view id) -> const EditorAssetQueryResult & {
        const auto found = std::find_if(assets.begin(), assets.end(), [&](const auto &asset) { return asset.id == id; });
        REQUIRE(found != assets.end());
        return *found;
    };
    // With the document the copy was made from: its spelling.
    const auto assets = collectEditorAssets(published, resolver, authored);
    const auto &box = find(assets, "box");
    CHECK(box.path == "project://models/box.glb");
    REQUIRE(box.resolved_path);
    CHECK(std::filesystem::path{*box.resolved_path}.is_absolute());
    CHECK(std::filesystem::equivalent(*box.resolved_path, root / "models/box.glb"));
    CHECK(box.status == "loaded");
    // Without it: project-relative.
    CHECK(find(collectEditorAssets(published, resolver), "box").path == "models/box.glb");

    for (const auto id : {"paint", "coat"}) {
        CAPTURE(id);
        const auto &material = find(assets, id);
        CHECK(material.kind == "material");
        CHECK(material.path == "materials/paint.material.json");
        CHECK(material.store == "project");
        CHECK(material.status == "loaded");
        REQUIRE(material.resolved_path);
        CHECK(std::filesystem::equivalent(*material.resolved_path, root / "materials/paint.material.json"));
    }
    const auto &broken = find(assets, "materials/broken.material.json");
    CHECK(broken.kind == "material");
    CHECK(broken.status == "invalid");
    const auto &gone = find(assets, "materials/gone.material.json");
    CHECK(gone.kind == "material");
    CHECK(gone.status == "missing");
    CHECK_FALSE(gone.resolved_path);

    // The wire form: resolved_path only where a file resolved.
    const auto document = AuthoringSceneDocument::load(readJson(authoringFixturePath()).dump(), SceneRevision{41});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .runtime_query = fakeRuntime,
        .assets = [&] { return collectEditorAssets(published, resolver, authored); },
    }};
    const EditorCommandRpcAdapter rpc{service};
    const auto listed = rpc.listAssets(nlohmann::json::object());
    for (const auto &asset : listed.at("assets")) {
        CAPTURE(asset.dump());
        const bool engine = asset.at("store") == "engine";
        const bool missing = asset.at("status") == "missing";
        CHECK(asset.contains("resolved_path") == (!engine && !missing));
    }
}

TEST_CASE("simplemodelview choices offer the engine primitives before the project's models",
          "[editor-command][query][component-addition][asset-picker][gp5b]") {
    const auto document =
        AuthoringSceneDocument::load(readJson(authoringFixturePath()).dump(), SceneRevision{41});
    const auto service = makeCollectedAssetService(document, nlohmann::json{
        {"models", {{{"name", "existing"}, {"path", authoringFixturePath().string()}}}}}.dump());
    const EditorCommandRpcAdapter rpc{service};
    const auto catalog = rpc.getComponentAddCandidates({{"name", "MixedObject"}});
    const auto &candidates = catalog.at("candidates");
    const auto model = std::find_if(candidates.begin(), candidates.end(), [](const auto &candidate) {
        return candidate.at("name") == "simplemodelview";
    });
    REQUIRE(model != candidates.end());
    const auto &choices = model->at("asset_selection").at("choices");
    REQUIRE(choices.size() == 4);
    for (std::size_t index = 0; index < engine_primitive_ids.size(); ++index) {
        CAPTURE(index);
        CHECK(choices[index] == nlohmann::ordered_json{{"id", engine_primitive_ids[index]},
                                                       {"path", engine_primitive_ids[index]},
                                                       {"status", "builtin"}});
    }
    CHECK(choices[3].at("id") == "existing");
    CHECK(choices[3].at("status") == "loaded");
    // The chosen id is what the component takes: it decodes as authored.
    auto authored = model->at("component_template");
    authored["model"] = choices[0].at("id");
    CHECK(requireComponentCodec("simplemodelview").decodeAuthored(authored).hasValue());
    // A texture picker is not offered a mesh.
    const auto sprite = std::find_if(candidates.begin(), candidates.end(), [](const auto &candidate) {
        return candidate.at("name") == "sprite_view";
    });
    REQUIRE(sprite != candidates.end());
    CHECK(sprite->at("asset_selection").at("choices").empty());
}

TEST_CASE("behavior addition catalog provides defaults without constructing an attachment",
          "[editor-command][query][component-addition][behavior]") {
    auto &registry = internal::getBehaviorRegisterer();
    const std::string type = "editor_catalog_behavior";
    const auto token = registry.registerBehavior<CatalogBehavior>(type, 1, {});
    struct Cleanup {
        internal::RegistrationToken token;
        ~Cleanup() { internal::unregisterBehavior(token); }
    } cleanup{token};
    const auto constructions = CatalogBehavior::constructions;
    auto source = readJson(authoringFixturePath());
    source["scenes"]["main"]["objects"][1]["components"].push_back(
        {{"name", "behavior"}, {"type", type}, {"params", {{"speed", 9.0}}}});
    const auto document = AuthoringSceneDocument::load(source.dump(), SceneRevision{41});
    const auto service = makeQueryService(document);
    const EditorCommandRpcAdapter rpc{service};
    const auto catalog = rpc.getComponentAddCandidates({{"name", "MixedObject"}});
    const auto &candidates = catalog.at("candidates");
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto &candidate) {
        return candidate.at("key") == "behavior:" + type;
    });
    REQUIRE(found != candidates.end());
    CHECK(found->at("available") == true);
    CHECK(found->at("name") == "behavior");
    CHECK(found->at("behavior_type") == type);
    CHECK(found->at("component_template") == nlohmann::ordered_json{
        {"name", "behavior"}, {"type", type}, {"params", {{"speed", 2.0}}}});
    CHECK(found->at("component") == found->at("component_template"));
    REQUIRE(found->at("schema_fields").size() == 1);
    CHECK(found->at("schema_fields")[0].at("name") == "speed");
    CHECK(CatalogBehavior::constructions == constructions);
}

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

TEST_CASE("ray occurrence production RPC exposes inventory and tri-state resolution",
          "[editor-command][rpc][wp382c]") {
    auto document = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{1});
    const auto occurrence = RayOccurrenceCatalogEntry{
        .occurrence_id = "0123456789abcdef0123456789abcdef",
        .display_name = "hero:Body/0",
        .asset = "hero",
        .node_path = {"Root", "Body"},
        .mesh = 2,
        .primitive = 0,
        .material = "body",
        .variant = "none",
        .eligibility = RayOccurrenceEligibilityKind::ok,
        .q = 7,
        .authoring_object_id = 1,
    };
    RayGroupLayout layout;
    layout.source_reference = "layout.json";
    layout.groups = {"default", "surface"};
    layout.group_types = {RayGroupType::triangles, RayGroupType::triangles};
    layout.default_group = "default";
    layout.data_size = 32;
    layout.record = {{"signed", RayRecordType::i32, 0, 4, 4},
        {"weight", RayRecordType::f32, 4, 4, 4}, {"color", RayRecordType::vec3, 16, 12, 16}};
    RayMaterialBinding row;
    row.selector.reference = "body.json";
    row.group_index = 1;
    row.record_data.resize(32);
    const std::int32_t signed_value = -12;
    const float weight = 0.5F;
    const float color[]{1, 2, 3};
    std::memcpy(row.record_data.data(), &signed_value, 4);
    std::memcpy(row.record_data.data() + 4, &weight, 4);
    std::memcpy(row.record_data.data() + 16, color, 12);
    RayBindings authored_bindings;
    authored_bindings.source_reference = "bindings.json";
    authored_bindings.bindings.push_back(std::move(row));
    const std::array inputs{RaySceneRowInput{
        .scene_identity = "MixedObject", .material_document = "body.json"}};
    auto plan = std::make_shared<RaySceneRowSnapshot>(
        resolveRaySceneRows(inputs, layout, &authored_bindings));
    REQUIRE(plan->rows.size() == 1);
    plan->rows[0].q = 7;
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .ray_occurrences = [occurrence, plan](std::uint64_t) {
            return EditorRayOccurrencesResult{.entries = {occurrence}, .plan = plan,
                                              .authoring_object_id = 1};
        },
        .resolve_ray_occurrence = [occurrence](std::string_view id) {
            if (id != occurrence.occurrence_id)
                return RayOccurrenceResolution{};
            return RayOccurrenceResolution{
                .state = RayOccurrenceResolveState::found,
                .candidates = {occurrence},
            };
        },
    }};
    const EditorCommandRpcAdapter rpc{service};
    const auto inventory = rpc.getRayOccurrences(
        nlohmann::json{{"name", "MixedObject"}});
    REQUIRE(inventory.at("entries").size() == 1);
    REQUIRE(inventory.at("entries").at(0).at("occurrence_id") ==
            occurrence.occurrence_id);
    REQUIRE(inventory.at("entries").at(0).at("eligibility") == "ok");

    const auto &binding = inventory.at("entries").at(0).at("binding");
    CHECK(binding.at("record_source") == "scene_plan");
    CHECK(binding.at("group_name") == "surface");
    CHECK(binding.at("group_source") == "binding");
    CHECK(binding.at("layout_reference") == "layout.json");
    CHECK(binding.at("binding_reference") == "bindings.json");
    CHECK(binding.at("binding_index") == 0);
    CHECK(binding.at("record_value_source") == "binding");
    REQUIRE(binding.at("record_fields").size() == 3);
    CHECK(binding.at("record_fields").at(0).at("value") == -12);
    CHECK(binding.at("record_fields").at(1).at("value") == 0.5);
    CHECK(binding.at("record_fields").at(2).at("value") == nlohmann::json::array({1, 2, 3}));
    CHECK(binding.at("record_fields").at(2).at("offset") == 16);

    *plan = resolveRaySceneRows(inputs, layout, nullptr);
    REQUIRE(plan->rows.size() == 1);
    plan->rows[0].q = 7;
    const auto defaults = rpc.getRayOccurrences(nlohmann::json{{"name", "MixedObject"}});
    const auto &default_binding = defaults.at("entries").at(0).at("binding");
    CHECK(default_binding.at("group_name") == "default");
    CHECK(default_binding.at("group_source") == "layout_default");
    CHECK(default_binding.at("binding_index").is_null());
    CHECK(default_binding.at("binding_reference").is_null());
    CHECK(default_binding.at("record_value_source") == "zero_default");
    CHECK(default_binding.at("setting_sources").at("mask") == "engine_default");
    CHECK(default_binding.at("setting_sources").at("custom_index") == "engine_default");
    CHECK(default_binding.at("setting_sources").at("geometry_rule_index").is_null());
    CHECK(default_binding.at("setting_sources").at("override_row_id").is_null());
    CHECK(default_binding.at("mask") == 255);
    CHECK(default_binding.at("record_fields").at(0).at("value") == 0);

    const auto found = rpc.resolveRayOccurrence(
        nlohmann::json{{"occurrence_id", occurrence.occurrence_id}});
    REQUIRE(found.at("status") == "found");
    REQUIRE(found.at("q") == 7);
    const auto absent = rpc.resolveRayOccurrence(nlohmann::json{
        {"occurrence_id", "ffffffffffffffffffffffffffffffff"}});
    REQUIRE(absent == nlohmann::ordered_json{
                          {"status", "absent"},
                          {"occurrence_id",
                           "ffffffffffffffffffffffffffffffff"}});
}

TEST_CASE("get_components publishes recursive v3 component catalog nodes",
          "[editor-command][query][wp382a][schema-v3]") {
    const auto owner = internal::allocateRegistrationOwner();
    internal::RegistrationToken token;
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        token = internal::getComponentRegisterer()
                    .registerComponent<wp382a_catalog_fixture::Component>(
                        "wp382a_rpc_component");
    }
    const nlohmann::json source{
        {"schema", "pelican.scene"},
        {"version", 2},
        {"scenes",
         {{"main",
           {{"objects",
             nlohmann::json::array(
                 {{{"name", "CatalogCarrier"},
                   {"components",
                    nlohmann::json::array(
                        {{{"name", "wp382a_rpc_component"},
                          {"rows",
                           {{"$list", {{"v", 1}, {"next_id", 1}}},
                            {"rows", nlohmann::json::array()}}}}})}}})}}}}}};
    const auto document = AuthoringSceneDocument::load(
        source.dump(), SceneRevision{382});
    const auto service = makeQueryService(document);
    const EditorCommandRpcAdapter rpc{service};
    const auto response = rpc.getComponents({{"name", "CatalogCarrier"}});

    const auto &catalog_component = response.at("components").at(0);
    REQUIRE(catalog_component.at("name") == "wp382a_rpc_component");
    const auto &root = catalog_component.at("schema").at("node");
    REQUIRE(root.at("tag") == "struct");
    const auto &list = root.at("children").at(0);
    REQUIRE(list.at("tag") == "list");
    REQUIRE(list.at("presence") == "defaulted");
    REQUIRE(list.at("default").at("$list").at("v") == 1);
    const auto &row = list.at("children").at(0);
    REQUIRE(row.at("children").at(1).at("presence") ==
            "optional_override");
    const auto &flags = row.at("children").at(2);
    REQUIRE(flags.at("presence") == "optional_override");
    REQUIRE(flags.at("vocabulary").at(0).at("name") == "A");
    REQUIRE(flags.at("vocabulary").at(0).at("value") == 1);
    REQUIRE(flags.at("vocabulary").at(1).at("value") == 2);
    REQUIRE(row.at("children").at(3).at("default") == false);

    const auto &component = GET_MODULE(ComponentInfoManager).get(
        GET_MODULE(ComponentInfoManager)
            .getComponentIdByName("wp382a_rpc_component"));
    REQUIRE(component.schema_v3.has_value());
    const auto providers = buildProductionBindableProviderSnapshot();
    const auto provider = std::ranges::find_if(
        providers.descriptors(), [](const auto &entry) {
            return entry.provider_name == "component:wp382a_rpc_component";
        });
    REQUIRE(provider != providers.descriptors().end());
    REQUIRE(provider->schema_node == component.schema_v3);

    GET_MODULE(ComponentInfoManager).unregisterComponent(token);
    REQUIRE(internal::releaseRegistrationOwner(owner) ==
            internal::RegistrationOwnerReleaseResult::released);
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

TEST_CASE("ExportSceneSnapshot fixture uses semantic bytes and their real sha256",
          "[editor-command][snapshot][wp154]") {
    const auto fixture = readJson(fixturePath("export_scene_snapshot_v1.json"));
    const auto document = AuthoringSceneDocument::load(fixture.at("document").dump(), SceneRevision{42});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .snapshot_state = [] { return EditorSnapshotState{.preview_epoch = 7}; },
    }};
    const EditorCommandRpcAdapter rpc{service};
    const EditorCommandImGuiFakeAdapter imgui{service};

    const auto rpc_response = rpc.exportSceneSnapshot(fixture.at("request"));
    const auto typed_response = imgui.exportSceneSnapshotV2({.schema_version = 2, .allow_pending = false});
    // The closure is SnapshotV2's own (checked by the WP362 tests); the rest
    // of the response is the fixture's.
    auto without_closure = nlohmann::json(rpc_response);
    REQUIRE(without_closure.at("prefab_closure").at("scenes").size() == 1);
    without_closure.erase("prefab_closure");
    REQUIRE(without_closure == fixture.at("expected_response"));
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

TEST_CASE("ExportSceneSnapshot validates strict request fields and pending state",
          "[editor-command][snapshot][negative][wp154]") {
    const auto fixture = readJson(fixturePath("export_scene_snapshot_v1.json"));
    const auto document = AuthoringSceneDocument::load(fixture.at("document").dump(), SceneRevision{8});
    const EditorCommandService busy_service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .snapshot_state = [] {
            return EditorSnapshotState{.pending_ticket_ids = {"ticket-7"},
                                       .open_preview_lease = true,
                                       .preview_epoch = 9};
        },
    }};
    const EditorCommandRpcAdapter rpc{busy_service};

    REQUIRE(errorCode([&] { (void)rpc.exportSceneSnapshot({{"schema_version", 2}}); }) ==
            EditorCommandErrorCode::SnapshotBusy);
    const auto allowed = rpc.exportSceneSnapshot({{"schema_version", 2}, {"allow_pending", true}});
    REQUIRE(allowed.at("pending_ticket_ids") == nlohmann::ordered_json::array({"ticket-7"}));
    REQUIRE(allowed.at("preview_epoch") == 9);

    REQUIRE(errorCode([&] { (void)rpc.exportSceneSnapshot({{"schema_version", 3}}); }) ==
            EditorCommandErrorCode::UnsupportedSnapshotVersion);
    REQUIRE(errorCode([&] { (void)rpc.exportSceneSnapshot({{"schema_version", 2.0}}); }) ==
            EditorCommandErrorCode::InvalidParams);
    REQUIRE(errorCode([&] {
                (void)rpc.exportSceneSnapshot({{"schema_version", 2}, {"allow_pending", nullptr}});
            }) == EditorCommandErrorCode::InvalidParams);
    REQUIRE(errorCode([&] {
                (void)rpc.exportSceneSnapshot({{"schema_version", 2}, {"unknown", false}});
            }) == EditorCommandErrorCode::InvalidParams);
}

TEST_CASE("ExportSceneSnapshot rejects semantic payloads over 64 MiB",
          "[editor-command][snapshot][negative][wp154]") {
    auto oversized_document = readJson(fixturePath("export_scene_snapshot_v1.json")).at("document");
    oversized_document["padding"] = std::string(maxSceneSnapshotBytes, 'x');
    const auto document = AuthoringSceneDocument::load(oversized_document.dump(), SceneRevision{9});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .snapshot_state = [] { return EditorSnapshotState{}; },
    }};

    const ExportSceneSnapshotRequestV2 request{.schema_version = 2};
    const auto code = errorCode([&] { (void)service.exportSceneSnapshotV2(request); });
    REQUIRE(code == EditorCommandErrorCode::SnapshotTooLarge);
}

TEST_CASE("ImportSceneSnapshot round trips all semantic bytes with fresh identities",
          "[editor-command][snapshot][import][roundtrip][wp168]") {
    const auto source = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{42});
    const EditorCommandService source_service{EditorCommandServiceDependencies{
        .document = [&source]() -> const AuthoringSceneDocument & { return source; },
        .resolved_scene = explicitResolvedProvider(source),
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    const EditorCommandRpcAdapter source_rpc{source_service};
    const auto exported = source_service.exportSceneSnapshotV2({});
    const auto exported_json = source_rpc.exportSceneSnapshot(
        {{"schema_version", 2}, {"allow_pending", false}});

    auto target = AuthoringSceneDocument::load(
        R"({"schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}})",
        SceneRevision{7});
    std::string target_scene = "main";
    std::size_t import_calls = 0;
    EditorCommandService target_service{EditorCommandServiceDependencies{
        .document = [&target]() -> const AuthoringSceneDocument & { return target; },
        .resolved_scene = explicitResolvedProvider(target),
        .current_scene_id = [&target_scene] { return target_scene; },
        .import_scene_snapshot_versioned =
            [&](std::string_view bytes, std::string_view current_scene_id,
                std::uint64_t schema_version) {
                REQUIRE(schema_version == 2);
                ++import_calls;
                target = AuthoringSceneDocument::load(
                    bytes, SceneRevision{target.revision().value + 1U}, 1000);
                target_scene = std::string{current_scene_id};
                return SceneImportResult{
                    target.revision(),
                    CommittedWithEffectFailure{
                        {},
                        {{"saveData", "persistence_write_failed@1",
                          {{"path", "slot-c.json"}}, "flush"}},
                        {}}};
            },
    }};
    EditorCommandRpcAdapter target_rpc{target_service};

    const auto response =
        target_rpc.importSceneSnapshot(importRequestJson(exported_json));
    REQUIRE(response.at("status") == "imported");
    REQUIRE(response.at("committed") == true);
    REQUIRE(response.at("scene_revision") == 8);
    REQUIRE(response.at("current_scene_id") == "main");
    REQUIRE(response.at("effect_errors").size() == 1);
    REQUIRE(response.at("effect_errors").at(0).at("api") == "saveData");
    REQUIRE(response.at("effect_errors").at(0).at("error_code") ==
            "persistence_write_failed@1");
    REQUIRE(import_calls == 1);

    const auto round_tripped = target_service.exportSceneSnapshotV2({});
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

TEST_CASE("ImportSceneSnapshot enforces five ordered gates without publication",
          "[editor-command][snapshot][import][negative][fault][wp168]") {
    auto target = AuthoringSceneDocument::load(
        R"({"schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}})",
        SceneRevision{11});
    const auto source = AuthoringSceneDocument::load(
        readJson(authoringFixturePath()).dump(), SceneRevision{21});
    const EditorCommandService source_service{EditorCommandServiceDependencies{
        .document = [&source]() -> const AuthoringSceneDocument & { return source; },
        .resolved_scene = explicitResolvedProvider(source),
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    auto valid = importRequest(source_service.exportSceneSnapshotV2({}));

    bool inject_fault = false;
    std::size_t import_calls = 0;
    EditorCommandService target_service{EditorCommandServiceDependencies{
        .document = [&target]() -> const AuthoringSceneDocument & { return target; },
        .resolved_scene = explicitResolvedProvider(target),
        .current_scene_id = [] { return std::string{"main"}; },
        .import_scene_snapshot_versioned =
            [&](std::string_view bytes, std::string_view, std::uint64_t) {
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
    unsupported.schema_version = 3;
    unsupported.semantic_scene_bytes.assign(maxSceneSnapshotBytes + 1U, 'x');
    REQUIRE(errorCode([&] { (void)target_service.importSceneSnapshotV2(unsupported); }) ==
            EditorCommandErrorCode::UnsupportedSnapshotVersion);
    require_unpublished();

    auto oversized = valid;
    oversized.semantic_scene_bytes.assign(maxSceneSnapshotBytes + 1U, 'x');
    oversized.digest.hex = "wrong";
    REQUIRE(errorCode([&] { (void)target_service.importSceneSnapshotV2(oversized); }) ==
            EditorCommandErrorCode::SnapshotTooLarge);
    require_unpublished();

    auto mismatched = valid;
    mismatched.digest.hex = std::string(64, '0');
    REQUIRE(errorCode([&] { (void)target_service.importSceneSnapshotV2(mismatched); }) ==
            EditorCommandErrorCode::DigestMismatch);
    require_unpublished();

    auto invalid = valid;
    invalid.semantic_scene_bytes = "{not-json";
    invalid.digest = digestFor(invalid.semantic_scene_bytes);
    std::optional<std::string> invalid_detail;
    try {
        (void)target_service.importSceneSnapshotV2(invalid);
    } catch (const EditorCommandError &error) {
        REQUIRE(error.code() == EditorCommandErrorCode::SnapshotInvalid);
        invalid_detail = error.detail();
    }
    REQUIRE(invalid_detail.has_value());
    REQUIRE_FALSE(invalid_detail->empty());
    require_unpublished();

    auto missing_scene = valid;
    missing_scene.current_scene_id = "absent";
    REQUIRE(errorCode([&] { (void)target_service.importSceneSnapshotV2(missing_scene); }) ==
            EditorCommandErrorCode::SceneNotFound);
    require_unpublished();
    REQUIRE(import_calls == 0);

    inject_fault = true;
    std::optional<std::string> fault_detail;
    try {
        (void)target_service.importSceneSnapshotV2(valid);
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
                (void)rpc.importSceneSnapshot({{"schema_version", 3}});
            }) == EditorCommandErrorCode::UnsupportedSnapshotVersion);
    REQUIRE(errorCode([&] {
                (void)rpc.importSceneSnapshot(
                    {{"schema_version", 2},
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
        R"({"schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}})",
        SceneRevision{1});
    std::size_t import_calls = 0;
    EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .import_scene_snapshot_versioned =
            [&](std::string_view, std::string_view, std::uint64_t) {
                ++import_calls;
                return SceneRevision{2};
            },
    }};
    EditorCommandRpcAdapter rpc{service};
    REQUIRE(errorCode([&] { (void)rpc.importSceneSnapshot(example); }) ==
            EditorCommandErrorCode::SnapshotInvalid);
    REQUIRE(import_calls == 0);
}

// MG-2b: SnapshotV1 carried a pelican.scene version 1 payload, which this
// engine does not read. Both RPCs refuse schema_version 1 by name before the
// payload is parsed, and nothing is published.
TEST_CASE("SnapshotV1 is refused by name on export and import",
          "[editor-command][snapshot][negative][mg2b]") {
    const auto document = AuthoringSceneDocument::load(
        R"({"schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}})",
        SceneRevision{1});
    std::size_t import_calls = 0;
    EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .import_scene_snapshot_versioned =
            [&](std::string_view, std::string_view, std::uint64_t) {
                ++import_calls;
                return SceneRevision{2};
            },
    }};
    EditorCommandRpcAdapter rpc{service};
    const auto require_v1_refused = [](auto &&call) {
        try {
            (void)call();
            FAIL("SnapshotV1 was accepted");
        } catch (const EditorCommandError &error) {
            REQUIRE(error.code() ==
                    EditorCommandErrorCode::UnsupportedSnapshotVersion);
            REQUIRE(std::string_view{error.what()}.find(
                        "pelican.scene version 1") != std::string_view::npos);
        }
    };
    require_v1_refused([&] {
        return rpc.exportSceneSnapshot(
            {{"schema_version", 1}, {"allow_pending", false}});
    });
    const std::string v1_bytes =
        R"({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[]}}})";
    require_v1_refused([&] {
        return rpc.importSceneSnapshot(
            {{"schema_version", 1},
             {"semantic_scene_bytes", v1_bytes},
             {"digest", {{"algorithm", "sha256"},
                         {"hex", digestFor(v1_bytes).hex}}},
             {"current_scene_id", "main"}});
    });
    // The version is the first gate: a request with nothing else in it is
    // still refused by the version name.
    require_v1_refused([&] {
        return rpc.importSceneSnapshot({{"schema_version", 1}});
    });
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

namespace {
nlohmann::json outlineFixture() {
    return nlohmann::json::parse(R"json({"schema":"pelican.scene","version":2,"scenes":{
      "main":{"objects":[
        {"name":"Child","parent":"Root","components":[{"name":"transform"}]},
        {"parent":"Root","components":[{"name":"transform"}]},
        {"name":"Root","components":[{"name":"transform"}]}
      ]},
      "secondary":{"objects":[{"name":"Root","components":[]}]},
      "empty":{"objects":[]}
    }})json");
}
} // namespace

TEST_CASE("scene_outline returns stable hierarchy in declaration order without detail queries",
          "[editor-command][scene-outline][rpc]") {
    const auto document = AuthoringSceneDocument::load(outlineFixture().dump(), SceneRevision{41});
    int runtime_calls = 0;
    int schema_calls = 0;
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [] { return std::string{"main"}; },
        .runtime_query = [&](const ResolvedSceneView &, const ResolvedObject &) -> EditorRuntimeObjectState {
            ++runtime_calls;
            throw std::logic_error("outline must not query runtime components");
        },
    }};
    EditorCommandRpcAdapter rpc{service, [&](StructFieldType) -> std::string {
        ++schema_calls;
        throw std::logic_error("outline must not expand schema");
    }};
    const EditorCommandImGuiFakeAdapter imgui{service};
    std::istringstream input;
    std::ostringstream output;
    RpcServer server{input, output};
    configureEditorRpcHandlers(server, rpc, {
        .snapshot_imported = [] {}, .save_busy = [] { return false; }});
    const auto wire = nlohmann::json::parse(server.processLine(
        R"json({"jsonrpc":"2.0","id":1,"method":"scene_outline","params":{}})json"));
    REQUIRE_FALSE(wire.contains("error"));
    const auto result = rpc.sceneOutline(nlohmann::json::object());
    REQUIRE(wire.at("result") == nlohmann::json(result));
    REQUIRE(result == editorQueryJson(imgui.sceneOutline()));
    REQUIRE(result.at("scene_id") == "main");
    REQUIRE(result.at("scene_revision") == 41);
    REQUIRE(result.at("not_modified") == false);
    const auto &objects = result.at("objects");
    REQUIRE(objects.size() == 3);
    const auto parent_id = objects.at(2).at("authoring_object_id");
    REQUIRE(objects.at(0).at("parent_authoring_object_id") == parent_id);
    REQUIRE(objects.at(1).at("parent_authoring_object_id") == parent_id);
    REQUIRE(objects.at(2).at("parent_authoring_object_id").is_null());
    REQUIRE(objects.at(0).at("name") == "Child");
    REQUIRE_FALSE(objects.at(1).contains("name"));
    REQUIRE(objects.at(2).at("name") == "Root");
    auto detailed_service = makeQueryService(document);
    const EditorCommandRpcAdapter detailed{detailed_service};
    const auto original_tree = detailed.sceneTree(nlohmann::json::object());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto &row = objects.at(i);
        REQUIRE(row.at("declaration_index") == i);
        REQUIRE(row.at("authoring_object_id").get<std::uint64_t>() != 0);
        REQUIRE(row.at("authoring_object_id") == original_tree.at("objects").at(i).at("authoring_object_id"));
        REQUIRE(row.size() == (i == 1 ? 3 : 4));
        const auto detail = detailed.getComponents({{"authoring_object_id", row.at("authoring_object_id")}});
        REQUIRE(detail.at("declaration_index") == i);
    }
    REQUIRE(rpc.sceneOutline({{"scene_id", "empty"}}).at("objects").empty());
    REQUIRE(runtime_calls == 0);
    REQUIRE(schema_calls == 0);
    REQUIRE(detailed.sceneTree(nlohmann::json::object()) == original_tree);
}

TEST_CASE("scene_outline conditional reads require scene identity and observe new revisions",
          "[editor-command][scene-outline][poll]") {
    auto document = AuthoringSceneDocument::load(outlineFixture().dump(), SceneRevision{41});
    std::string current = "main";
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = explicitResolvedProvider(document),
        .current_scene_id = [&] { return current; },
    }};
    const EditorCommandRpcAdapter rpc{service};
    const auto first = rpc.sceneOutline(nlohmann::json::object());
    REQUIRE(rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", 41}}) ==
            nlohmann::ordered_json{{"scene_revision", 41}, {"scene_id", "main"}, {"not_modified", true}});
    current = "secondary";
    REQUIRE(rpc.sceneOutline(nlohmann::json::object()).at("objects").size() == 1);
    REQUIRE(rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", 41}}).at("scene_id") == "main");
    REQUIRE(errorCode([&] { rpc.sceneOutline({{"if_scene_revision", 41}}); }) == EditorCommandErrorCode::InvalidParams);
    REQUIRE(errorCode([&] { rpc.sceneOutline({{"scene_id", "absent"}, {"if_scene_revision", 41}}); }) == EditorCommandErrorCode::SceneNotFound);
    const AuthoringObjectId child_id{first.at("objects").at(0).at("authoring_object_id").get<std::uint64_t>()};
    const AuthoringObjectId unnamed_id{first.at("objects").at(1).at("authoring_object_id").get<std::uint64_t>()};
    const AuthoringObjectId root_id{first.at("objects").at(2).at("authoring_object_id").get<std::uint64_t>()};
    auto changed = AuthoringSceneAuthority::structuralStage(document);
    changed.renameObject(child_id, "Renamed");
    document = std::move(changed).finish(SceneRevision{42});
    const auto next = rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", 41}});
    REQUIRE(next.at("not_modified") == false);
    REQUIRE(next.at("scene_revision") == 42);
    REQUIRE(next.at("objects").at(0).at("name") == "Renamed");
    REQUIRE(next.at("objects").at(0).at("authoring_object_id") == first.at("objects").at(0).at("authoring_object_id"));
    REQUIRE(rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", 42}}).at("not_modified") == true);
    REQUIRE(rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", 43}}).contains("objects"));
    auto reordered = AuthoringSceneAuthority::structuralStage(document);
    reordered.reorderObject(root_id, 0);
    reordered.rawJson()["scenes"]["main"]["objects"][2]["parent"] = "Renamed";
    document = std::move(reordered).finish(SceneRevision{43});
    const auto hierarchy = rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", 42}}).at("objects");
    REQUIRE(hierarchy.at(0).at("authoring_object_id") == root_id.value);
    REQUIRE(hierarchy.at(1).at("authoring_object_id") == child_id.value);
    REQUIRE(hierarchy.at(2).at("authoring_object_id") == unnamed_id.value);
    REQUIRE(hierarchy.at(2).at("parent_authoring_object_id") == child_id.value);
    for (std::size_t i = 0; i < hierarchy.size(); ++i) REQUIRE(hierarchy.at(i).at("declaration_index") == i);
    auto replaced = AuthoringSceneAuthority::structuralStage(document);
    (void)replaced.removeObject(unnamed_id);
    const auto inserted_id = replaced.insertObject("main", 1, {{"components", nlohmann::json::array()}});
    document = std::move(replaced).finish(SceneRevision{44});
    const auto replacement = rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", 43}}).at("objects");
    REQUIRE(inserted_id.value != 0);
    REQUIRE(inserted_id != unnamed_id);
    REQUIRE(replacement.at(1).at("authoring_object_id") == inserted_id.value);
    REQUIRE_FALSE(replacement.at(1).contains("name"));
    REQUIRE(replacement.at(1).at("parent_authoring_object_id").is_null());
}

TEST_CASE("scene_outline rejects malformed selectors on the registered RPC wire",
          "[editor-command][scene-outline][rpc]") {
    const auto document = AuthoringSceneDocument::load(outlineFixture().dump(), SceneRevision{41});
    const auto service = makeQueryService(document);
    EditorCommandRpcAdapter rpc{service};
    std::istringstream input;
    std::ostringstream output;
    RpcServer server{input, output};
    configureEditorRpcHandlers(server, rpc, {
        .snapshot_imported = [] {}, .save_busy = [] { return false; }});
    const std::vector<nlohmann::json> invalid{
        nlohmann::json::array(), {{"unknown", 1}}, {{"scene_id", ""}},
        {{"scene_id", nullptr}}, {{"if_scene_revision", 41}},
        {{"scene_id", "main"}, {"if_scene_revision", -1}},
        {{"scene_id", "main"}, {"if_scene_revision", 41.0}},
        {{"scene_id", "main"}, {"if_scene_revision", "41"}},
        {{"scene_id", "main"}, {"if_scene_revision", true}},
        {{"scene_id", "main"}, {"if_scene_revision", nullptr}},
        {{"scene_id", "main"}, {"if_scene_revision", UINT64_C(9007199254740992)}}};
    for (const auto &params : invalid) {
        const nlohmann::json request{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "scene_outline"}, {"params", params}};
        const auto response = nlohmann::json::parse(server.processLine(request.dump()));
        REQUIRE(response.at("error").at("code") == -32602);
    }
    REQUIRE_FALSE(rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", 0}}).at("not_modified").get<bool>());
    REQUIRE_FALSE(rpc.sceneOutline({{"scene_id", "main"}, {"if_scene_revision", maxExactEditorJsonInteger}}).at("not_modified").get<bool>());
}

} // namespace Pelican
