#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/runtimespawn.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "scene_load_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

// GP-2 acceptance 5: ctx.spawn asks the build-feature gate a scene load asks
// (PG-1's resolvedObjectFeatureRefusal), of the prefab as it expanded. The
// same translation unit runs in every build: with physics the collider prefab
// spawns, without it the spawn is refused at the call as
// physics.module_unavailable@1 and nothing is queued. A prefab without a
// collider spawns in both. run_build_units_smoke.cmake runs the [pg1] tag in
// the physics-OFF build.

namespace Pelican {
namespace {
using Json = nlohmann::json;

constexpr auto sceneDocument = R"({"schema":"pelican.scene","version":2,"scenes":{
  "main":{"objects":[{"name":"Prop","components":[
    {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}]}}})";

// Only the collider differs between the two prefabs.
constexpr auto solidPrefab = R"({"schema":"pelican.prefab","version":1,"name":"solid",
  "components":[{"name":"collider","shape":"sphere","radius":0.5}]})";
constexpr auto markPrefab = R"({"schema":"pelican.prefab","version":1,"name":"mark",
  "components":[{"name":"sprite_view","texture":"white","size":[1,1],"pivot":[0.5,0.5]}]})";

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;
    Sandbox() {
        base = std::filesystem::temp_directory_path() /
               ("pelican_gp2_gate_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base / "project";
        std::filesystem::create_directories(root / "passes");
        std::filesystem::create_directories(root / "prefabs");
        std::ofstream{root / "scene.json", std::ios::binary} << sceneDocument;
        std::ofstream{root / "prefabs/solid.json", std::ios::binary} << solidPrefab;
        std::ofstream{root / "prefabs/mark.json", std::ios::binary} << markPrefab;
        std::ofstream{root / "passes/main_rendering_config.json", std::ios::binary}
            << R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})";
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

Json projectJson() {
    return Json{{"schema", "pelican.project"}, {"version", 1}, {"name", "gp2-gate"},
        {"engine_min_version", "0.1.0"},
        {"prefabs", Json::array({{{"name", "solid"}, {"path", "prefabs/solid.json"}},
                                 {{"name", "mark"}, {"path", "prefabs/mark.json"}}})},
        {"basic_config", {{"window_size", {{"width", 100}, {"height", 100}}},
            {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 1000.0}, {"up", {0.0, 1.0, 0.0}}}},
            {"default_scene_id", "main"}, {"scene_data_json", "scene.json"}}}};
}

} // namespace

TEST_CASE("GP-2 a spawned prefab passes the build-feature gate a scene load asks",
          "[pg1][gp2][spawn][physics]") {
    setupLogger();
    const Sandbox sandbox;
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(projectJson().dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &loader = GET_MODULE(SceneLoader);
    test_support::loadScene(loader, SceneId{"main"});
    auto &spawner = GET_MODULE(RuntimeSpawner);
    spawner.applyBoundary();

    const GameContext ctx;
    // The control: a prefab without a module's component spawns in every build.
    const auto mark = ctx.spawn("mark", SpawnParams{});
    REQUIRE(spawner.stats().queued == 1U);

#if PELICAN_WITH_PHYSICS
    const auto solid = ctx.spawn("solid", SpawnParams{});
    REQUIRE(spawner.stats().queued == 2U);
    spawner.applyBoundary();
    REQUIRE(ctx.objects().materialize(solid).ok());
#else
    try {
        (void)ctx.spawn("solid", SpawnParams{});
        FAIL("a collider prefab spawned in a build without physics");
    } catch (const SpawnError &error) {
        REQUIRE(error.code() == "physics.module_unavailable@1");
        REQUIRE_THAT(std::string{error.what()},
                     Catch::Matchers::ContainsSubstring("collider"));
        std::cout << "GP2_SPAWN_GATE state=OFF code=" << error.code() << "\n";
    }
    // Refused at the call: nothing was queued for the boundary.
    REQUIRE(spawner.stats().queued == 1U);
    spawner.applyBoundary();
#endif
    REQUIRE(ctx.objects().materialize(mark).ok());
}

} // namespace Pelican
