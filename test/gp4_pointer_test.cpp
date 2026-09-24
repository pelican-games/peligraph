#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/os/inputsequence.hpp"
#include "../src/core/os/inputstate.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/ui/inputrouter.hpp"
#include "../src/core/ui/widgetarena.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "scene_load_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// GP-4: the pointer in framebuffer pixels, its record and replay, the UI
// router's conversion, the screen ray and the cursor mode. Everything here
// runs the production functions: InputSequenceRuntime::prepareFrame /
// recordFrame, InputState::beginFrame, InputRouter::routeFrameInput and the
// public facades.

namespace Pelican {
namespace {
using Json = nlohmann::json;

std::filesystem::path uniqueTemp(std::string_view stem) {
    return std::filesystem::temp_directory_path() /
           (std::string{stem} + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void requirePointer(const PointerState &actual, float x, float y, bool inside) {
    CHECK(actual.position_px.x == Catch::Approx(x));
    CHECK(actual.position_px.y == Catch::Approx(y));
    CHECK(actual.inside == inside);
}

} // namespace

// Acceptance 1. A sequence recorded on a 200% display (framebuffer 200x160,
// window 100x80) replays on a 100% one: the pointer reports the pixels the
// recording saw, not the window coordinates times this machine's scale.
TEST_CASE("GP-4 a replay reports the pointer pixels it recorded whatever this machine's content scale",
          "[gp4][pointer][replay]") {
    FastModuleContainer modules;
    auto &input = GET_MODULE(InputState);
    auto &runtime = GET_MODULE(InputSequenceRuntime);
    const GameContext ctx;
    const auto path = uniqueTemp("pelican_gp4_replay") += ".jsonl";

    const auto hidpi = pointerTransformFor(200, 160, 100, 80);
    REQUIRE(hidpi.scale_x == 2.0f);
    REQUIRE(hidpi.scale_y == 2.0f);
    // The window moved to a display that is not scaled.
    const auto plain = pointerTransformFor(100, 80, 100, 80);
    REQUIRE(plain.scale_x == 1.0f);

    runtime.startRecording(path, 60.0);
    const auto recordFrame = [&](InputEvent event, const PointerTransform &live) {
        input.queueEvent(event);
        runtime.prepareFrame(input, live);
        input.beginFrame();
        runtime.recordFrame(input.currentFrameInput());
        return ctx.input().pointer();
    };
    requirePointer(recordFrame(InputEvent::cursorMove(30.0f, 40.0f), hidpi), 60.0f, 80.0f, true);
    // Past the framebuffer's right edge: reported, not clamped, and outside.
    requirePointer(recordFrame(InputEvent::cursorMove(120.0f, 10.0f), hidpi), 240.0f, 20.0f, false);
    requirePointer(recordFrame(InputEvent::cursorMove(50.0f, 40.0f), plain), 50.0f, 40.0f, true);
    (void)runtime.stopRecording();

    const auto text = readText(path);
    INFO(text);
    std::istringstream lines{text};
    std::string header_line;
    std::getline(lines, header_line);
    const auto header = Json::parse(header_line);
    CHECK(header.at("version") == 2);
    CHECK(header.at("content_scale") == Json::array({2.0, 2.0}));
    CHECK(header.at("framebuffer") == Json::array({200, 160}));
    // Only the frame whose transform changed carries one.
    CHECK(text.find(R"({"content_scale":[1.0,1.0],"frame":2,"framebuffer":[100,80]})") != std::string::npos);
    CHECK(text.find(R"({"frame":1})") != std::string::npos);

    // Replay on the 100% machine.
    input.clear();
    runtime.startReplay(path);
    const auto replayFrame = [&] {
        runtime.prepareFrame(input, plain);
        input.beginFrame();
        return ctx.input().pointer();
    };
    requirePointer(replayFrame(), 60.0f, 80.0f, true);
    requirePointer(replayFrame(), 240.0f, 20.0f, false);
    requirePointer(replayFrame(), 50.0f, 40.0f, true);
    REQUIRE(runtime.replayComplete());
    // The replay is done: live input converts by this machine again.
    input.queueEvent(InputEvent::cursorMove(30.0f, 40.0f));
    requirePointer(replayFrame(), 30.0f, 40.0f, true);
    runtime.stopReplay();

    std::filesystem::remove(path);
}

// v1 recordings live outside the project (MG-1 does not migrate them). What
// their window coordinates meant then -- the UI router took them as pixels --
// is content scale 1, and the framebuffer is this machine's.
TEST_CASE("GP-4 a v1 recording replays its window coordinates at content scale 1",
          "[gp4][pointer][replay]") {
    FastModuleContainer modules;
    auto &input = GET_MODULE(InputState);
    auto &runtime = GET_MODULE(InputSequenceRuntime);
    const GameContext ctx;
    const auto path = uniqueTemp("pelican_gp4_v1") += ".jsonl";
    std::ofstream{path, std::ios::binary}
        << "{\"schema\":\"pelican.input_seq\",\"version\":1,\"fps\":60}\n"
           "{\"frame\":0}\n"
           "{\"event_seq\":0,\"type\":\"cursor_move\",\"x\":30,\"y\":40}\n"
           "{\"frame\":1}\n"
           "{\"event_seq\":1,\"type\":\"cursor_move\",\"x\":90,\"y\":40}\n";

    runtime.startReplay(path);
    const auto hidpi = pointerTransformFor(160, 120, 80, 60);
    runtime.prepareFrame(input, hidpi);
    input.beginFrame();
    requirePointer(ctx.input().pointer(), 30.0f, 40.0f, true);
    CHECK(input.pointerTransform().scale_x == 1.0f);
    CHECK(input.pointerTransform().framebuffer_width == 160);
    runtime.prepareFrame(input, hidpi);
    input.beginFrame();
    requirePointer(ctx.input().pointer(), 90.0f, 40.0f, true);
    runtime.stopReplay();
    std::filesystem::remove(path);
}

TEST_CASE("GP-4 input_seq v2 requires its header transform and v1 carries none",
          "[gp4][pointer][input-sequence]") {
    const std::string v2 =
        R"({"schema":"pelican.input_seq","version":2,"fps":60,"content_scale":[1.5,1.5],"framebuffer":[300,150]})";
    const auto parsed = InputSequence::fromJsonLines(
        v2 + "\n{\"frame\":0}\n{\"content_scale\":[1.0,2.0],\"frame\":1,\"framebuffer\":[64,32]}\n{\"frame\":2}\n");
    REQUIRE(parsed.frames().size() == 3);
    CHECK(parsed.sourceVersion() == 2);
    CHECK(parsed.frames()[0].pointer_transform == PointerTransform{1.5f, 1.5f, 300, 150});
    CHECK(parsed.frames()[1].pointer_transform == PointerTransform{1.0f, 2.0f, 64, 32});
    CHECK(parsed.frames()[2].pointer_transform == PointerTransform{1.0f, 2.0f, 64, 32});
    CHECK(InputSequence::fromJsonLines(parsed.toJsonLines()).toJsonLines() == parsed.toJsonLines());

    REQUIRE_THROWS_WITH(
        InputSequence::fromJsonLines(R"({"schema":"pelican.input_seq","version":2,"fps":60})"
                                     "\n{\"frame\":0}\n"),
        Catch::Matchers::ContainsSubstring("content_scale"));
    REQUIRE_THROWS_WITH(
        InputSequence::fromJsonLines(
            R"({"schema":"pelican.input_seq","version":2,"fps":60,"content_scale":[0,1],"framebuffer":[1,1]})"
            "\n{\"frame\":0}\n"),
        Catch::Matchers::ContainsSubstring("positive"));
    REQUIRE_THROWS_WITH(
        InputSequence::fromJsonLines(R"({"schema":"pelican.input_seq","version":1,"fps":60})"
                                     "\n{\"content_scale\":[2,2],\"frame\":0,\"framebuffer\":[2,2]}\n"),
        Catch::Matchers::ContainsSubstring("needs version 2"));
    REQUIRE_THROWS_WITH(
        InputSequence::fromJsonLines(R"({"schema":"pelican.input_seq","version":3,"fps":60})"
                                     "\n{\"frame\":0}\n"),
        "input_seq version is not supported");
}

// Acceptance 5, the half this WP changes: the router used to take window
// coordinates as framebuffer pixels (DPI 1.0 fixed). On a 200% window a
// cursor at window (25, 25) is pixel (50, 50), over the button.
TEST_CASE("GP-4 the UI router hit-tests the frame's framebuffer pixels", "[gp4][ui][input]") {
    ui::WidgetArena arena;
    const auto button = arena.create({.stable_id = "root/b", .type = "button", .rect_ui = {40, 40, 60, 60},
                                      .clip_ui = {0, 0, 100, 100}, .decl_seq = 1});
    const std::array traversal{button};
    const auto viewport = ui::ViewportTransform::letterboxed({100, 100}, 1);

    InputState input;
    input.setPointerTransform(pointerTransformFor(100, 100, 50, 50));
    input.queueEvents({InputEvent::cursorMove(25.0f, 25.0f), InputEvent::button(KeyCode::MouseLeft, true)});
    input.beginFrame();
    ui::InputRouter router;
    const auto routed = router.routeFrameInput(input.currentFrameInput(), arena, traversal, viewport);
    REQUIRE(routed.events.size() == 2);
    CHECK(routed.events[0].input.position_px == ui::PointI{50, 50});
    CHECK(routed.events[1].target == button);
    CHECK(routed.events[1].effects == std::vector{ui::PointerEffect::Capture});
    CHECK(routed.consumed_pointer);
}

namespace {

// A box a camera looks at, the camera off the origin and turned: it sits at
// (4, 3, -2) looking along (1, 0, 1) / sqrt(2), and the box is 10 ahead.
constexpr auto screenRaySceneDocument = R"({"schema":"pelican.scene","version":2,"scenes":{
  "main":{"objects":[{"name":"Target","components":[
    {"name":"transform","pos":[11.0710678,3.0,5.0710678],"rotation":[0,0,0,1],"scale":[1,1,1]},
    {"name":"collider","shape":"box","half_extents":[0.5,0.5,0.5]}]}]}}})";

struct ScreenRaySandbox {
    std::filesystem::path base = uniqueTemp("pelican_gp4_screen_ray");
    std::filesystem::path root = base / "project";
    ScreenRaySandbox() {
        std::filesystem::create_directories(root / "passes");
        std::ofstream{root / "scene.json", std::ios::binary} << screenRaySceneDocument;
        std::ofstream{root / "passes/main_rendering_config.json", std::ios::binary}
            << R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{}})";
    }
    ~ScreenRaySandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

void configureScreenRayProject(const ScreenRaySandbox &sandbox) {
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(
        Json{{"schema", "pelican.project"},
             {"version", 1},
             {"name", "gp4-screen-ray"},
             {"engine_min_version", "0.1.0"},
             {"basic_config",
              {{"window_size", {{"width", 100}, {"height", 100}}},
               {"camera", {{"yfov", 0.5}, {"znear", 0.1}, {"zfar", 1000.0}, {"up", {0.0, 1.0, 0.0}}}},
               {"default_scene_id", "main"},
               {"scene_data_json", "scene.json"}}}}
            .dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
}

} // namespace

#if PELICAN_WITH_PHYSICS
// Acceptance 2. The centre of the screen is the camera's line of sight and
// finds the box; the edges of a 0.5 rad field of view pass 2.5 units beside
// it. The camera is neither at the origin nor looking down an axis, so a ray
// that skipped the inverse view (it would leave the origin along -z) or the
// inverse projection (it would run backwards from the camera) misses.
TEST_CASE("GP-4 screenRay through the screen centre hits the collider in front of a moved and turned camera",
          "[gp4][screen-ray][physics]") {
    setupLogger();
    const ScreenRaySandbox sandbox;
    FastModuleContainer modules;
    configureScreenRayProject(sandbox);
    test_support::loadScene(GET_MODULE(SceneLoader), SceneId{"main"});

    auto &camera = GET_MODULE(Camera);
    const glm::vec3 eye{4.0f, 3.0f, -2.0f};
    const glm::vec3 forward = glm::normalize(glm::vec3{1.0f, 0.0f, 1.0f});
    camera.setPos(eye);
    camera.setDir(forward);
    camera.setUp({0.0f, 1.0f, 0.0f});
    REQUIRE(camera.viewportWidth() == 100);
    REQUIRE(camera.viewportHeight() == 100);

    const GameContext ctx;
    const auto centre = ctx.render().screenRay({50.0f, 50.0f});
    INFO(centre.errorName());
    REQUIRE(centre.ok());
    CHECK(centre.value().direction.x == Catch::Approx(forward.x).margin(1e-4));
    CHECK(centre.value().direction.y == Catch::Approx(forward.y).margin(1e-4));
    CHECK(centre.value().direction.z == Catch::Approx(forward.z).margin(1e-4));
    // The ray starts on the near plane, 0.1 in front of the eye.
    CHECK(centre.value().origin.x == Catch::Approx(eye.x + 0.1f * forward.x).margin(1e-4));
    CHECK(centre.value().origin.z == Catch::Approx(eye.z + 0.1f * forward.z).margin(1e-4));

    const auto hit = ctx.physics().raycastClosest(centre.value());
    REQUIRE(hit.has_value());
    CHECK(hit->position.x == Catch::Approx(11.07f).margin(0.75f));
    CHECK(hit->position.z == Catch::Approx(5.07f).margin(0.75f));

    for (const auto edge : {vec2{0.0f, 50.0f}, vec2{99.0f, 50.0f}, vec2{50.0f, 0.0f}, vec2{50.0f, 99.0f}}) {
        INFO("edge " << edge.x << "," << edge.y);
        const auto ray = ctx.render().screenRay(edge);
        REQUIRE(ray.ok());
        CHECK_FALSE(ctx.physics().raycastClosest(ray.value()).has_value());
    }
}
#endif

// Acceptance 3.
TEST_CASE("GP-4 screenRay is render.screen_ray_unavailable@1 while XR is active",
          "[gp4][screen-ray][xr]") {
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).xr_active = true;
    const GameContext ctx;
    const auto ray = ctx.render().screenRay({50.0f, 50.0f});
    REQUIRE_FALSE(ray.ok());
    CHECK(ray.error() == RenderError::screen_ray_unavailable);
    CHECK(std::string{ray.errorName()} == "render.screen_ray_unavailable@1");
    CHECK(ray.get() == nullptr);
    REQUIRE_THROWS_WITH(ray.value(), Catch::Matchers::StartsWith("render.screen_ray_unavailable@1"));

    const auto invalid = ctx.render().screenRay({std::numeric_limits<float>::quiet_NaN(), 0.0f});
    REQUIRE_FALSE(invalid.ok());
    CHECK(std::string{invalid.errorName()} == "render.screen_position_invalid@1");
}

TEST_CASE("GP-4 setCursorMode is recorded for the window and an embedded window never locks",
          "[gp4][cursor]") {
    CHECK(effectiveCursorMode(CursorMode::locked, false) == CursorMode::locked);
    CHECK(effectiveCursorMode(CursorMode::locked, true) == CursorMode::normal);
    CHECK(effectiveCursorMode(CursorMode::hidden, true) == CursorMode::hidden);
    CHECK(effectiveCursorMode(CursorMode::normal, true) == CursorMode::normal);
    CHECK(std::string{cursorModeName(CursorMode::locked)} == "locked");

    FastModuleContainer modules;
    const GameContext ctx;
    auto &input = GET_MODULE(InputState);
    CHECK(input.cursorMode().requested == CursorMode::normal);
    ctx.input().setCursorMode(CursorMode::locked);
    CHECK(input.cursorMode().requested == CursorMode::locked);
    // Clearing the input (every headless frame does) keeps the request.
    input.clear();
    CHECK(input.cursorMode().requested == CursorMode::locked);
}

} // namespace Pelican
