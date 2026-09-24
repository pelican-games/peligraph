#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/ecs/predefined/transform.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "scene_load_test_support.hpp"

#if defined(PELICAN_GP1_OBJECT_REF_FIXTURE)
#include "fixtures/gp1_object_ref/api.hpp"
#include <Windows.h>
#endif

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// GP-1: ObjectRef, the world transform read, and a localtransform on every
// scene object with a transform. Scenes are loaded through the production
// SceneLoader transaction and advanced with the production ECS update.

namespace Pelican {
namespace {
using Json = nlohmann::json;

Json transform(double x, double y, double z,
               std::array<double, 4> rotation = {0.0, 0.0, 0.0, 1.0},
               std::array<double, 3> scale = {1.0, 1.0, 1.0}) {
    return Json{{"name", "transform"},
                {"pos", {x, y, z}},
                {"rotation", rotation},
                {"scale", scale}};
}

constexpr double half_sqrt2 = 0.70710678118654752;

Json sceneDocument() {
    const std::array<double, 4> y90{0.0, half_sqrt2, 0.0, half_sqrt2};
    const std::array<double, 4> z90{0.0, 0.0, half_sqrt2, half_sqrt2};
    const Json light{{"name", "light"},
                     {"type", "directional"},
                     {"direction", {0.0, -1.0, 0.0}},
                     {"intensity", 1.0},
                     {"color", {1.0, 1.0, 1.0}}};
    return Json{
        {"schema", "pelican.scene"},
        {"version", 1},
        {"scenes",
         {{"main",
           {{"objects",
             Json::array({
                 {{"name", "Parent"},
                  {"components", Json::array({transform(1, 2, 3, y90, {2, 2, 2})})}},
                 {{"name", "Child"},
                  {"parent", "Parent"},
                  {"components",
                   Json::array({transform(1, 0, 0, z90, {0.5, 1.0, 1.5})})}},
                 {{"name", "Prop"}, {"components", Json::array({transform(10, 0, 0)})}},
                 {{"name", "Sun"}, {"components", Json::array({light})}},
                 {{"name", "Brain"},
                  {"components",
                   Json::array({{{"name", "behavior"},
                                 {"type", "gp1_object_ref_behavior"},
                                 {"params", {{"target", "Child"}}}}})}},
             })}}},
          // No object in this scene has a parent or a child.
          {"flat",
           {{"objects",
             Json::array({
                 {{"name", "Crate"}, {"components", Json::array({transform(1, 0, 0)})}},
                 {{"name", "Lamp"}, {"components", Json::array({light})}},
             })}}},
          {"family",
           {{"objects",
             Json::array({
                 {{"name", "Base"},
                  {"components", Json::array({transform(0, 1, 0, y90, {2, 2, 2})})}},
                 {{"name", "Arm"},
                  {"parent", "Base"},
                  {"components", Json::array({transform(1, 0, 0)})}},
             })}}}}},
    };
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;
    Sandbox() {
        base = std::filesystem::temp_directory_path() /
               ("pelican_gp1_object_ref_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base / "project";
        std::filesystem::create_directories(root / "passes");
        std::ofstream{root / "scene.json", std::ios::binary} << sceneDocument().dump(2);
        std::ofstream{root / "passes/main_rendering_config.json", std::ios::binary} << "{}";
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

void configureProject(const Sandbox &sandbox, std::string_view default_scene) {
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(
        Json{{"schema", "pelican.project"},
             {"version", 1},
             {"name", "gp1-object-ref"},
             {"engine_min_version", "0.1.0"},
             {"basic_config",
              {{"window_size", {{"width", 100}, {"height", 100}}},
               {"camera",
                {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 1000.0}, {"up", {0.0, 1.0, 0.0}}}},
               {"default_scene_id", std::string{default_scene}},
               {"scene_data_json", "scene.json"}}}}
            .dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
}

void frame() { GET_MODULE(ECSCore).update(); }

void requireWorld(const WorldTransform &actual, glm::vec3 pos, glm::quat rotation,
                  glm::vec3 scale) {
    CHECK(actual.pos.x == Catch::Approx(pos.x).margin(1e-5));
    CHECK(actual.pos.y == Catch::Approx(pos.y).margin(1e-5));
    CHECK(actual.pos.z == Catch::Approx(pos.z).margin(1e-5));
    // q and -q are the same rotation.
    const float sign = glm::dot(glm::quat{actual.rotation.w, actual.rotation.x,
                                          actual.rotation.y, actual.rotation.z},
                                rotation) < 0.0f
                           ? -1.0f
                           : 1.0f;
    CHECK(actual.rotation.x == Catch::Approx(sign * rotation.x).margin(1e-5));
    CHECK(actual.rotation.y == Catch::Approx(sign * rotation.y).margin(1e-5));
    CHECK(actual.rotation.z == Catch::Approx(sign * rotation.z).margin(1e-5));
    CHECK(actual.rotation.w == Catch::Approx(sign * rotation.w).margin(1e-5));
    CHECK(actual.scale.x == Catch::Approx(scale.x).margin(1e-5));
    CHECK(actual.scale.y == Catch::Approx(scale.y).margin(1e-5));
    CHECK(actual.scale.z == Catch::Approx(scale.z).margin(1e-5));
}

WorldTransform requireWorldOf(std::string_view name) {
    const GameContext ctx;
    const auto ref = ctx.objects().resolve(name);
    INFO("resolve " << name << ": " << ref.errorName());
    REQUIRE(ref.ok());
    const auto world = ctx.objects().worldTransform(ref.value());
    INFO("worldTransform " << name << ": " << world.errorName());
    REQUIRE(world.ok());
    return world.value();
}

LocalTransformComponent localAt(float x, float y, float z) {
    return LocalTransformComponent{.scale = {1.0f, 1.0f, 1.0f},
                                   .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
                                   .pos = {x, y, z},
                                   .parent = invalidEntityId};
}

} // namespace

// Acceptance 2. A scene with no parent/child link anywhere used to load
// without any localtransform, so setLocalTransform had nothing to write and
// returned false.
TEST_CASE("GP-1 a scene without parents gives every transform a localtransform",
          "[gp1][localtransform]") {
    setupLogger();
    const Sandbox sandbox;
    FastModuleContainer modules;
    configureProject(sandbox, "flat");
    auto &loader = GET_MODULE(SceneLoader);
    test_support::loadScene(loader, SceneId{"flat"});

    const GameContext ctx;
    const auto ref = ctx.objects().resolve("Crate");
    REQUIRE(ref.ok());
    const auto crate = ctx.objects().materialize(ref.value());
    REQUIRE(crate.ok());

    // The injected local holds the authored transform, so the transform
    // update recomputes the same world rather than a default one.
    frame();
    requireWorld(requireWorldOf("Crate"), {1, 0, 0}, glm::quat{1, 0, 0, 0}, {1, 1, 1});

    REQUIRE(ctx.setLocalTransform(crate.value(), localAt(5.0f, 0.0f, 0.0f)));
    frame();
    requireWorld(requireWorldOf("Crate"), {5, 0, 0}, glm::quat{1, 0, 0, 0}, {1, 1, 1});
    REQUIRE(ctx.localTransform(crate.value()).pos.x == Catch::Approx(5.0));

    // The runtime transform write (update_transforms, camera controllers)
    // writes the world; it has to carry the local with it, or the next
    // transform update puts the object back where the local says.
    loader.applyObjectTransform(
        "Crate", SceneObjectTransform{.pos = {8.0f, 0.0f, 0.0f},
                                      .rotation = glm::quat{1, 0, 0, 0},
                                      .scale = {1.0f, 1.0f, 1.0f}});
    frame();
    frame();
    requireWorld(requireWorldOf("Crate"), {8, 0, 0}, glm::quat{1, 0, 0, 0}, {1, 1, 1});
}

TEST_CASE("GP-1 a runtime world write on a parented object survives the transform update",
          "[gp1][localtransform]") {
    setupLogger();
    const Sandbox sandbox;
    FastModuleContainer modules;
    configureProject(sandbox, "family");
    auto &loader = GET_MODULE(SceneLoader);
    test_support::loadScene(loader, SceneId{"family"});
    frame();

    const glm::quat y90{static_cast<float>(half_sqrt2), 0.0f,
                        static_cast<float>(half_sqrt2), 0.0f};
    // Arm = Base * (1, 0, 0): (0, 1, 0) + y90 * (2, 0, 0) = (0, 1, -2).
    requireWorld(requireWorldOf("Arm"), {0, 1, -2}, y90, {2, 2, 2});

    loader.applyObjectTransform(
        "Arm", SceneObjectTransform{.pos = {4.0f, 4.0f, 4.0f},
                                    .rotation = glm::quat{1, 0, 0, 0},
                                    .scale = {1.0f, 1.0f, 1.0f}});
    frame();
    frame();
    requireWorld(requireWorldOf("Arm"), {4, 4, 4}, glm::quat{1, 0, 0, 0}, {1, 1, 1});
}



#if defined(PELICAN_GP1_OBJECT_REF_FIXTURE)
namespace {

struct FixtureExports {
    PelicanGp1ResolveFn resolve = nullptr;
    PelicanGp1RefFn materialize = nullptr;
    PelicanGp1RefFn world = nullptr;
    PelicanGp1InitProbeFn init_probe = nullptr;
};

FixtureExports fixtureExports() {
    const auto status = configuredGameLogicStatus();
    REQUIRE(status.loaded);
    auto module = GetModuleHandleW(status.loaded_copy.c_str());
    if (module == nullptr) module = GetModuleHandleW(status.loaded_copy.filename().c_str());
    REQUIRE(module != nullptr);
    FixtureExports result{
        .resolve = reinterpret_cast<PelicanGp1ResolveFn>(
            GetProcAddress(module, "pelican_gp1_fixture_resolve")),
        .materialize = reinterpret_cast<PelicanGp1RefFn>(
            GetProcAddress(module, "pelican_gp1_fixture_materialize")),
        .world = reinterpret_cast<PelicanGp1RefFn>(
            GetProcAddress(module, "pelican_gp1_fixture_world")),
        .init_probe = reinterpret_cast<PelicanGp1InitProbeFn>(
            GetProcAddress(module, "pelican_gp1_fixture_init_probe")),
    };
    REQUIRE(result.resolve != nullptr);
    REQUIRE(result.materialize != nullptr);
    REQUIRE(result.world != nullptr);
    REQUIRE(result.init_probe != nullptr);
    return result;
}

class GameLogicScope {
  public:
    GameLogicScope() {
        GET_MODULE(EngineLaunchConfig).game_logic_dll =
            std::filesystem::path{PELICAN_GP1_OBJECT_REF_FIXTURE};
        if (!initializeConfiguredGameLogic()) {
            FAIL("GP-1 fixture failed to load: " << configuredGameLogicStatus().last_error);
        }
    }
    ~GameLogicScope() {
        GET_MODULE(BehaviorAttachmentArena).deactivateAll();
        shutdownConfiguredGameLogic();
    }
    GameLogicScope(const GameLogicScope &) = delete;
    GameLogicScope &operator=(const GameLogicScope &) = delete;
};

std::string errorOf(const Gp1Fixture::Probe &probe) { return probe.error; }

} // namespace

// Acceptance 1. Every ctx.objects() call below is made inside the game DLL
// against the SDK the host exports.
TEST_CASE("GP-1 a game DLL resolves objects by name and reads world transforms",
          "[gp1][object-ref][game-logic]") {
    setupLogger();
    const Sandbox sandbox;
    FastModuleContainer modules;
    configureProject(sandbox, "main");
    const GameLogicScope game_logic;
    const auto dll = fixtureExports();
    auto &loader = GET_MODULE(SceneLoader);
    test_support::loadScene(loader, SceneId{"main"});
    frame();

    const glm::quat y90{static_cast<float>(half_sqrt2), 0.0f,
                        static_cast<float>(half_sqrt2), 0.0f};
    const glm::quat z90{static_cast<float>(half_sqrt2), 0.0f, 0.0f,
                        static_cast<float>(half_sqrt2)};
    // Child = Parent * local: (1, 2, 3) + y90 * (2 * (1, 0, 0)) = (1, 2, 1),
    // rotation y90 * z90, scale 2 * (0.5, 1, 1.5).
    const glm::vec3 child_pos{1.0f, 2.0f, 1.0f};
    const glm::quat child_rotation = glm::normalize(y90 * z90);
    const glm::vec3 child_scale{1.0f, 2.0f, 3.0f};

    Gp1Fixture::Probe child;
    dll.resolve("Child", &child);
    INFO("resolve Child: " << errorOf(child));
    REQUIRE(child.ok == 1U);
    Gp1Fixture::Probe child_world;
    dll.world(&child.ref, &child_world);
    INFO("worldTransform Child: " << errorOf(child_world));
    REQUIRE(child_world.ok == 1U);
    requireWorld(child_world.world, child_pos, child_rotation, child_scale);

    Gp1Fixture::Probe parent_world;
    Gp1Fixture::Probe parent;
    dll.resolve("Parent", &parent);
    REQUIRE(parent.ok == 1U);
    dll.world(&parent.ref, &parent_world);
    REQUIRE(parent_world.ok == 1U);
    requireWorld(parent_world.world, {1, 2, 3}, y90, {2, 2, 2});

    // The behavior resolved "Child" in onInit, while "main" was still the
    // load candidate. The ref it kept is the one the live scene issues now.
    Gp1Fixture::Probe init_resolved;
    Gp1Fixture::Probe init_world;
    REQUIRE(dll.init_probe(&init_resolved, &init_world));
    INFO("onInit resolve: " << errorOf(init_resolved)
                            << " world: " << errorOf(init_world));
    REQUIRE(init_resolved.ok == 1U);
    REQUIRE(init_resolved.ref == child.ref);
    REQUIRE(init_world.ok == 1U);
    requireWorld(init_world.world, child_pos, child_rotation, child_scale);

    // Not declared.
    Gp1Fixture::Probe nobody;
    dll.resolve("Nobody", &nobody);
    REQUIRE(nobody.ok == 0U);
    REQUIRE(errorOf(nobody) == "object.not_materialized@1");

    // materialize() hands out the same live object the loader bound. (A
    // scene document refuses two objects of one name, so the first-declared
    // rule of resolve() has no authored case to observe here.)
    Gp1Fixture::Probe prop;
    dll.resolve("Prop", &prop);
    REQUIRE(prop.ok == 1U);
    Gp1Fixture::Probe prop_world;
    dll.world(&prop.ref, &prop_world);
    REQUIRE(prop_world.ok == 1U);
    CHECK(prop_world.world.pos.x == Catch::Approx(10.0));
    Gp1Fixture::Probe prop_object;
    dll.materialize(&prop.ref, &prop_object);
    REQUIRE(prop_object.ok == 1U);
    REQUIRE(GameObjectId{prop_object.object_index, prop_object.object_generation} ==
            loader.objectId("Prop"));

    // Declared, but light-only: nothing at runtime to materialize.
    Gp1Fixture::Probe sun;
    dll.resolve("Sun", &sun);
    REQUIRE(sun.ok == 1U);
    Gp1Fixture::Probe sun_object;
    dll.materialize(&sun.ref, &sun_object);
    REQUIRE(sun_object.ok == 0U);
    REQUIRE(errorOf(sun_object) == "object.not_materialized@1");

    // Live, but without a transform to read.
    Gp1Fixture::Probe brain;
    dll.resolve("Brain", &brain);
    REQUIRE(brain.ok == 1U);
    Gp1Fixture::Probe brain_object;
    dll.materialize(&brain.ref, &brain_object);
    REQUIRE(brain_object.ok == 1U);
    Gp1Fixture::Probe brain_world;
    dll.world(&brain.ref, &brain_world);
    REQUIRE(brain_world.ok == 0U);
    REQUIRE(errorOf(brain_world) == "object.missing_capability@1");

    // Reload the same scene. The old ref names the same authored object id,
    // but a different scene run: it is refused, not read against the new one.
    test_support::loadScene(loader, SceneId{"main"});
    frame();
    Gp1Fixture::Probe stale_object;
    dll.materialize(&child.ref, &stale_object);
    REQUIRE(stale_object.ok == 0U);
    REQUIRE(errorOf(stale_object) == "object.stale_authority@1");
    Gp1Fixture::Probe stale_world;
    dll.world(&child.ref, &stale_world);
    REQUIRE(stale_world.ok == 0U);
    REQUIRE(errorOf(stale_world) == "object.stale_authority@1");

    Gp1Fixture::Probe fresh;
    dll.resolve("Child", &fresh);
    REQUIRE(fresh.ok == 1U);
    REQUIRE(fresh.ref.objectId() == child.ref.objectId());
    REQUIRE_FALSE(fresh.ref.authority() == child.ref.authority());
    Gp1Fixture::Probe fresh_world;
    dll.world(&fresh.ref, &fresh_world);
    REQUIRE(fresh_world.ok == 1U);
    requireWorld(fresh_world.world, child_pos, child_rotation, child_scale);

    // A ref nobody resolved names no scene run.
    const ObjectRef empty;
    Gp1Fixture::Probe empty_world;
    dll.world(&empty, &empty_world);
    REQUIRE(errorOf(empty_world) == "object.stale_authority@1");
}
#endif

} // namespace Pelican
