#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/playmode.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/components/predefined.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/project/projecttemplate.hpp"
#include "fixtures/gp2_spawn/api.hpp"
#include "vulkan_test_support.hpp"

#include <Windows.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>

// RO-1. get_runtime_objects over the headless RPC, the way Studio's Outliner
// asks for the objects a running game spawned. The spawns are the GP-2
// fixture DLL's (fixtures/gp2_spawn), built against the SDK this test
// executable exports: some are made from the host through the DLL, outside
// every callback, and one by the DLL's spawner behavior from its onUpdate.

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::string readFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path, std::ios::binary} << text;
}

struct TempDir {
    std::filesystem::path root;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

// The blank template, with the probe prefab the spawns name, a spawner the
// host can ask to spawn from inside its onUpdate, and an authored object with
// a transform (the parent the test gives a spawned object, and the object
// whose get_components gives the runtime Transform's form).
std::filesystem::path makeProject(const std::filesystem::path &dir) {
    const auto root = dir / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    auto project = Json::parse(readFile(root / "project.json"));
    project["prefabs"] = Json::array({{{"name", "probe"}, {"path", "prefabs/probe.json"}}});
    writeFile(root / "project.json", project.dump(2));
    writeFile(root / "prefabs/probe.json",
              Json{{"schema", "pelican.prefab"},
                   {"version", 1},
                   {"name", "probe"},
                   {"parameters",
                    Json::array({{{"name", "tag"}, {"type", "string"}, {"default", "none"}}})},
                   {"components",
                    Json::array({{{"name", "behavior"},
                                  {"type", "gp2_probe"},
                                  {"params", {{"tag", {{"$param", "tag"}}}}}}})}}
                  .dump(2));

    auto scene = Json::parse(readFile(root / "scenes/main.scene.json"));
    auto &objects = scene.at("scenes").at("default_scene").at("objects");
    objects.push_back({{"name", "Spawner"},
                       {"components",
                        Json::array({{{"name", "behavior"},
                                      {"type", "gp2_spawner"},
                                      {"params", {{"label", "ro1"}}}}})}});
    objects.push_back({{"name", "Anchor"},
                       {"components",
                        Json::array({{{"name", "transform"},
                                      {"pos", {3.0, 0.0, 0.0}},
                                      {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                      {"scale", {1.0, 1.0, 1.0}}}})}});
    writeFile(root / "scenes/main.scene.json", scene.dump(2));
    return root;
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 9700;

    Json call(std::string_view method, Json params = Json::object()) {
        return Json::parse(endpoint.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", ++next_id}, {"method", method}, {"params", std::move(params)}}
                .dump()));
    }
    Json result(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(method << ": " << response.dump(2));
        REQUIRE(response.contains("result"));
        return response.at("result");
    }
    Json error(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(method << ": " << response.dump(2));
        REQUIRE(response.contains("error"));
        return response.at("error");
    }
    Json runtimeObjects() {
        const auto answer = result("get_runtime_objects");
        INFO(answer.dump(2));
        REQUIRE(answer.is_object());
        REQUIRE(answer.size() == 1U);
        REQUIRE(answer.at("objects").is_array());
        return answer.at("objects");
    }
};

struct Dll {
    PelicanGp2SpawnFn spawn = nullptr;
    PelicanGp2ProbeFn materialize = nullptr;
    PelicanGp2ProbeFn world = nullptr;
    PelicanGp2ClearFn clear = nullptr;
    PelicanGp2QueueInsideFn queue_inside = nullptr;
    PelicanGp2InsideResultsFn inside_results = nullptr;
    PelicanGp2GameCodeFn game_code = nullptr;
};

template <class Fn> Fn exportOf(HMODULE module, const char *name) {
    const auto function = reinterpret_cast<Fn>(GetProcAddress(module, name));
    INFO("fixture export " << name);
    REQUIRE(function != nullptr);
    return function;
}

Dll fixtureExports() {
    const auto status = configuredGameLogicStatus();
    REQUIRE(status.loaded);
    auto module = GetModuleHandleW(status.loaded_copy.c_str());
    if (module == nullptr) module = GetModuleHandleW(status.loaded_copy.filename().c_str());
    REQUIRE(module != nullptr);
    return Dll{
        .spawn = exportOf<PelicanGp2SpawnFn>(module, "pelican_gp2_spawn"),
        .materialize = exportOf<PelicanGp2ProbeFn>(module, "pelican_gp2_materialize"),
        .world = exportOf<PelicanGp2ProbeFn>(module, "pelican_gp2_world"),
        .clear = exportOf<PelicanGp2ClearFn>(module, "pelican_gp2_clear"),
        .queue_inside = exportOf<PelicanGp2QueueInsideFn>(module, "pelican_gp2_queue_inside"),
        .inside_results =
            exportOf<PelicanGp2InsideResultsFn>(module, "pelican_gp2_inside_results"),
        .game_code = exportOf<PelicanGp2GameCodeFn>(module, "pelican_gp2_game_code"),
    };
}

TransientObjectRef spawnOutside(const Dll &dll, const char *tag, float x, float y,
                                float z, const char *name = nullptr) {
    Gp2Fixture::Request request;
    dll.spawn("probe", tag, x, y, z, name, &request);
    INFO("spawn " << tag << ": " << request.error);
    REQUIRE(request.ok == 1U);
    return request.ref;
}

Json entityOf(const Dll &dll, const TransientObjectRef &ref) {
    Gp2Fixture::Probe probe;
    dll.materialize(&ref, &probe);
    INFO("materialize: " << probe.error);
    REQUIRE(probe.ok == 1U);
    return Json{{"index", probe.object_index}, {"generation", probe.object_generation}};
}

GameObjectId entityId(const Json &entity) {
    return GameObjectId{entity.at("index").get<std::uint32_t>(),
                        entity.at("generation").get<std::uint32_t>()};
}

std::string spawnName(const TransientObjectRef &ref) {
    return "spawn:" + std::to_string(ref.boundary()) + "." + std::to_string(ref.spawnIndex());
}

// The runtime Transform of an entity, computed here from its ECS components
// through the one projection get_components uses.
Json transformOf(GameObjectId entity) {
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto *world = ecs.tryComponent<TransformComponent>(entity);
    REQUIRE(world != nullptr);
    auto *local = ecs.tryComponent<LocalTransformComponent>(entity);
    return Json::parse(
        projectTransformRuntimeJson(TransformCodecTarget{.world = world, .local = local}).dump());
}

// The form of a JSON value: its keys and their forms, an array's length and
// its elements' forms, and a leaf's type. Two values with the same form carry
// the same fields.
Json formOf(const Json &value) {
    if (value.is_object()) {
        Json form = Json::object();
        for (const auto &[key, field] : value.items()) form[key] = formOf(field);
        return form;
    }
    if (value.is_array()) {
        Json form = Json::array();
        for (const auto &element : value) form.push_back(formOf(element));
        return form;
    }
    if (value.is_number()) return "number";
    return value.type_name();
}

std::uint64_t objectId(Rpc &rpc, std::string_view name) {
    const auto tree = rpc.result("scene_tree");
    for (const auto &object : tree.at("objects"))
        if (object.value("name", std::string{}) == name)
            return object.at("authoring_object_id").get<std::uint64_t>();
    FAIL("scene_tree has no object named " << name);
    return 0;
}

Json authoredTransformRuntimeJson(Rpc &rpc, std::string_view name) {
    const auto components = rpc.result("get_components", {{"name", name}}).at("components");
    for (const auto &component : components) {
        if (component.value("name", std::string{}) == "transform") {
            INFO(component.dump(2));
            REQUIRE(component.contains("runtime_json"));
            return component.at("runtime_json");
        }
    }
    FAIL("get_components of " << name << " has no transform");
    return nullptr;
}

// A --start-state edit launch, as Studio starts the engine, with the GP-2
// fixture DLL as the project's game logic.
struct EditLaunch {
    TempDir dir{std::filesystem::temp_directory_path() /
                ("pelican_ro1_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    std::filesystem::path root = makeProject(dir.root);
    FastModuleContainer modules;

    EditLaunch() {
        setupLogger(true);
        GET_MODULE(PathResolver).setup(root, false);
        GET_MODULE(ProjectSource).setProjectData(readFile(root / "project.json"));
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.rpc = true;
        launch.headless_extent = vk::Extent2D{64, 64};
        launch.shader_hot_reload = false;
        launch.start_state = EngineStartState::edit;
        internal::completeEditStartLaunch(launch);
        launch.game_logic_dll = std::filesystem::path{PELICAN_GP2_SPAWN_FIXTURE};
        GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
        TestSupport::requireVulkanDevice("RO-1 get_runtime_objects over RPC");
        GET_MODULE(ECSPredefinedRegistration).reg();
        if (!initializeConfiguredGameLogic()) {
            FAIL("RO-1 fixture failed to load: " << configuredGameLogicStatus().last_error);
        }
        (void)GET_MODULE(watch::ReloadService);
        internal::applyLaunchStartState();
    }
    ~EditLaunch() {
        GET_MODULE(VulkanManageCore).waitIdle();
        GET_MODULE(BehaviorAttachmentArena).deactivateAll();
        shutdownConfiguredGameLogic();
    }
};

} // namespace

// RO-1 acceptance 1-3 and range 2-3: nothing is listed in edit; the objects
// the game spawns are listed in spawn order with their entity, the prefab
// that made them and their runtime Transform; a parent is reported in the
// form of what it is; a scene load and the end of the play session empty the
// list; an extra parameter is refused.
TEST_CASE("RO-1 get_runtime_objects lists the objects the running game spawned, in spawn order",
          "[ro1][gp2][spawn][rpc][game-logic]") {
    EditLaunch launch;
    Rpc rpc;
    const auto dll = fixtureExports();
    dll.clear();
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");

    // Range 3: the request carries no fields.
    const auto extra = rpc.error("get_runtime_objects", {{"scene_id", "default_scene"}});
    REQUIRE(extra.at("code") == -32602);

    // Range 2: edit runs no game step, so nothing is spawned.
    dll.queue_inside("probe", "early", 9.0f, 0.0f, 0.0f);
    (void)rpc.result("step_frame");
    Gp2Fixture::GameCodeProbe code;
    dll.game_code(&code);
    REQUIRE(code.spawner_updates == 0U);
    REQUIRE(rpc.runtimeObjects() == Json::array());
    dll.clear();

    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}).at("previous") == "edit");
    (void)rpc.result("step_frame");
    REQUIRE(rpc.runtimeObjects() == Json::array());

    // Two spawns from outside every callback, one of them named, and one the
    // spawner behavior makes from its onUpdate in the frame they land.
    const auto a = spawnOutside(dll, "a", 1.0f, 2.0f, 3.0f);
    const auto named = spawnOutside(dll, "named", 4.0f, 0.0f, 0.0f, "Named");
    dll.queue_inside("probe", "c", 5.0f, 0.0f, 0.0f);
    (void)rpc.result("step_frame"); // a and Named land; the spawner asks for c
    (void)rpc.result("step_frame"); // c lands
    std::array<Gp2Fixture::Request, 4> inside{};
    REQUIRE(dll.inside_results(inside.data(), inside.size()) == 1U);
    INFO("inside spawn: " << inside[0].error);
    REQUIRE(inside[0].ok == 1U);
    const auto c = inside[0].ref;
    REQUIRE(c.boundary() == a.boundary() + 1U);

    auto objects = rpc.runtimeObjects();
    INFO(objects.dump(2));
    REQUIRE(objects.size() == 3U);
    const std::array expected_names{spawnName(a), std::string{"Named"}, spawnName(c)};
    const std::array refs{a, named, c};
    const auto authored_form = formOf(authoredTransformRuntimeJson(rpc, "Anchor"));
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto &object = objects[index];
        INFO("object " << index << ": " << object.dump(2));
        REQUIRE(object.at("name") == expected_names[index]);
        REQUIRE(object.at("entity") == entityOf(dll, refs[index]));
        REQUIRE(object.at("spawned_by") == Json{{"prefab", "probe"}});
        REQUIRE_FALSE(object.contains("parent"));
        // Acceptance 1: the runtime Transform, in get_components' form and
        // with the value its projection gives for this entity.
        REQUIRE(formOf(object.at("transform")) == authored_form);
        REQUIRE(object.at("transform") == transformOf(entityId(object.at("entity"))));
        Gp2Fixture::Probe world;
        dll.world(&refs[index], &world);
        REQUIRE(world.ok == 1U);
        const auto &pos = object.at("transform").at("world_trs").at("pos");
        REQUIRE(pos == Json::array({world.world.pos.x, world.world.pos.y, world.world.pos.z}));
    }
    REQUIRE(objects[0].at("transform").at("world_trs").at("pos") == Json::array({1.0, 2.0, 3.0}));

    // Acceptance 3: a spawn has no parent of its own, so the parents are
    // given here, on the live localtransform the transform update follows:
    // c under a (a spawned object), a under Anchor (an authored one).
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    const auto a_entity = entityId(objects[0].at("entity"));
    const auto c_entity = entityId(objects[2].at("entity"));
    const auto anchor_entity = GET_MODULE(SceneLoader).objectId("Anchor");
    REQUIRE(anchor_entity);
    auto *a_local = ecs.tryComponent<LocalTransformComponent>(a_entity);
    auto *c_local = ecs.tryComponent<LocalTransformComponent>(c_entity);
    REQUIRE(a_local != nullptr);
    REQUIRE(c_local != nullptr);
    REQUIRE(a_local->parent == invalidEntityId);
    a_local->parent = *anchor_entity;
    c_local->parent = a_entity;
    objects = rpc.runtimeObjects();
    INFO(objects.dump(2));
    REQUIRE(objects.size() == 3U);
    REQUIRE(objects[0].at("parent") ==
            Json{{"authoring_object_id", objectId(rpc, "Anchor")}, {"scene_id", "default_scene"}});
    REQUIRE_FALSE(objects[1].contains("parent"));
    REQUIRE(objects[2].at("parent") == Json{{"entity", objects[0].at("entity")}});
    a_local->parent = invalidEntityId;
    c_local->parent = invalidEntityId;

    // A play session refuses load_scene (PM-2), and the list is untouched.
    REQUIRE(rpc.error("load_scene", {{"name", "default_scene"}}).at("data").at("code") ==
            "play_session_active");
    REQUIRE(rpc.runtimeObjects().size() == 3U);

    // Acceptance 2: the end of the play session retires the run's spawned objects.
    const auto d = spawnOutside(dll, "d", 6.0f, 0.0f, 0.0f);
    (void)rpc.result("step_frame");
    objects = rpc.runtimeObjects();
    REQUIRE(objects.size() == 4U);
    REQUIRE(objects[3].at("name") == spawnName(d));
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}).at("previous") == "playing");
    REQUIRE(rpc.runtimeObjects() == Json::array());
    (void)rpc.result("step_frame");
    REQUIRE(rpc.runtimeObjects() == Json::array());

    // And a scene load in edit leaves it empty.
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    REQUIRE(rpc.runtimeObjects() == Json::array());
}

} // namespace Pelican
