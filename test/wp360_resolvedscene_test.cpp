#include "../src/core/build_features.hpp"
#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/loader/authoringsceneauthority.hpp"
#include "../src/core/loader/editorpreviewprojection.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/userpublic/behavior.hpp"

#include <catch2/catch_test_macros.hpp>

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
#ifndef PELICAN_WP360_SKIP_DEVSTUDIO
#define PELICAN_WP360_SKIP_DEVSTUDIO 0
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
    std::cout << "WP360_PREVIEW_MATRIX yfov="
              << result.at(0).at("data").at("yfov")
              << " znear=" << result.at(0).at("data").at("znear")
              << " zfar=" << result.at(0).at("data").at("zfar")
              << " imgui=" << PELICAN_WITH_IMGUI
              << " skip_devstudio=" << PELICAN_WP360_SKIP_DEVSTUDIO << '\n';
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
