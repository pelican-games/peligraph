#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/playmode.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/userpublic/gameobjects.hpp"
#include "../src/core/userpublic/userinput.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/project/inputactionoverlay.hpp"
#include "../src/project/projecttemplate.hpp"
#include "fixtures/pm1b_edit/api.hpp"
#include "vulkan_test_support.hpp"

#include <Windows.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// PM-1b. The edit state over the headless RPC, the way Studio drives it: an
// engine launched with --start-state edit loads its scene with dormant
// behaviors, set_play_state starts and ends a play session, set_view_camera
// switches the view, and the editor's input overlays leave the action set
// stack while the game plays. The behaviors are a real game DLL's
// (fixtures/pm1b_edit), built against the SDK this test executable exports.

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

Json transform(double x) {
    return Json{{"name", "transform"},
                {"pos", {x, 0.0, 0.0}},
                {"rotation", {0.0, 0.0, 0.0, 1.0}},
                {"scale", {1.0, 1.0, 1.0}}};
}

// The blank template, with a mover that spawns a spark on its first update,
// a plain object to attach behaviors to, and a game action ("jump") on the
// key the editor transform overlay also binds (G).
std::filesystem::path makeProject(const std::filesystem::path &dir) {
    const auto root = dir / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    auto project = Json::parse(readFile(root / "project.json"));
    project["prefabs"] = Json::array({{{"name", "pm1b_spark"}, {"path", "prefabs/pm1b_spark.json"}}});
    writeFile(root / "project.json", project.dump(2));
    writeFile(root / "prefabs/pm1b_spark.json",
              Json{{"schema", "pelican.prefab"},
                   {"version", 1},
                   {"name", "pm1b_spark"},
                   {"components", Json::array({{{"name", "behavior"}, {"type", "pm1b_spark"}}})}}
                  .dump(2));

    auto scene = Json::parse(readFile(root / "scenes/main.scene.json"));
    auto &objects = scene.at("scenes").at("default_scene").at("objects");
    objects.push_back({{"name", "Mover"},
                       {"components",
                        Json::array({transform(0.0),
                                     {{"name", "behavior"},
                                      {"type", "pm1b_mover"},
                                      {"params", {{"step", 2}, {"spawn", true}}}}})}});
    objects.push_back({{"name", "Other"}, {"components", Json::array({transform(3.0)})}});
    writeFile(root / "scenes/main.scene.json", scene.dump(2));

    auto actions = Json::parse(readFile(root / "input/actions.json"));
    actions.at("action_sets").at(0).at("actions").push_back({{"name", "jump"}, {"type", "button"}});
    writeFile(root / "input/actions.json", actions.dump(2));
    auto profile = Json::parse(readFile(root / "input/profiles/keyboard.json"));
    profile.at("bindings").push_back({{"action", "jump"}, {"binding", "kbd:g"}});
    writeFile(root / "input/profiles/keyboard.json", profile.dump(2));
    return root;
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 9500;

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
    Json play() { return result("get_status").at("play"); }
    // One edit, committed by the frame after it.
    Json edit(std::uint64_t actor, Json operations) {
        const auto revision = result("get_scene_revision").at("scene_revision");
        const auto accepted = result("edit", {{"actor_id", actor},
                                              {"base_revision", revision},
                                              {"operations", std::move(operations)}});
        INFO(accepted.dump(2));
        REQUIRE(accepted.at("status") == "accepted");
        (void)result("step_frame");
        const auto committed = result("get_edit_result", {{"ticket", accepted.at("ticket")}});
        INFO(committed.dump(2));
        REQUIRE(committed.at("status") == "committed");
        return committed;
    }
    Json revert(std::string_view method, std::uint64_t actor) {
        const auto revision = result("get_scene_revision").at("scene_revision");
        const auto accepted = result(method, {{"actor_id", actor}, {"base_revision", revision}});
        INFO(accepted.dump(2));
        REQUIRE(accepted.at("status") == "accepted");
        (void)result("step_frame");
        const auto committed = result("get_edit_result", {{"ticket", accepted.at("ticket")}});
        INFO(committed.dump(2));
        REQUIRE(committed.at("status") == "committed");
        return committed;
    }
    void key(std::string_view type, std::string_view key) {
        (void)result("inject_input", {{"events", Json::array({{{"type", type}, {"key", key}}})}});
    }
};

std::string dataCode(const Json &error) {
    if (!error.contains("data") || !error.at("data").is_object() || !error.at("data").contains("code"))
        return {};
    return error.at("data").at("code").get<std::string>();
}

Pm1bFixture::Counts counts() {
    const auto status = configuredGameLogicStatus();
    REQUIRE(status.loaded);
    auto module = GetModuleHandleW(status.loaded_copy.c_str());
    if (module == nullptr) module = GetModuleHandleW(status.loaded_copy.filename().c_str());
    REQUIRE(module != nullptr);
    const auto read = reinterpret_cast<PelicanPm1bCountsFn>(GetProcAddress(module, "pelican_pm1b_counts"));
    REQUIRE(read != nullptr);
    Pm1bFixture::Counts result;
    read(&result);
    return result;
}

std::vector<BehaviorAttachmentInfo> attachments() {
    return GET_MODULE(BehaviorAttachmentArena).snapshot();
}

std::size_t spawnedAttachments() {
    const auto all = attachments();
    return static_cast<std::size_t>(std::count_if(all.begin(), all.end(), [](const auto &info) {
        return info.runtime_spawned;
    }));
}

float moverX() {
    const auto &loader = GET_MODULE(SceneLoader);
    const auto mover = loader.objectId("Mover");
    REQUIRE(mover);
    return GameObjects::localTransform(*mover).pos.x;
}

bool editorOverlaysStacked() {
    const auto sets = internal::editorInputOverlayActionSets();
    REQUIRE_FALSE(sets.empty());
    const auto stack = Actions::actionSetStack();
    const auto on = std::count_if(sets.begin(), sets.end(), [&](const std::string &name) {
        return std::find(stack.begin(), stack.end(), name) != stack.end();
    });
    REQUIRE((on == 0 || static_cast<std::size_t>(on) == sets.size()));
    return on != 0;
}

std::uint64_t objectId(Rpc &rpc, std::string_view name) {
    const auto tree = rpc.result("scene_tree");
    for (const auto &object : tree.at("objects"))
        if (object.value("name", std::string{}) == name)
            return object.at("authoring_object_id").get<std::uint64_t>();
    FAIL("scene_tree has no object named " << name);
    return 0;
}

// A --start-state edit launch, with the editor transform overlay Studio
// passes as well, and the fixture DLL as the project's game logic.
struct EditLaunch {
    TempDir dir{std::filesystem::temp_directory_path() /
                ("pelican_pm1b_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
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
        launch.input_action_overlays.emplace_back(editorTransformBlenderInputActionOverlayReference);
        internal::completeEditStartLaunch(launch);
        REQUIRE(launch.free_camera);
        launch.game_logic_dll = std::filesystem::path{PELICAN_PM1B_EDIT_FIXTURE};
        GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
        TestSupport::requireVulkanDevice("PM-1b edit state over RPC");
        GET_MODULE(ECSPredefinedRegistration).reg();
        if (!initializeConfiguredGameLogic()) {
            FAIL("PM-1b fixture failed to load: " << configuredGameLogicStatus().last_error);
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

// PM-1b acceptance 1, 3, 4 and 6: the edit state runs no game code, a session
// wakes the behaviors and a stop puts them to sleep again, a session can start
// paused, and the view camera and the editor overlays follow the state.
TEST_CASE("PM-1b an engine launched in edit runs no behavior until a play session, and stopping ends it",
          "[pm1b][play-state][rpc][gpu]") {
    EditLaunch launch;
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");

    // Acceptance 1. Loaded in edit: the attachment is there, dormant -- not
    // pending, not active -- and no constructor, onInit or onUpdate ran.
    auto play = rpc.play();
    INFO(play.dump());
    REQUIRE(play.at("state") == "edit");
    REQUIRE(play.at("view_camera") == "editor");
    const auto dormant = attachments();
    REQUIRE(dormant.size() == 1U);
    REQUIRE(dormant.front().stable_name == "pm1b_mover");
    REQUIRE_FALSE(dormant.front().pending);
    REQUIRE_FALSE(dormant.front().active);
    // load_scene in edit makes the edit runtime again, and scene_time is the
    // game clock's time there: the runtime starts at 0 whatever the clock.
    (void)rpc.result("set_time", {{"t", 1.5}});
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    REQUIRE(GET_MODULE(SceneLoader).sceneStartTime() == 0.0);
    REQUIRE(attachments().size() == 1U);
    REQUIRE_FALSE(attachments().front().active);
    for (int frame = 0; frame < 4; ++frame) (void)rpc.result("step_frame");
    auto seen = counts();
    REQUIRE(seen.constructed == 0U);
    REQUIRE(seen.inits == 0U);
    REQUIRE(seen.updates == 0U);
    REQUIRE(moverX() == 0.0f);
    play = rpc.play();
    REQUIRE(play.at("game_time").get<double>() == Catch::Approx(1.5));
    REQUIRE(play.at("game_frame") == 0);
    // Steps are a paused game's.
    const auto step_in_edit = rpc.error("advance_frames", {{"count", 1}});
    REQUIRE(dataCode(step_in_edit) == "play_state_mismatch");
    REQUIRE(step_in_edit.at("data").at("state") == "edit");

    // Acceptance 6. In edit: the editor camera is the view, set_view_camera
    // switches it, and the editor's overlay takes G before the game does.
    auto &camera = GET_MODULE(Camera);
    const auto editor_camera = camera.editorCameraName();
    REQUIRE_FALSE(editor_camera.empty());
    REQUIRE(camera.activeCameraName() == editor_camera);
    REQUIRE(rpc.result("set_view_camera", {{"camera", "game"}}) ==
            Json{{"camera", "game"}, {"previous", "editor"}});
    REQUIRE(camera.activeCameraName() != editor_camera);
    REQUIRE(rpc.play().at("view_camera") == "game");
    REQUIRE(rpc.result("set_view_camera", {{"camera", "editor"}}) ==
            Json{{"camera", "editor"}, {"previous", "game"}});
    REQUIRE(camera.activeCameraName() == editor_camera);
    REQUIRE(rpc.error("set_view_camera", {{"camera", "director"}}).at("code") == -32602);
    REQUIRE(editorOverlaysStacked());
    rpc.key("key_down", "g");
    (void)rpc.result("step_frame");
    REQUIRE(Actions::isHeld("gizmo.translate"));
    REQUIRE_FALSE(Actions::isHeld("jump"));
    rpc.key("key_up", "g");
    (void)rpc.result("step_frame");

    // Acceptance 3. Playing: the runtime is made again from the authoring,
    // the behavior is constructed and runs onInit, the clock starts at 0,
    // and the object moves.
    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}) ==
            Json{{"state", "playing"}, {"previous", "edit"}});
    seen = counts();
    REQUIRE(seen.constructed == 1U);
    REQUIRE(seen.inits == 1U);
    REQUIRE(seen.updates == 0U);
    play = rpc.play();
    INFO(play.dump());
    REQUIRE(play.at("state") == "playing");
    REQUIRE(play.at("game_frame") == 0);
    REQUIRE(play.at("game_time").get<double>() == 0.0);
    REQUIRE(play.at("view_camera") == "game");
    REQUIRE(camera.activeCameraName() != editor_camera);
    REQUIRE(GET_MODULE(SceneLoader).sceneStartTime() == 0.0);
    REQUIRE(attachments().front().active);
    // Playing, the view is the game's.
    const auto view_while_playing = rpc.error("set_view_camera", {{"camera", "editor"}});
    REQUIRE(dataCode(view_while_playing) == "play_state_mismatch");
    REQUIRE(view_while_playing.at("data").at("state") == "playing");
    // The editor overlays are off the stack: G reaches the game's action.
    REQUIRE_FALSE(editorOverlaysStacked());
    rpc.key("key_down", "g");
    (void)rpc.result("step_frame");
    REQUIRE(Actions::isHeld("jump"));
    rpc.key("key_up", "g");
    for (int frame = 0; frame < 2; ++frame) (void)rpc.result("step_frame");
    seen = counts();
    REQUIRE(seen.updates == 3U);
    REQUIRE(moverX() == Catch::Approx(3.0f));
    // The first update asked for a spark; it landed at the next boundary.
    REQUIRE(seen.spawns == 1U);
    REQUIRE(seen.spawn_errors == 0U);
    REQUIRE(seen.spark_inits == 1U);
    REQUIRE(spawnedAttachments() == 1U);
    REQUIRE(rpc.play().at("game_frame") == 3);

    // Stop: onDestroy runs, the spark goes with its run, the behavior is
    // dormant again, the object is where the authoring has it, and the clock
    // is back at the time it had before the session.
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}) ==
            Json{{"state", "edit"}, {"previous", "playing"}});
    seen = counts();
    REQUIRE(seen.destroys == 1U);
    REQUIRE(seen.spark_destroys == 1U);
    REQUIRE(spawnedAttachments() == 0U);
    REQUIRE(attachments().size() == 1U);
    REQUIRE_FALSE(attachments().front().active);
    REQUIRE_FALSE(attachments().front().pending);
    REQUIRE(moverX() == 0.0f);
    play = rpc.play();
    REQUIRE(play.at("state") == "edit");
    REQUIRE(play.at("view_camera") == "editor");
    REQUIRE(play.at("game_time").get<double>() == Catch::Approx(1.5));
    REQUIRE(camera.activeCameraName() == editor_camera);
    REQUIRE(editorOverlaysStacked());
    for (int frame = 0; frame < 3; ++frame) (void)rpc.result("step_frame");
    REQUIRE(counts().updates == seen.updates);
    REQUIRE(counts().constructed == 1U);
    // The same state again changes nothing.
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}) ==
            Json{{"state", "edit"}, {"previous", "edit"}});

    // Acceptance 4. Into paused: onInit runs and the game stands at frame 0;
    // advance_frames takes it one step.
    REQUIRE(rpc.result("set_play_state", {{"state", "paused"}}) ==
            Json{{"state", "paused"}, {"previous", "edit"}});
    seen = counts();
    REQUIRE(seen.constructed == 2U);
    REQUIRE(seen.inits == 2U);
    play = rpc.play();
    REQUIRE(play.at("state") == "paused");
    REQUIRE(play.at("game_frame") == 0);
    REQUIRE(play.at("view_camera") == "game");
    (void)rpc.result("step_frame");
    REQUIRE(counts().updates == seen.updates);
    const auto advanced = rpc.result("advance_frames", {{"count", 1}});
    REQUIRE(advanced.at("until_game_frame") == 1);
    REQUIRE(counts().updates == seen.updates + 1U);
    REQUIRE(rpc.play().at("game_frame") == 1);
    REQUIRE(moverX() == Catch::Approx(1.0f));
    // Paused, the editor camera can be looked through, and the overlays are
    // the editor's again.
    REQUIRE(editorOverlaysStacked());
    REQUIRE(rpc.result("set_view_camera", {{"camera", "editor"}}).at("camera") == "editor");
    REQUIRE(camera.activeCameraName() == editor_camera);
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}).at("previous") == "paused");
    REQUIRE(counts().destroys == 2U);
    REQUIRE(rpc.play().at("view_camera") == "editor");

    // What a session would keep is PM-2's; refused with nothing changed.
    REQUIRE(rpc.error("set_play_state", {{"state", "playing"}, {"keep", "all"}}).at("code") == -32602);
    REQUIRE(rpc.play().at("state") == "edit");
    REQUIRE(rpc.error("set_play_state", {{"state", "stopped"}}).at("code") == -32602);
}

// PM-1b acceptance 2 and 5: in edit, behavior edits reach the authoring and
// the dormant attachment only -- set_params, attach, undo and redo run no
// constructor -- and a game logic reload leaves the behaviors dormant.
TEST_CASE("PM-1b behavior edits and a game logic reload in edit leave the behaviors dormant",
          "[pm1b][play-state][rpc][game-logic][gpu]") {
    EditLaunch launch;
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");
    const auto actor = rpc.result("open_editor_session", {{"display_name", "PM-1b"}})
                           .at("actor_id")
                           .get<std::uint64_t>();
    const auto mover_id = objectId(rpc, "Mover");
    const auto other_id = objectId(rpc, "Other");
    const auto mover_entity = GET_MODULE(SceneLoader).objectId("Mover");
    REQUIRE(mover_entity);
    const auto mover_params = [&] {
        for (const auto &info : attachments())
            if (info.entity == *mover_entity) return Json::parse(info.canonical_params);
        FAIL("Mover has no behavior attachment");
        return Json{};
    };
    REQUIRE(mover_params().at("step") == 2);

    // set_params: the stored params change, no instance is made.
    (void)rpc.edit(actor, Json::array({{{"op", "set_component_value"},
                                        {"object_id", mover_id},
                                        {"component_slot", "behavior"},
                                        {"attachment_index", 1},
                                        {"field_path", "/params/step"},
                                        {"value", 6}}}));
    REQUIRE(mover_params().at("step") == 6);
    // Attach: a second dormant attachment, no constructor.
    (void)rpc.edit(actor, Json::array({{{"op", "add_component"},
                                        {"object_id", other_id},
                                        {"component",
                                         {{"name", "behavior"},
                                          {"type", "pm1b_mover"},
                                          {"params", {{"step", 4}}}}}}}));
    auto all = attachments();
    REQUIRE(all.size() == 2U);
    for (const auto &info : all) {
        REQUIRE_FALSE(info.pending);
        REQUIRE_FALSE(info.active);
    }
    REQUIRE(counts().constructed == 0U);

    // Undo and redo, through the journal's rebind to the dormant attachments.
    (void)rpc.revert("undo", actor);
    REQUIRE(attachments().size() == 1U);
    (void)rpc.revert("undo", actor);
    REQUIRE(mover_params().at("step") == 2);
    (void)rpc.revert("redo", actor);
    REQUIRE(mover_params().at("step") == 6);
    (void)rpc.revert("redo", actor);
    REQUIRE(attachments().size() == 2U);
    auto seen = counts();
    REQUIRE(seen.constructed == 0U);
    REQUIRE(seen.inits == 0U);
    REQUIRE(seen.updates == 0U);

    // Acceptance 5. A reload in edit makes the edit runtime again: the new
    // copy of the DLL constructs nothing and the behaviors stay dormant.
    const auto reloaded = rpc.result("reload_game_logic");
    INFO(reloaded.dump());
    REQUIRE(reloaded.at("generation").get<std::uint64_t>() >= 2U);
    REQUIRE(rpc.play().at("state") == "edit");
    all = attachments();
    REQUIRE(all.size() == 2U);
    for (const auto &info : all) REQUIRE_FALSE(info.active);
    for (int frame = 0; frame < 2; ++frame) (void)rpc.result("step_frame");
    seen = counts();
    REQUIRE(seen.constructed == 0U);
    REQUIRE(seen.inits == 0U);
    REQUIRE(seen.updates == 0U);
    REQUIRE(moverX() == 0.0f);

    // The session starts from the edited authoring: both movers wake, and
    // the edited step moves Mover by 3 per update.
    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}).at("previous") == "edit");
    seen = counts();
    REQUIRE(seen.constructed == 2U);
    REQUIRE(seen.inits == 2U);
    (void)rpc.result("step_frame");
    REQUIRE(moverX() == Catch::Approx(3.0f));
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}).at("previous") == "playing");
    REQUIRE(counts().destroys == 2U);
}

} // namespace Pelican
