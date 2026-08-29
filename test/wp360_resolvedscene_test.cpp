#include "../src/core/build_features.hpp"
#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/loader/authoringsceneauthority.hpp"
#include "../src/core/loader/editorpreviewprojection.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/userpublic/behavior.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <picosha2.h>
#include <sstream>
#include <string>

#ifndef PELICAN_WP360_RPC_FIXTURE
#define PELICAN_WP360_RPC_FIXTURE ""
#endif
namespace Pelican {
namespace {

using Json = nlohmann::json;

struct Wp360WireParams {
    std::int32_t count{};
    std::string label;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Wp360WireParams::count>("count"),
                  std::int32_t{360}),
        defaulted(field<&Wp360WireParams::label>("label"),
                  "parent-default"));
};

class Wp360WireBehavior final : public Behavior {
  public:
    using Params = Wp360WireParams;
};

struct Wp360aRequiredLabelParams {
    std::string label;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Wp360aRequiredLabelParams::label>("label"),
                  "default"));
};

class Wp360aInactiveBehavior final : public Behavior {
  public:
    using Params = Wp360aRequiredLabelParams;
};

PELICAN_REGISTER_BEHAVIOR(Wp360aInactiveBehavior,
                          "wp360a_inactive_behavior", 1);

struct Wp360aSandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    explicit Wp360aSandbox(std::string_view name) {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() /
               ("pelican_wp360a_" + std::string{name} + "_" + suffix);
        root = base / "project";
        std::filesystem::create_directories(root);
    }
    ~Wp360aSandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

void writeWp360aText(const std::filesystem::path &path,
                     std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary};
    output << text;
}

void ensureWp360aLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

Json wp360aProjectJson(float up_y = 1.0f, float zfar = 1000.0f,
                       std::string_view default_scene = "default_scene") {
    return Json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "wp360a-seam"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"window_size", {{"width", 100}, {"height", 100}}},
          {"camera",
           {{"yfov", 0.5},
            {"znear", 0.1},
            {"zfar", zfar},
            {"up", {0.0, up_y, 0.0}}}},
          {"default_scene_id", default_scene},
          {"scene_data_json", "scene.json"}}}};
}

void setupWp360aProject(const Wp360aSandbox &sandbox,
                        const Json &project, const Json &scene) {
    writeWp360aText(sandbox.root / "scene.json", scene.dump());
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
}

constexpr std::string_view wireScene() {
    return R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"name":"WP360WireProbe","components":[{"name":"camera","type":"perspective","yfov":0.731},{"name":"behavior","type":"wp360_wire_behavior","params":{}},{"name":"light","type":"point","position":[7,8,9],"intensity":6.25,"color":[0.2,0.4,0.8]},{"name":"unknown_read_only","payload":{"identity":360}}]}]}}})json";
}

void ensureWireBehavior() {
    static const bool registered = [] {
        auto &registry = internal::getBehaviorRegisterer();
        if (registry.findByName("wp360_wire_behavior") == nullptr) {
            (void)registry.registerBehavior<Wp360WireBehavior>(
                "wp360_wire_behavior", 1, {});
        }
        return true;
    }();
    (void)registered;
}

std::string readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("failed to open WP360 fixture");
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

#if PELICAN_WITH_RPC
std::string currentWireBytes() {
    ensureWireBehavior();
    const auto document = AuthoringSceneDocument::load(
        wireScene(), SceneRevision{360});
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{360});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .resolved_scene = [&resolved]() -> const ResolvedScene & {
            return resolved;
        },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    EditorCommandRpcAdapter editor{service};
    std::istringstream input{
        R"json({"jsonrpc":"2.0","id":360,"method":"get_components","params":{"name":"WP360WireProbe"}})json"
        "\n"};
    std::ostringstream output;
    RpcServer server{input, output};
    configureEditorRpcHandlers(
        server, editor,
        EditorRpcHandlerHooks{.snapshot_imported = [] {},
                              .save_busy = [] { return false; }});
    server.run();
    return output.str();
}
#endif

} // namespace

TEST_CASE("WP360 resolver owns source/effective forms defaults and authored identity",
          "[wp360][resolved-scene][identity]") {
    ensureWireBehavior();
    const auto document = AuthoringSceneDocument::load(
        wireScene(), SceneRevision{41}, 900);
    const ResolvedSceneDefaults defaults{
        CameraProjectionSpec{.kind = CameraProjectionKind::Perspective,
                             .yfov = 0.44f,
                             .znear = 0.25f,
                             .zfar = 321.0f,
                             .aspect = 1.5f,
                             .xmag = 2.0f,
                             .ymag = 3.0f},
        CameraSpritePolicySpec{
            .pixel_perfect = CameraPixelPerfectMode::strict,
            .sort = CameraSpriteSortPolicy::declaration},
    };
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{77}, defaults);
    REQUIRE(resolved.revision() == SceneRevision{41});
    REQUIRE(resolved.resolverGeneration() == SceneResolverGeneration{77});
    const auto *scene = resolved.findScene("main");
    REQUIRE(scene != nullptr);
    REQUIRE(scene->objects.size() == 1);
    const auto &object = scene->objects.front();
    REQUIRE(object.authoring_object_id == AuthoringObjectId{900});
    REQUIRE(object.authoring_object_index == 0);
    REQUIRE(object.components.size() == 4);
    for (std::size_t index = 0; index < object.components.size(); ++index) {
        REQUIRE(object.components[index].authoring_component_index == index);
    }

    const auto raw = AuthoringSceneAuthority::rawView(document)
                         .scenesJson()
                         .at("main")
                         .at("objects")
                         .at(0)
                         .at("components");
    const auto &camera = object.components.at(0);
    REQUIRE(camera.source_json_exact == raw.at(0));
    REQUIRE_FALSE(camera.source_json_exact.contains("znear"));
    REQUIRE(camera.effective_json.at("yfov") == 0.731f);
    REQUIRE(camera.effective_json.at("znear") == 0.25f);
    REQUIRE(camera.effective_json.at("zfar") == 321.0f);
    REQUIRE(camera.effective_json.at("sprite").at("pixel_perfect") ==
            "strict");
    REQUIRE(camera.effective_json.at("sprite").at("sort") ==
            "declaration");

    const auto &behavior = object.components.at(1);
    REQUIRE(raw.at(1).at("params") == Json::object());
    REQUIRE(behavior.source_json_exact.at("params").at("count") == 360);
    REQUIRE(behavior.source_json_exact.at("params").at("label") ==
            "parent-default");
    REQUIRE(resolved.behaviorReloadSources().size() == 1);
    REQUIRE(resolved.behaviorReloadSources().front().source_params ==
            Json::object());
    REQUIRE(resolved.behaviorReloadSources().front().provenance.component_index ==
            1);

    const auto &light = object.components.at(2);
    REQUIRE(light.source_json_exact == raw.at(2));
    REQUIRE(light.effective_json.at("intensity") == 6.25f);
    REQUIRE(light.effective_json.at("position") == Json::array({7, 8, 9}));
    REQUIRE(object.components.at(3).source_json_exact == raw.at(3));
    REQUIRE(object.components.at(3).effective_json == raw.at(3));
    std::cout << "WP360_RESOLVED_IDENTITY revision="
              << resolved.revision().value << " generation="
              << resolved.resolverGeneration().value
              << " camera_zfar=" << camera.effective_json.at("zfar")
              << " light_intensity=" << light.effective_json.at("intensity")
              << " behavior_raw_params={} authored_indices=0,1,2,3\n";
}

TEST_CASE("WP360 generated requests return only the named unsupported error",
          "[wp360][resolved-scene][generated][negative]") {
    const auto document = AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"components":[{"name":"camera","generated":true}]}]}}})json",
        SceneRevision{1});
    try {
        (void)ResolvedSceneResolver::resolve(
            document, SceneResolverGeneration{1});
        FAIL("generated component unexpectedly resolved");
    } catch (const ResolvedSceneError &error) {
        REQUIRE(error.code() ==
                ResolvedSceneErrorCode::generated_component_unsupported);
        REQUIRE(std::string{error.what()}.find(
                    "generated_component_unsupported") != std::string::npos);
    }
}

TEST_CASE("WP360 physics matrix consumes the resolved collider with a named OFF error",
          "[wp360][physics][matrix]") {
    const auto document = AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"name":"PhysicsProbe","components":[{"name":"collider","shape":"sphere","radius":0.375}] }]}}})json",
        SceneRevision{9});
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{9});
    const auto *scene = resolved.findScene("main");
    REQUIRE(scene != nullptr);
    REQUIRE(scene->objects.front().components.front().effective_json.at(
                "radius") == 0.375f);
#if PELICAN_WITH_PHYSICS
    REQUIRE_NOTHROW(internal::validateResolvedSceneFeatureBindings(*scene));
    std::cout << "WP360_PHYSICS_MATRIX feature=PELICAN_WITH_PHYSICS state=ON"
                 " collider_radius=0.375\n";
#else
    try {
        internal::validateResolvedSceneFeatureBindings(*scene);
        FAIL("physics OFF unexpectedly accepted a resolved collider");
    } catch (const BuildFeatureDisabledError &error) {
        REQUIRE(error.feature() == "PELICAN_WITH_PHYSICS");
        REQUIRE(std::string{error.what()}.find("scene contains collider components") !=
                std::string::npos);
        std::cout << "WP360_PHYSICS_MATRIX feature=PELICAN_WITH_PHYSICS state=OFF"
                     " error='" << error.what() << "'\n";
    }
#endif
}

TEST_CASE("WP360 preview matrix reads resolver-completed values in every UI configuration",
          "[wp360][preview][matrix]") {
    const auto document = AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"name":"PreviewProbe","components":[{"name":"camera","type":"perspective","yfov":0.731}]}]}}})json",
        SceneRevision{10});
    const auto prepared = prepareEditorPreviewProjection(
        document, Json::array(),
        ResolvedSceneDefaults{
            CameraProjectionSpec{.kind = CameraProjectionKind::Perspective,
                                 .yfov = 0.5f,
                                 .znear = 0.125f,
                                 .zfar = 360.0f},
            CameraSpritePolicySpec{}});
    const EditorPreviewEvaluationContext context{prepared};
    const auto result = context.evaluate(Json::array({
        {{"kind", "component"}, {"object_id", 1},
         {"component_slot", "camera"}},
    }));
    REQUIRE(result.at(0).at("data").at("yfov") == 0.731f);
    REQUIRE(result.at(0).at("data").at("znear") == 0.125f);
    REQUIRE(result.at(0).at("data").at("zfar") == 360.0f);
    STATIC_REQUIRE(PELICAN_WP360_SKIP_DEVSTUDIO == 0 ||
                   PELICAN_WP360_SKIP_DEVSTUDIO == 1);
    std::cout << "WP360_PREVIEW_MATRIX yfov="
              << result.at(0).at("data").at("yfov")
              << " znear=" << result.at(0).at("data").at("znear")
              << " zfar=" << result.at(0).at("data").at("zfar")
              << " imgui=" << PELICAN_WITH_IMGUI
              << " skip_devstudio=" << PELICAN_WP360_SKIP_DEVSTUDIO << '\n';
}

TEST_CASE("WP360a case 1 accepts a target-less orbit through codec and Camera",
          "[wp360a][harm-1]") {
    ensureWp360aLogger();
    Wp360aSandbox sandbox{"orbit"};
    const auto scene = Json::parse(R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[
        {"name":"OrbitNoTarget","components":[
          {"name":"camera","controller":{"type":"orbit","distance":4.0}}
        ]}
      ]}}
    })json");
    FastModuleContainer modules;
    setupWp360aProject(sandbox, wp360aProjectJson(), scene);
    auto &camera = GET_MODULE(Camera);
    const auto *controller = camera.sceneCameraController("OrbitNoTarget");
    REQUIRE(controller != nullptr);
    REQUIRE(controller->target.empty());
    REQUIRE(controller->distance == 4.0f);
    std::cout << "WP360A_FIXED_CASE1 accepted target=<empty> distance="
              << controller->distance << '\n';
}

TEST_CASE("WP360a case 2 preserves nondefault project up only when rotation is omitted",
          "[wp360a][harm-2]") {
    ensureWp360aLogger();
    Wp360aSandbox sandbox{"up"};
    const auto scene = Json::parse(R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[
        {"name":"NoRotation","components":[
          {"name":"transform","pos":[1,2,3]},{"name":"camera"}
        ]},
        {"name":"IdentityRotation","components":[
          {"name":"transform","pos":[4,5,6],"rotation":[0,0,0,1]},
          {"name":"camera"}
        ]}
      ]}}
    })json");
    FastModuleContainer modules;
    setupWp360aProject(sandbox, wp360aProjectJson(-1.0f), scene);
    auto &camera = GET_MODULE(Camera);
    camera.setActiveCamera("NoRotation");
    REQUIRE(camera.getPos() == glm::vec3{1, 2, 3});
    REQUIRE(camera.getUp() == glm::vec3{0, -1, 0});
    const auto omitted_up = camera.getUp();
    camera.setActiveCamera("IdentityRotation");
    REQUIRE(camera.getUp() == glm::vec3{0, 1, 0});
    std::cout << "WP360A_FIXED_CASE2 omitted_up=" << omitted_up.x << ','
              << omitted_up.y << ',' << omitted_up.z
              << " explicit_identity_up=" << camera.getUp().x << ','
              << camera.getUp().y << ',' << camera.getUp().z << '\n';
}

TEST_CASE("WP360a case 3 rejects a string camera override at the merge seam",
          "[wp360a][harm-3]") {
    const auto document = AuthoringSceneDocument::load(R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"main":{"objects":[
        {"name":"Camera","components":[
          {"name":"camera","type":"perspective","yfov":0.8,"znear":0.1,"zfar":100}
        ]}
      ]}}
    })json", SceneRevision{1});
    const auto overrides = Json::array({
        {{"op", "set_component_value"}, {"object_id", 1},
         {"component_slot", "camera"}, {"field_path", "/yfov"},
         {"value", "not-a-number"}},
    });
    try {
        (void)prepareEditorPreviewProjection(document, overrides);
        FAIL("preview unexpectedly accepted invalid yfov");
    } catch (const EditorPreviewProjectionError &error) {
        REQUIRE(error.code() ==
                EditorPreviewProjectionErrorCode::schema_violation);
        REQUIRE(error.field() == "overrides/0/field_path");
        std::cout << "WP360A_FIXED_CASE3 code="
                  << editorPreviewProjectionErrorCodeName(error.code())
                  << " field=" << error.field() << " message="
                  << error.what() << '\n';
    }
}

TEST_CASE("WP360a case 4 reserves only exact generated markers",
          "[wp360a][harm-4]") {
    const auto accepted = AuthoringSceneDocument::load(R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"main":{"objects":[{"components":[
        {"name":"external_false","generated":false},
        {"name":"external_null","generated":null},
        {"name":"origin_false","origin":false},
        {"name":"origin_null","origin":null},
        {"name":"origin_external","origin":"external"}
      ]}]}}
    })json", SceneRevision{1});
    const auto resolved = ResolvedSceneResolver::resolve(
        accepted, SceneResolverGeneration{1});
    REQUIRE(resolved.findScene("main")->objects.front().components.size() ==
            5);

    const auto require_reserved = [](std::string_view marker) {
        const auto document = AuthoringSceneDocument::load(
            std::string{R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"components":[{"name":"external_probe",)json"} +
                std::string{marker} +
                R"json(}] }]}}})json",
            SceneRevision{1});
        REQUIRE_THROWS_AS(ResolvedSceneResolver::resolve(
                              document, SceneResolverGeneration{1}),
                          ResolvedSceneError);
    };
    require_reserved(R"json("generated":true)json");
    require_reserved(R"json("origin":"generated")json");
    std::cout << "WP360A_FIXED_CASE4 accepted=false,null,external"
                 " rejected=generated:true,origin:generated\n";
}

TEST_CASE("WP360a case 5 compares znear with the resolved project zfar once",
          "[wp360a][harm-5]") {
    ensureWp360aLogger();
    Wp360aSandbox sandbox{"camera-default"};
    const auto scene = Json::parse(R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[
        {"name":"LongRange","components":[
          {"name":"camera","type":"perspective","yfov":0.5,"znear":5000}
        ]}
      ]}}
    })json");
    FastModuleContainer modules;
    setupWp360aProject(sandbox, wp360aProjectJson(1.0f, 10000.0f), scene);
    auto &camera = GET_MODULE(Camera);
    camera.setActiveCamera("LongRange");
    const auto projection = camera.getProjectionSpec();
    REQUIRE(projection.znear == 5000.0f);
    REQUIRE(projection.zfar == 10000.0f);
    std::cout << "WP360A_FIXED_CASE5 accepted znear=" << projection.znear
              << " zfar=" << projection.zfar << '\n';
}

TEST_CASE("WP360a case 6 stores inactive behavior errors and names active bind failures",
          "[wp360a][harm-6]") {
    ensureWp360aLogger();
    Wp360aSandbox sandbox{"inactive"};
    const auto scene = Json::parse(R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{
        "active":{"objects":[]},
        "inactive":{"objects":[
          {"name":"InvalidInactive","components":[
            {"name":"behavior","type":"wp360a_inactive_behavior","params":{"label":7}}
          ]}
        ]}
      }
    })json");
    FastModuleContainer modules;
    setupWp360aProject(
        sandbox, wp360aProjectJson(1.0f, 1000.0f, "active"), scene);
    GET_MODULE(ECSPredefinedRegistration).reg();
    REQUIRE_NOTHROW(GET_MODULE(SceneLoader).load(SceneId{"active"}));
    const auto &resolved = GET_MODULE(ProjectBasicConfig).resolvedScene();
    const auto *inactive = resolved.findScene("inactive");
    REQUIRE(inactive != nullptr);
    const auto &component =
        inactive->objects.front().components.front();
    REQUIRE(component.runtime_resolution_error.has_value());
    REQUIRE_THROWS_WITH(
        prepareResolvedSceneBehaviorAttachments(
            inactive->objects, BehaviorRegistryAvailability::active),
        Catch::Matchers::ContainsSubstring("behavior_resolution_error") &&
            Catch::Matchers::ContainsSubstring("InvalidInactive"));
    std::cout << "WP360A_FIXED_CASE6 active=loaded inactive_error=stored"
                 " bind_error=behavior_resolution_error\n";
}

TEST_CASE("WP360a missing resolved provider is a named error with no fallback",
          "[wp360a][fallback]") {
    const auto document = AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[]}}})json",
        SceneRevision{1});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    try {
        (void)service.sceneTree();
        FAIL("missing resolved provider unexpectedly fell back");
    } catch (const EditorCommandError &error) {
        REQUIRE(error.code() ==
                EditorCommandErrorCode::ResolvedSceneProviderUnavailable);
        REQUIRE(std::string{editorCommandErrorCodeName(error.code())} ==
                "resolved_scene_provider_unavailable");
        std::cout << "WP360A_FALLBACK code="
                  << editorCommandErrorCodeName(error.code())
                  << " message=" << error.what() << '\n';
    }
}

#if PELICAN_WITH_RPC
TEST_CASE("WP360 current resolved query is byte exact with named parent RpcServer run",
          "[wp360][rpc][wire][parent]") {
    const auto expected = readBytes(PELICAN_WP360_RPC_FIXTURE);
    const auto actual = currentWireBytes();
    REQUIRE(actual == expected);
    REQUIRE_FALSE(actual.empty());
    REQUIRE(actual.back() == '\n');
    const auto digest = picosha2::hash256_hex_string(actual.begin(), actual.end());
    std::cout << "WP360_PARENT_RPC named_sha="
              << "447104d0b7ac6a15198dcbe03d84ef113b81a588"
              << " bytes=" << actual.size() << " sha256=" << digest
              << '\n';
}
#else
TEST_CASE("WP360 RPC OFF is named feature-disabled and creates no server",
          "[wp360][rpc][feature-off]") {
    std::istringstream input;
    std::ostringstream output;
    try {
        runEngineRpcServer(input, output);
        FAIL("RPC OFF unexpectedly generated a server");
    } catch (const BuildFeatureDisabledError &error) {
        REQUIRE(error.feature() == "PELICAN_WITH_RPC");
        REQUIRE(output.str().empty());
        std::cout << "WP360_RPC_MATRIX feature=PELICAN_WITH_RPC state=OFF"
                     " server_created=false error='" << error.what() << "'\n";
    }
}
#endif

} // namespace Pelican
