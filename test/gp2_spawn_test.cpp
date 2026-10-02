#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/framephase.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/runtimespawn.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/userpublic/details/system/registerer.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "scene_load_test_support.hpp"

#if defined(PELICAN_GP2_SPAWN_FIXTURE)
#include "fixtures/gp2_spawn/api.hpp"
#include <Windows.h>
#endif

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// GP-2: GameContext::spawn / destroy. Scenes are loaded through the
// production SceneLoader transaction and advanced with the production frame
// (updateFrameState), whose first step is the spawn boundary; every game-side
// call is made inside the fixture DLL.

#if defined(PELICAN_GP2_SPAWN_FIXTURE)
namespace Pelican {
namespace {
using Json = nlohmann::json;

Json transform(double x, double y, double z) {
    return Json{{"name", "transform"},
                {"pos", {x, y, z}},
                {"rotation", {0.0, 0.0, 0.0, 1.0}},
                {"scale", {1.0, 1.0, 1.0}}};
}

Json sceneDocument() {
    return Json{
        {"schema", "pelican.scene"},
        {"version", 2},
        {"scenes",
         {{"main",
           {{"objects",
             Json::array({
                 {{"name", "Spawner"},
                  {"components",
                   Json::array({{{"name", "behavior"},
                                 {"type", "gp2_spawner"},
                                 {"params", {{"label", "main"}}}}})}},
                 {{"name", "Marker"}, {"components", Json::array({transform(0, 0, 0)})}},
             })}}},
          {"other",
           {{"objects",
             Json::array({
                 {{"name", "Lone"}, {"components", Json::array({transform(0, 0, 0)})}},
             })}}}}},
    };
}

// A collider the ray from above can hit, and the probe behavior that reports
// its lifecycle under the tag it is given.
Json probePrefab() {
    return Json{
        {"schema", "pelican.prefab"},
        {"version", 1},
        {"name", "probe"},
        {"parameters",
         Json::array({{{"name", "tag"}, {"type", "string"}, {"default", "none"}}})},
        {"components",
         Json::array({
             {{"name", "collider"}, {"shape", "sphere"}, {"radius", 0.5}},
             {{"name", "behavior"},
              {"type", "gp2_probe"},
              {"params", {{"tag", {{"$param", "tag"}}}}}},
         })},
    };
}

Json lampPrefab() {
    return Json{
        {"schema", "pelican.prefab"},
        {"version", 1},
        {"name", "lamp"},
        {"components",
         Json::array({{{"name", "light"},
                       {"type", "directional"},
                       {"direction", {0.0, -1.0, 0.0}},
                       {"intensity", 1.0},
                       {"color", {1.0, 1.0, 1.0}}}})},
    };
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;
    Sandbox() {
        base = std::filesystem::temp_directory_path() /
               ("pelican_gp2_spawn_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base / "project";
        std::filesystem::create_directories(root / "passes");
        std::filesystem::create_directories(root / "prefabs");
        std::ofstream{root / "scene.json", std::ios::binary} << sceneDocument().dump(2);
        std::ofstream{root / "prefabs/probe.json", std::ios::binary} << probePrefab().dump(2);
        std::ofstream{root / "prefabs/lamp.json", std::ios::binary} << lampPrefab().dump(2);
        std::ofstream{root / "passes/main_rendering_config.json", std::ios::binary}
            << R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})";
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

void configureProject(const Sandbox &sandbox) {
    // The production frame (updateFrameState) runs here without a window.
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{32, 32};
    launch.headless_frames = 1;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(
        Json{{"schema", "pelican.project"},
             {"version", 1},
             {"name", "gp2-spawn"},
             {"engine_min_version", "0.1.0"},
             {"prefabs",
              Json::array({{{"name", "probe"}, {"path", "prefabs/probe.json"}},
                           {{"name", "lamp"}, {"path", "prefabs/lamp.json"}}})},
             {"basic_config",
              {{"window_size", {{"width", 100}, {"height", 100}}},
               {"camera",
                {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 1000.0}}},
               {"default_scene_id", "main"},
               {"scene_data_json", "scene.json"}}}}
            .dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
}

struct Dll {
    PelicanGp2SpawnFn spawn = nullptr;
    PelicanGp2DestroyFn destroy = nullptr;
    PelicanGp2ProbeFn materialize = nullptr;
    PelicanGp2ProbeFn world = nullptr;
    PelicanGp2RaycastFn raycast = nullptr;
    PelicanGp2TraceFn trace = nullptr;
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
        .destroy = exportOf<PelicanGp2DestroyFn>(module, "pelican_gp2_destroy"),
        .materialize = exportOf<PelicanGp2ProbeFn>(module, "pelican_gp2_materialize"),
        .world = exportOf<PelicanGp2ProbeFn>(module, "pelican_gp2_world"),
        .raycast = exportOf<PelicanGp2RaycastFn>(module, "pelican_gp2_raycast"),
        .trace = exportOf<PelicanGp2TraceFn>(module, "pelican_gp2_trace"),
        .clear = exportOf<PelicanGp2ClearFn>(module, "pelican_gp2_clear"),
        .queue_inside = exportOf<PelicanGp2QueueInsideFn>(module, "pelican_gp2_queue_inside"),
        .inside_results =
            exportOf<PelicanGp2InsideResultsFn>(module, "pelican_gp2_inside_results"),
        .game_code = exportOf<PelicanGp2GameCodeFn>(module, "pelican_gp2_game_code"),
    };
}

class GameLogicScope {
  public:
    GameLogicScope() {
        GET_MODULE(EngineLaunchConfig).game_logic_dll =
            std::filesystem::path{PELICAN_GP2_SPAWN_FIXTURE};
        if (!initializeConfiguredGameLogic()) {
            FAIL("GP-2 fixture failed to load: " << configuredGameLogicStatus().last_error);
        }
    }
    ~GameLogicScope() {
        GET_MODULE(BehaviorAttachmentArena).deactivateAll();
        shutdownConfiguredGameLogic();
    }
    GameLogicScope(const GameLogicScope &) = delete;
    GameLogicScope &operator=(const GameLogicScope &) = delete;
};

std::vector<Gp2Fixture::TraceEntry> traceOf(const Dll &dll) {
    std::vector<Gp2Fixture::TraceEntry> entries(64);
    const auto count = dll.trace(entries.data(), entries.size());
    REQUIRE(count <= entries.size());
    entries.resize(count);
    return entries;
}

std::string describe(const std::vector<Gp2Fixture::TraceEntry> &trace) {
    std::ostringstream out;
    for (const auto &entry : trace) {
        out << (entry.kind == Gp2Fixture::TraceKind::init ? "init:" : "destroy:")
            << entry.tag << "@" << entry.attachment_seq << " ";
    }
    return out.str();
}

Gp2Fixture::Request spawnOutside(const Dll &dll, const char *tag, float x,
                                 const char *name = nullptr) {
    Gp2Fixture::Request result;
    dll.spawn("probe", tag, x, 0.0f, 0.0f, name, &result);
    INFO("spawn " << tag << ": " << result.error);
    REQUIRE(result.ok == 1U);
    return result;
}

Gp2Fixture::Probe materialized(const Dll &dll, const TransientObjectRef &ref) {
    Gp2Fixture::Probe probe;
    dll.materialize(&ref, &probe);
    return probe;
}

Gp2Fixture::RayProbe rayDown(const Dll &dll, float x) {
    Gp2Fixture::RayProbe probe;
    dll.raycast(x, 5.0f, 0.0f, 0.0f, -1.0f, 0.0f, &probe);
    return probe;
}

std::size_t spawnedAttachments() {
    const auto snapshot = GET_MODULE(BehaviorAttachmentArena).snapshot();
    return static_cast<std::size_t>(std::count_if(
        snapshot.begin(), snapshot.end(),
        [](const BehaviorAttachmentInfo &info) { return info.runtime_spawned; }));
}

std::size_t runtimeOnlyColliders() {
    const auto state = GET_MODULE(PhysWorld).snapshotPrepared();
    return static_cast<std::size_t>(std::count_if(
        state.bindings.begin(), state.bindings.end(),
        [](const PhysWorld::Binding &binding) { return binding.runtime_only; }));
}

struct Fixture {
    Sandbox sandbox;
    FastModuleContainer modules;
    std::optional<GameLogicScope> game_logic;
    Dll dll;

    Fixture() {
        setupLogger();
        configureProject(sandbox);
        game_logic.emplace();
        dll = fixtureExports();
        dll.clear();
        load("main");
    }
    ~Fixture() { game_logic.reset(); }

    void load(std::string_view scene) {
        test_support::loadScene(GET_MODULE(SceneLoader), SceneId{std::string{scene}});
    }
};

// One run of the "preview" the determinism check repeats: the same input,
// with spawns made both outside callbacks (the host through the DLL) and
// inside one (the spawner behavior's onUpdate).
struct RunRecord {
    std::vector<std::pair<std::string, std::uint64_t>> inits;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> refs;
    ObjectAuthority authority;
};

RunRecord previewRun(Fixture &fixture) {
    auto &dll = fixture.dll;
    dll.clear();
    fixture.load("main");
    updateFrameState();
    RunRecord record;
    const auto outside_a = spawnOutside(dll, "a", 1.0f);
    dll.queue_inside("probe", "b", 2.0f, 0.0f, 0.0f);
    dll.queue_inside("probe", "c", 3.0f, 0.0f, 0.0f);
    updateFrameState(); // a lands; the spawner asks for b and c
    updateFrameState(); // b and c land
    const auto outside_d = spawnOutside(dll, "d", 4.0f);
    updateFrameState(); // d lands
    updateFrameState();
    for (const auto &entry : traceOf(dll)) {
        if (entry.kind == Gp2Fixture::TraceKind::init) {
            record.inits.emplace_back(entry.tag, entry.attachment_seq);
        }
    }
    std::array<Gp2Fixture::Request, 8> inside{};
    const auto inside_count = dll.inside_results(inside.data(), inside.size());
    REQUIRE(inside_count == 2U);
    for (const auto &request :
         {outside_a, inside[0], inside[1], outside_d}) {
        REQUIRE(request.ok == 1U);
        record.refs.emplace_back(request.ref.boundary(), request.ref.spawnIndex());
    }
    record.authority = outside_a.ref.authority();
    return record;
}

// PM-1a: an editor system of the host (engine-owned and marked the way the
// modal transform and the camera controller are), with what it last read of
// the clock through GameContext.
struct EditorProbe {
    std::uint64_t updates = 0;
    std::uint64_t last_frame = 0;
    double last_dt = -1.0;
};
EditorProbe editor_probe;

struct Pm1aEditorProbeSystem {
    void update(GameContext &ctx) {
        ++editor_probe.updates;
        editor_probe.last_frame = ctx.frameIndex();
        editor_probe.last_dt = ctx.deltaTime();
    }
};

struct Pm1aEditorProbeRegistration {
    Pm1aEditorProbeRegistration() {
        internal::markEditorGameSystem(
            internal::getGameSystemRegisterer().registerSystem<Pm1aEditorProbeSystem>(
                "Pm1aEditorProbeSystem", 10020, {}));
    }
};
const Pm1aEditorProbeRegistration pm1a_editor_probe_registration;

bool registeredAsEditorSystem(std::string_view name) {
    for (const auto &system : internal::getGameSystemRegisterer().registeredSystems()) {
        if (system.name == name) return internal::isEditorGameSystem(system.token);
    }
    FAIL("no registered system is named " << name);
    return false;
}

Gp2Fixture::GameCodeProbe gameCodeOf(const Dll &dll) {
    Gp2Fixture::GameCodeProbe probe;
    dll.game_code(&probe);
    return probe;
}

} // namespace

// Acceptance 1 and 6: a prefab with a collider and a behavior, spawned from
// the game DLL. Nothing is there before the next boundary; after it the ray
// hits the spawned object and onInit ran once; after a destroy's boundary
// onDestroy ran and the ray misses.
TEST_CASE("GP-2 a spawned prefab's collider and behavior live from the next boundary until its destroy",
          "[gp2][spawn][game-logic]") {
    Fixture fixture;
    auto &dll = fixture.dll;
    updateFrameState();
    REQUIRE(traceOf(dll).empty());
    REQUIRE_FALSE(rayDown(dll, 0.0f).hit);

    const auto spawned = spawnOutside(dll, "p0", 0.0f);
    CHECK(spawned.ref.boundary() == 2U);
    CHECK(spawned.ref.spawnIndex() == 0U);

    // Acceptance 6: before its boundary the ref names nothing live.
    const auto early = materialized(dll, spawned.ref);
    REQUIRE(early.ok == 0U);
    REQUIRE(std::string{early.error} == "object.not_materialized@1");
    REQUIRE_FALSE(rayDown(dll, 0.0f).hit);
    REQUIRE(traceOf(dll).empty());

    updateFrameState();
    const auto live = materialized(dll, spawned.ref);
    INFO("materialize: " << live.error);
    REQUIRE(live.ok == 1U);
    const auto hit = rayDown(dll, 0.0f);
    REQUIRE(hit.hit == 1U);
    REQUIRE(hit.entity_index == live.object_index);
    REQUIRE(hit.entity_generation == live.object_generation);
    Gp2Fixture::Probe world;
    dll.world(&spawned.ref, &world);
    REQUIRE(world.ok == 1U);
    CHECK(world.world.pos.x == Catch::Approx(0.0));
    {
        const auto trace = traceOf(dll);
        INFO(describe(trace));
        REQUIRE(trace.size() == 1U);
        REQUIRE(trace[0].kind == Gp2Fixture::TraceKind::init);
        REQUIRE(std::string{trace[0].tag} == "p0");
        REQUIRE(trace[0].entity_index == live.object_index);
        REQUIRE(trace[0].attachment_seq == runtimeSpawnAttachmentSeq(2, 0, 0));
    }
    // onInit ran once, not once per frame.
    updateFrameState();
    REQUIRE(traceOf(dll).size() == 1U);

    // The spawned object is nobody's authored object.
    REQUIRE_FALSE(GET_MODULE(SceneLoader).hasRuntimeOnlyChanges());
    REQUIRE(std::none_of(GET_MODULE(SceneLoader).runtimeObjectBindings().begin(),
                         GET_MODULE(SceneLoader).runtimeObjectBindings().end(),
                         [&](const SceneRuntimeObjectBinding &binding) {
                             return binding.object_id.index == live.object_index;
                         }));

    Gp2Fixture::Request destroyed;
    dll.destroy(&spawned.ref, &destroyed);
    INFO("destroy: " << destroyed.error);
    REQUIRE(destroyed.ok == 1U);
    // Queued, not done: still live until the boundary.
    REQUIRE(materialized(dll, spawned.ref).ok == 1U);
    REQUIRE(rayDown(dll, 0.0f).hit == 1U);
    Gp2Fixture::Request twice;
    dll.destroy(&spawned.ref, &twice);
    REQUIRE(twice.ok == 0U);
    REQUIRE(std::string{twice.error} == "object.not_materialized@1");

    updateFrameState();
    {
        const auto trace = traceOf(dll);
        INFO(describe(trace));
        REQUIRE(trace.size() == 2U);
        REQUIRE(trace[1].kind == Gp2Fixture::TraceKind::destroy);
        REQUIRE(std::string{trace[1].tag} == "p0");
        REQUIRE(trace[1].attachment_seq == trace[0].attachment_seq);
    }
    const auto gone = materialized(dll, spawned.ref);
    REQUIRE(gone.ok == 0U);
    REQUIRE(std::string{gone.error} == "object.not_materialized@1");
    REQUIRE_FALSE(rayDown(dll, 0.0f).hit);
    REQUIRE(spawnedAttachments() == 0U);
    REQUIRE(runtimeOnlyColliders() == 0U);
}

// Acceptance 2: where the call is made does not change what it does. The
// same three spawns, all from outside callbacks in one run and all from
// inside the spawner's onUpdate in the other, land at the same boundary of
// their frame, in call order, with the same identifiers. Mixed in one frame,
// they keep call order across the two sides.
TEST_CASE("GP-2 a spawn inside a behavior callback and one outside land the same way",
          "[gp2][spawn][order][game-logic]") {
    Fixture fixture;
    auto &dll = fixture.dll;
    updateFrameState();

    // Outside: made between frames, before the frame whose boundary lands them.
    dll.clear();
    std::vector<Gp2Fixture::Request> outside;
    for (const auto *tag : {"x0", "x1", "x2"}) {
        outside.push_back(spawnOutside(dll, tag, 1.0f));
    }
    for (const auto &request : outside) {
        REQUIRE(materialized(dll, request.ref).ok == 0U);
    }
    updateFrameState();
    const auto outside_trace = traceOf(dll);

    // Inside: made during a frame's onUpdate; they land at the next boundary.
    fixture.load("main");
    dll.clear();
    updateFrameState();
    for (const auto *tag : {"x0", "x1", "x2"}) {
        dll.queue_inside("probe", tag, 1.0f, 0.0f, 0.0f);
    }
    updateFrameState(); // the spawner makes the three requests
    std::array<Gp2Fixture::Request, 8> inside{};
    REQUIRE(dll.inside_results(inside.data(), inside.size()) == 3U);
    for (std::size_t index = 0; index < 3U; ++index) {
        INFO("inside spawn " << index << ": " << inside[index].error);
        REQUIRE(inside[index].ok == 1U);
        REQUIRE(materialized(dll, inside[index].ref).ok == 0U);
    }
    REQUIRE(traceOf(dll).empty());
    updateFrameState();
    const auto inside_trace = traceOf(dll);

    INFO("outside: " << describe(outside_trace));
    INFO("inside: " << describe(inside_trace));
    REQUIRE(outside_trace.size() == 3U);
    REQUIRE(inside_trace.size() == 3U);
    for (std::size_t index = 0; index < 3U; ++index) {
        REQUIRE(std::string{outside_trace[index].tag} == std::string{inside_trace[index].tag});
        REQUIRE(outside_trace[index].kind == inside_trace[index].kind);
        REQUIRE(outside[index].ref.spawnIndex() == inside[index].ref.spawnIndex());
        REQUIRE(outside[index].ref.spawnIndex() == index);
        // Each landed at the first boundary after the frame it was made in:
        // boundary 2 of its run either way.
        REQUIRE(outside[index].ref.boundary() == 2U);
        REQUIRE(inside[index].ref.boundary() == 3U);
    }
    // Relative to the boundary each landed at, the identifiers agree.
    for (std::size_t index = 0; index < 3U; ++index) {
        REQUIRE(outside_trace[index].attachment_seq ==
                runtimeSpawnAttachmentSeq(2, index, 0));
        REQUIRE(inside_trace[index].attachment_seq ==
                runtimeSpawnAttachmentSeq(3, index, 0));
    }

    // Mixed in one frame: outside, then inside (during the frame), then
    // outside again after it. One queue, call order.
    fixture.load("main");
    dll.clear();
    updateFrameState();
    const auto first = spawnOutside(dll, "m0", 1.0f);
    updateFrameState(); // m0 lands
    dll.queue_inside("probe", "m1", 2.0f, 0.0f, 0.0f);
    updateFrameState(); // m1 requested inside this frame
    const auto third = spawnOutside(dll, "m2", 3.0f);
    REQUIRE(dll.inside_results(inside.data(), inside.size()) == 1U);
    REQUIRE(inside[0].ref.boundary() == third.ref.boundary());
    REQUIRE(inside[0].ref.spawnIndex() == 0U);
    REQUIRE(third.ref.spawnIndex() == 1U);
    REQUIRE(first.ref.boundary() + 2U == third.ref.boundary());
    updateFrameState();
    const auto mixed = traceOf(dll);
    INFO("mixed: " << describe(mixed));
    REQUIRE(mixed.size() == 3U);
    REQUIRE(std::string{mixed[0].tag} == "m0");
    REQUIRE(std::string{mixed[1].tag} == "m1");
    REQUIRE(std::string{mixed[2].tag} == "m2");
    REQUIRE(mixed[1].attachment_seq < mixed[2].attachment_seq);
}

// Acceptance 3: two runs of the same scene with the same input give the same
// attachment identifiers and refs; only the authority (the scene run) differs.
TEST_CASE("GP-2 the same input in two runs of a scene gives the same attachment identifiers",
          "[gp2][spawn][determinism][game-logic]") {
    Fixture fixture;
    const auto first = previewRun(fixture);
    const auto second = previewRun(fixture);
    INFO("first run inits: " << first.inits.size() << " second: " << second.inits.size());
    REQUIRE(first.inits.size() == 4U);
    REQUIRE(first.inits == second.inits);
    REQUIRE(first.refs == second.refs);
    REQUIRE_FALSE(first.authority == second.authority);
    // And what they are: (boundary, spawn index, attachment index).
    REQUIRE(first.inits[0] == std::pair<std::string, std::uint64_t>{
                                  "a", runtimeSpawnAttachmentSeq(2, 0, 0)});
    REQUIRE(first.inits[1] == std::pair<std::string, std::uint64_t>{
                                  "b", runtimeSpawnAttachmentSeq(3, 0, 0)});
    REQUIRE(first.inits[2] == std::pair<std::string, std::uint64_t>{
                                  "c", runtimeSpawnAttachmentSeq(3, 1, 0)});
    REQUIRE(first.inits[3] == std::pair<std::string, std::uint64_t>{
                                  "d", runtimeSpawnAttachmentSeq(4, 0, 0)});
}

// Acceptance 4: a spawned object is never saved, and the end of the scene run
// -- a scene load, the same scene included, which is how Studio's embedded
// player ends a preview -- ends it: onDestroy, no collider, a stale ref.
TEST_CASE("GP-2 a spawned object is not saved and ends with its scene run",
          "[gp2][spawn][lifetime][game-logic]") {
    Fixture fixture;
    auto &dll = fixture.dll;
    updateFrameState();
    const auto named = spawnOutside(dll, "s0", 0.0f, "Spawned");
    const auto unnamed = spawnOutside(dll, "s1", 2.0f);
    updateFrameState();
    REQUIRE(materialized(dll, named.ref).ok == 1U);
    REQUIRE(materialized(dll, unnamed.ref).ok == 1U);
    const auto hit = rayDown(dll, 0.0f);
    REQUIRE(hit.hit == 1U);
    REQUIRE(std::string{hit.name} == "Spawned");
    REQUIRE(spawnedAttachments() == 2U);
    REQUIRE(runtimeOnlyColliders() == 2U);

    // Saving writes the document, which never had them.
    auto &loader = GET_MODULE(SceneLoader);
    REQUIRE_FALSE(loader.hasRuntimeOnlyChanges());
    auto &config = GET_MODULE(ProjectBasicConfig);
    REQUIRE_NOTHROW(config.saveSceneDocument());
    std::ifstream saved_file{fixture.sandbox.root / "scene.json", std::ios::binary};
    const std::string saved{std::istreambuf_iterator<char>{saved_file}, {}};
    const auto saved_json = Json::parse(saved);
    const auto &objects = saved_json.at("scenes").at("main").at("objects");
    REQUIRE(objects.size() == 2U);
    CHECK(saved.find("Spawned") == std::string::npos);
    CHECK(saved.find("gp2_probe") == std::string::npos);
    CHECK(saved.find("spawn-") == std::string::npos);

    // The run ends: reload the scene.
    fixture.load("main");
    {
        const auto trace = traceOf(dll);
        INFO(describe(trace));
        const auto destroys = std::count_if(trace.begin(), trace.end(), [](const auto &entry) {
            return entry.kind == Gp2Fixture::TraceKind::destroy;
        });
        REQUIRE(destroys == 2);
    }
    REQUIRE(spawnedAttachments() == 0U);
    REQUIRE(runtimeOnlyColliders() == 0U);
    REQUIRE_FALSE(rayDown(dll, 0.0f).hit);
    const auto stale = materialized(dll, named.ref);
    REQUIRE(stale.ok == 0U);
    REQUIRE(std::string{stale.error} == "object.stale_authority@1");
    Gp2Fixture::Request stale_destroy;
    dll.destroy(&named.ref, &stale_destroy);
    REQUIRE(std::string{stale_destroy.error} == "object.stale_authority@1");
    updateFrameState();
    REQUIRE(GET_MODULE(RuntimeSpawner).stats().live == 0U);

    // A spawn queued in a run that ends before its boundary is dropped.
    const auto orphan = spawnOutside(dll, "s2", 0.0f);
    fixture.load("other");
    updateFrameState();
    REQUIRE(std::string{materialized(dll, orphan.ref).error} == "object.stale_authority@1");
    REQUIRE(GET_MODULE(RuntimeSpawner).stats().live == 0U);
    REQUIRE_FALSE(rayDown(dll, 0.0f).hit);
}

// The refusals a spawn answers with at the call, by name, and nothing queued.
TEST_CASE("GP-2 a spawn the engine cannot carry out is refused at the call by name",
          "[gp2][spawn][errors][game-logic]") {
    Fixture fixture;
    auto &dll = fixture.dll;
    updateFrameState();
    const auto refused = [&](const char *prefab, const char *name) {
        Gp2Fixture::Request result;
        dll.spawn(prefab, "t", 0.0f, 0.0f, 0.0f, name, &result);
        REQUIRE(result.ok == 0U);
        return std::string{result.error};
    };
    REQUIRE(refused("nothing", nullptr) == "prefab_not_found");
    const GameContext ctx;
    try {
        (void)ctx.spawn("lamp", SpawnParams{});
        FAIL("a prefab with a light was accepted");
    } catch (const SpawnError &error) {
        REQUIRE(error.code() == "object.spawn_component_unsupported@1");
    }
    REQUIRE(refused("probe", "Marker") == "object.name_reserved_by_authored@1");
    (void)spawnOutside(dll, "n0", 0.0f, "Twin");
    REQUIRE(refused("probe", "Twin") == "object.name_duplicate@1");
    REQUIRE(GET_MODULE(RuntimeSpawner).stats().queued == 1U);

    // A parameter the prefab does not declare.
    SpawnParams params;
    params.parameters = Json{{"hp", 3}};
    try {
        (void)ctx.spawn("probe", params);
        FAIL("an undeclared parameter was accepted");
    } catch (const SpawnError &error) {
        REQUIRE(error.code() == "prefab_parameter_unknown");
    }
}

// PM-1a acceptance 3, through the production frame (advance + updateFrameState)
// with the game DLL loaded. Paused, the DLL's behavior onUpdate and its game
// system are not called, a spawn waits at the boundary, and the editor
// systems still run on the editor clock. A requested step runs the game once;
// playing again runs it every frame.
TEST_CASE("PM-1a a paused frame runs no game code and lands no spawn, and the editor systems still run",
          "[gp2][pm1a][game-logic]") {
    Fixture fixture;
    auto &dll = fixture.dll;
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    const auto frame = [&] {
        time.advance();
        updateFrameState();
    };

    // The engine's two editor systems carry the mark; game systems do not.
    REQUIRE(registeredAsEditorSystem("BuiltinModalTransformSystem"));
    REQUIRE(registeredAsEditorSystem("BuiltinCameraControllerSystem"));
    REQUIRE_FALSE(registeredAsEditorSystem("BehaviorSystem"));
    REQUIRE_FALSE(registeredAsEditorSystem("Gp2TickSystem"));

    frame();
    frame();
    const auto playing = gameCodeOf(dll);
    REQUIRE(playing.spawner_updates == 2U);
    REQUIRE(playing.system_updates == 2U);
    REQUIRE(playing.system_last_frame == 2U);
    REQUIRE(playing.system_last_dt == Catch::Approx(1.0 / 60.0));
    const auto editor_before = editor_probe.updates;

    time.setPlayState(EngineTime::PlayState::paused);
    (void)spawnOutside(dll, "paused", 1.0f);
    dll.queue_inside("probe", "inside", 2.0f, 0.0f, 0.0f);
    for (int paused_frame = 0; paused_frame < 3; ++paused_frame) frame();
    const auto paused = gameCodeOf(dll);
    REQUIRE(paused.spawner_updates == playing.spawner_updates);
    REQUIRE(paused.system_updates == playing.system_updates);
    REQUIRE(traceOf(dll).empty());
    REQUIRE(spawnedAttachments() == 0U);
    REQUIRE(GET_MODULE(RuntimeSpawner).stats().queued == 1U);
    REQUIRE(time.frameIndex() == 2U);
    REQUIRE(time.evaluationIndex() == 5U);
    // The editor systems ran on every paused frame, on the editor clock: the
    // evaluation index and the frame's own dt, not the stopped game clock.
    REQUIRE(editor_probe.updates == editor_before + 3U);
    REQUIRE(editor_probe.last_frame == time.evaluationIndex());
    REQUIRE(editor_probe.last_dt == Catch::Approx(1.0 / 60.0));
    REQUIRE(time.dt() == 0.0);

    // One requested step: the spawn lands at its boundary, the behavior and
    // the system run once and read the step dt. ctx.frameIndex() is the
    // evaluation index for game code too -- the revision the engine runs the
    // animation phases with, which game code hands to prepareTick -- so after
    // the pause it has jumped by the paused frames while the game frame has
    // not.
    time.requestSteps(1);
    frame();
    const auto stepped = gameCodeOf(dll);
    REQUIRE(stepped.spawner_updates == playing.spawner_updates + 1U);
    REQUIRE(stepped.system_updates == playing.system_updates + 1U);
    REQUIRE(time.frameIndex() == 3U);
    REQUIRE(stepped.system_last_frame == time.evaluationIndex());
    REQUIRE(stepped.system_last_frame == 6U);
    REQUIRE(stepped.system_last_dt == Catch::Approx(1.0 / 60.0));
    REQUIRE(spawnedAttachments() == 1U);
    REQUIRE(time.evaluationIndex() == 6U);
    frame();
    REQUIRE(gameCodeOf(dll).system_updates == stepped.system_updates);

    // Playing again: the spawn the behavior made in the step lands.
    time.setPlayState(EngineTime::PlayState::playing);
    frame();
    const auto resumed = gameCodeOf(dll);
    REQUIRE(resumed.system_updates == stepped.system_updates + 1U);
    REQUIRE(resumed.system_last_frame == time.evaluationIndex());
    REQUIRE(resumed.system_last_frame == 8U);
    REQUIRE(time.frameIndex() == 4U);
    REQUIRE(spawnedAttachments() == 2U);
}
} // namespace Pelican
#endif
