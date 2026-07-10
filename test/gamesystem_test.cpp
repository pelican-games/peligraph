#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/os/inputstate.hpp"
#include "../src/core/userpublic/details/system/registerer.hpp"
#include "../src/core/userpublic/gamecontext.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

struct ProbeState {
    bool actions_configured = false;
    bool jump_pressed = false;
    ActionAxis2 move{};
    double time = 0.0;
    double dt = 0.0;
    std::uint64_t frame = 0;
};

ProbeState probe_state;

struct Wp43ActionTimeProbeSystem {
    void update(GameContext &ctx) {
        probe_state.actions_configured = ctx.actionsConfigured();
        probe_state.jump_pressed = ctx.actionPressed("jump");
        probe_state.move = ctx.actionAxis2("move");
        probe_state.time = ctx.time();
        probe_state.dt = ctx.deltaTime();
        probe_state.frame = ctx.frameIndex();
    }
};

PELICAN_REGISTER_SYSTEM(Wp43ActionTimeProbeSystem, 50);

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "input_actions";
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    file << text;
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    Sandbox() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("pelican_gamesystem_" + suffix);
        root = base / "project";
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
};

LocalTransformComponent makeTransform(float x, float y, float z) {
    return LocalTransformComponent{
        .scale = vec3{1.0f + x, 1.0f + y, 1.0f + z},
        .rotation = quat{0.0f, 0.0f, 0.0f, 1.0f},
        .pos = vec3{x, y, z},
        .parent = invalidGameObjectId,
    };
}

} // namespace

TEST_CASE("Game system ordering is deterministic by order then registered name", "[gamesystem]") {
    using internal::GameSystemRegistration;
    const auto sorted = internal::sortGameSystemRegistrations(std::vector<GameSystemRegistration>{
        {.name = "Zulu", .order = 10},
        {.name = "Bravo", .order = 5},
        {.name = "Alpha", .order = 5},
        {.name = "Echo", .order = 10},
    });

    REQUIRE(sorted.size() == 4);
    REQUIRE(sorted[0].name == "Alpha");
    REQUIRE(sorted[1].name == "Bravo");
    REQUIRE(sorted[2].name == "Echo");
    REQUIRE(sorted[3].name == "Zulu");
}

TEST_CASE("Registered game system can read Actions and EngineTime through GameContext", "[gamesystem]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "input" / "actions.json",
              readText(fixtureRoot() / "valid" / "gameplay_menu.json"));

    const auto project = nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "game-system-actions"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"input_actions_json", "input/actions.json"}}},
    };

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());

    auto &engine_time = GET_MODULE(EngineTime);
    engine_time.setup(EngineTime::Mode::fixed_step, 0.25);
    engine_time.advance();

    auto &input = GET_MODULE(InputState);
    input.queueEvent(InputEvent::button(KeyCode::D, true));
    input.queueEvent(InputEvent::button(KeyCode::Space, true));
    input.beginFrame();

    probe_state = {};
    GameContext ctx;
    internal::updateRegisteredGameSystems(ctx);

    REQUIRE(probe_state.actions_configured);
    REQUIRE(probe_state.jump_pressed);
    REQUIRE(probe_state.move.x == Catch::Approx(1.0f));
    REQUIRE(probe_state.move.y == Catch::Approx(0.0f));
    REQUIRE(probe_state.time == Catch::Approx(0.25));
    REQUIRE(probe_state.dt == Catch::Approx(0.25));
    REQUIRE(probe_state.frame == 1);
}

TEST_CASE("GameContext creates and updates local transforms without exposing modules", "[gamesystem]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();

    GameContext ctx;
    const auto initial = makeTransform(1.0f, 2.0f, 3.0f);
    const auto object = ctx.createObject(initial);

    auto transform = ctx.localTransform(object);
    REQUIRE(transform.pos.x == Catch::Approx(1.0f));
    REQUIRE(transform.pos.y == Catch::Approx(2.0f));
    REQUIRE(transform.pos.z == Catch::Approx(3.0f));

    const auto updated = makeTransform(4.0f, 5.0f, 6.0f);
    REQUIRE(ctx.setLocalTransform(object, updated));
    transform = ctx.localTransform(object);
    REQUIRE(transform.pos.x == Catch::Approx(4.0f));
    REQUIRE(transform.pos.y == Catch::Approx(5.0f));
    REQUIRE(transform.pos.z == Catch::Approx(6.0f));
    REQUIRE(ctx.removeObject(object));
    REQUIRE_FALSE(ctx.removeObject(object));
    REQUIRE_FALSE(ctx.setLocalTransform(object, updated));
}

} // namespace Pelican
