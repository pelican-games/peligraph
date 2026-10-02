#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/loader/sceneload.hpp"
#include "../src/core/communication/animationstatus.hpp"
#include "../src/core/log.hpp"
#include "scene_load_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

// This translation unit exists only in a PELICAN_WITH_ANIMATION=OFF build.
// AG-1b acceptance 3: a scene that carries an animation component is refused by
// name at scene prepare -- the phase whose failure the load contract rolls back
// whole -- so the scene that was already live stays live. The codec is not
// touched: an OFF build still decodes and encodes the component, which is what
// lets its tools read and save the same project.
#if PELICAN_WITH_ANIMATION
#error "animation_off_scene_test belongs to a PELICAN_WITH_ANIMATION=OFF build"
#endif

namespace Pelican {
namespace {
using Json = nlohmann::json;

// Two scenes over the same objects; the only difference is the component.
constexpr auto sceneDocument = R"({"schema":"pelican.scene","version":2,"scenes":{
  "active":{"objects":[{"name":"Prop","components":[
    {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}]},
  "animated":{"objects":[{"name":"Prop","components":[
    {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]},
    {"name":"Hero","components":[
    {"name":"animation","clip":"hero.gltf#animation/Run"}]}]}}})";

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;
    Sandbox() {
        base = std::filesystem::temp_directory_path() /
               ("pelican_animation_off_" +
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
    return Json{{"schema", "pelican.project"}, {"version", 1}, {"name", "animation-off"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"window_size", {{"width", 100}, {"height", 100}}},
            {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 1000.0}}},
            {"default_scene_id", "active"}, {"scene_data_json", "scene.json"}}}};
}

} // namespace

TEST_CASE("a build without the animation module refuses a scene that animates",
          "[animation-off][scene]") {
    setupLogger();
    const Sandbox sandbox;
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &loader = GET_MODULE(SceneLoader);

    // The scene without the component loads, so the refusal below is about the
    // component and not about this build being unable to load scenes at all.
    // Decoding is untouched either way: both scenes came through the same codec.
    REQUIRE_NOTHROW(test_support::loadScene(loader, SceneId{"active"}));
    REQUIRE(loader.currentScene() == SceneId{"active"});

    const auto result = loader.load(SceneLoadRequest::borrowed(
        "animated", GET_MODULE(ProjectBasicConfig).sceneProjectionState()));
    const auto *failure = std::get_if<PreCommitFailure>(&result);
    REQUIRE(failure != nullptr);
    // prepare, not commit: no participant published anything, so the load is
    // rolled back whole.
    REQUIRE(failure->phase == SceneLoadPhase::prepare);
    REQUIRE_THAT(failure->error,
                 Catch::Matchers::ContainsSubstring("animation.module_unavailable@1"));
    // The refusal names the object, so whoever has to remove it or rebuild with
    // the module knows which one it is.
    REQUIRE_THAT(failure->error, Catch::Matchers::ContainsSubstring("Hero"));
    // The rollback: the scene that was live is still live and still complete.
    REQUIRE(loader.currentScene() == SceneId{"active"});
    REQUIRE(loader.objectId("Prop"));

    // And it stays refused: this is a property of the document, not a one-off.
    const auto again = loader.load(SceneLoadRequest::borrowed(
        "animated", GET_MODULE(ProjectBasicConfig).sceneProjectionState()));
    REQUIRE(std::get_if<PreCommitFailure>(&again) != nullptr);
    REQUIRE(loader.currentScene() == SceneId{"active"});
}

// AG-3a acceptance 1's control. The observation side of the same gate: the
// section get_status carries says the module is absent, rather than reporting
// an empty sink list -- which a tool would read as "the module is here and
// nothing is animating", the one answer this build can never truthfully give.
TEST_CASE("the animation status section says the module is absent",
          "[animation-off][status]") {
    const auto status = internal::animationStatusJson();
    REQUIRE(status.at("available") == false);
    // Nothing else: no sink list to mistake for an empty one.
    REQUIRE(status.size() == 1);
}

} // namespace Pelican
