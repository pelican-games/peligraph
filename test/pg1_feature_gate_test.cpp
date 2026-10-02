#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/loader/sceneload.hpp"
#include "../src/core/log.hpp"
#include "scene_load_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

// PG-1 acceptance 1, the scene-load half. The same translation unit runs in
// every build: with the module the scene loads, without it the scene is
// refused at prepare under `<requirement>.module_unavailable@1` and the scene
// that was live stays live. run_build_units_smoke.cmake runs the [pg1] tag in
// the physics-OFF and audio-OFF builds.

namespace Pelican {
namespace {
using Json = nlohmann::json;

// Two scenes over the same object names; the only difference is the collider.
constexpr auto sceneDocument = R"({"schema":"pelican.scene","version":2,"scenes":{
  "active":{"objects":[{"name":"Prop","components":[
    {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}]},
  "physical":{"objects":[{"name":"Prop","components":[
    {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]},
    {"name":"Floor","components":[
    {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
    {"name":"collider","shape":"box","half_extents":[3.0,0.1,0.5]}]}]}}})";

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;
    Sandbox() {
        base = std::filesystem::temp_directory_path() /
               ("pelican_pg1_gate_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base / "project";
        std::filesystem::create_directories(root / "passes");
        std::ofstream{root / "scene.json", std::ios::binary} << sceneDocument;
        std::ofstream{root / "passes/main_rendering_config.json", std::ios::binary}
            << R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})";
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

Json projectJson() {
    return Json{{"schema", "pelican.project"}, {"version", 1}, {"name", "pg1-gate"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"window_size", {{"width", 100}, {"height", 100}}},
            {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 1000.0}}},
            {"default_scene_id", "active"}, {"scene_data_json", "scene.json"}}}};
}

} // namespace

TEST_CASE("PG-1 scene load refuses a collider by the physics module's name",
          "[pg1][scene][physics]") {
    setupLogger();
    const Sandbox sandbox;
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &loader = GET_MODULE(SceneLoader);

    // The scene without the collider loads, so the result below is about the
    // component and not about this build being unable to load scenes at all.
    REQUIRE_NOTHROW(test_support::loadScene(loader, SceneId{"active"}));
    REQUIRE(loader.currentScene() == SceneId{"active"});

    const auto result = loader.load(SceneLoadRequest::borrowed(
        "physical", GET_MODULE(ProjectBasicConfig).sceneProjectionState()));
#if PELICAN_WITH_PHYSICS
    REQUIRE(std::get_if<PreCommitFailure>(&result) == nullptr);
    REQUIRE(loader.currentScene() == SceneId{"physical"});
    REQUIRE(loader.objectId("Floor"));
#else
    const auto *failure = std::get_if<PreCommitFailure>(&result);
    REQUIRE(failure != nullptr);
    // prepare, not commit: nothing was published, so the load rolls back whole.
    REQUIRE(failure->phase == SceneLoadPhase::prepare);
    REQUIRE_THAT(failure->error,
                 Catch::Matchers::StartsWith("physics.module_unavailable@1:"));
    REQUIRE_THAT(failure->error, Catch::Matchers::ContainsSubstring("Floor"));
    REQUIRE(loader.currentScene() == SceneId{"active"});
    REQUIRE(loader.objectId("Prop"));
    REQUIRE_FALSE(loader.objectId("Floor"));
    std::cout << "PG1_SCENE_LOAD state=OFF error='" << failure->error << "'\n";
#endif
}

// The gate itself, over resolved objects. Scene load and the editor's
// add_component / spawn both ask this one function; which codec needs which
// module is a column of the codec table, not a list in the gate.
TEST_CASE("PG-1 codec rows name their module and the resolved gate answers by it",
          "[pg1][component-codec]") {
    // The two codecs whose meaning at runtime is a module's. Every other row
    // is the engine core's.
    for (const auto &codec : componentCodecRows()) {
        const std::string name{codec.name};
        const std::string required{codec.required_feature};
        CAPTURE(name);
        if (name == "collider") {
            REQUIRE(required == "physics");
        } else if (name == "animation") {
            REQUIRE(required == "animation");
        } else {
            REQUIRE(required.empty());
        }
    }
    // The codec stays registered either way: an OFF build still reads and
    // writes the component.
    REQUIRE(findComponentCodec("collider") != nullptr);
    REQUIRE(findComponentCodec("animation") != nullptr);
    REQUIRE(std::string{componentCodecUnavailableCode(requireComponentCodec("collider"))} ==
            (PELICAN_WITH_PHYSICS ? "" : "physics.module_unavailable@1"));
    REQUIRE(std::string{componentCodecUnavailableCode(requireComponentCodec("animation"))} ==
            (PELICAN_WITH_ANIMATION ? "" : "animation.module_unavailable@1"));
    REQUIRE(componentCodecUnavailableCode(requireComponentCodec("transform")).empty());

    const auto document = AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[
          {"name":"Plain","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]},
          {"name":"Solid","components":[{"name":"collider","shape":"sphere","radius":0.5}]},
          {"name":"Moving","components":[{"name":"animation","clip":"hero.gltf#animation/Run"}]}]}}})json",
        SceneRevision{3});
    const auto resolved = ResolvedSceneResolver::resolve(document, SceneResolverGeneration{3});
    const auto *scene = resolved.findScene("main");
    REQUIRE(scene != nullptr);
    REQUIRE(scene->objects.size() == 3);
    REQUIRE_FALSE(resolvedObjectFeatureRefusal(scene->objects[0]));

    const auto solid = resolvedObjectFeatureRefusal(scene->objects[1]);
#if PELICAN_WITH_PHYSICS
    REQUIRE_FALSE(solid);
#else
    REQUIRE(solid);
    REQUIRE(solid->code == "physics.module_unavailable@1");
    REQUIRE(solid->requirement == "physics");
    REQUIRE(solid->option == "PELICAN_WITH_PHYSICS");
    REQUIRE(solid->component == "collider");
    REQUIRE(solid->object == "Solid");
    REQUIRE_THAT(solid->message, Catch::Matchers::StartsWith("physics.module_unavailable@1:"));
#endif
    const auto moving = resolvedObjectFeatureRefusal(scene->objects[2]);
#if PELICAN_WITH_ANIMATION
    REQUIRE_FALSE(moving);
#else
    REQUIRE(moving);
    REQUIRE(moving->code == "animation.module_unavailable@1");
#endif
}

} // namespace Pelican
