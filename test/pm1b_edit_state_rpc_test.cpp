#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/playmode.hpp"
#if PELICAN_WITH_AUDIO
#include "../src/core/audio/audio.hpp"
#endif
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/authoringsceneauthority.hpp"
#include "../src/core/loader/basicconfig.hpp"
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
#include <functional>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// PM-1b. The edit state over the headless RPC, the way Studio drives it: an
// engine launched with --start-state edit loads its scene with dormant
// behaviors, set_play_state starts and ends a play session, set_view_camera
// switches the view, and the editor's input overlays leave the action set
// stack while the game plays. The behaviors are a real game DLL's
// (fixtures/pm1b_edit), built against the SDK this test executable exports.
//
// PM-2. The play session over the same launch: what stopping puts back
// (the authoring, the history, the first scene, silence), what `keep` carries
// past it, the record of the edits made while playing, and the refusals.

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
// passes as well, and the fixture DLL as the project's game logic. `adjust`
// changes the project before the engine reads it.
using ProjectAdjustment = std::function<void(const std::filesystem::path &root)>;

struct EditLaunch {
    TempDir dir{std::filesystem::temp_directory_path() /
                ("pelican_pm1b_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    std::filesystem::path root;
    FastModuleContainer modules;

    explicit EditLaunch(const ProjectAdjustment &adjust = {}) : root{makeProject(dir.root)} {
        if (adjust) adjust(root);
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
    // advance_frames takes it one step. (PM-2: every session starts from a
    // fresh copy of the DLL, so its counts start over.)
    REQUIRE(rpc.result("set_play_state", {{"state", "paused"}}) ==
            Json{{"state", "paused"}, {"previous", "edit"}});
    seen = counts();
    REQUIRE(seen.constructed == 1U);
    REQUIRE(seen.inits == 1U);
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
    REQUIRE(counts().destroys == 1U);
    REQUIRE(rpc.play().at("view_camera") == "editor");

    // `keep` belongs to stopping; refused with nothing changed.
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

namespace {

// PM-2 helpers.

std::string persistentAuthoring() {
    return AuthoringSceneAuthority::encodePersistent(GET_MODULE(ProjectBasicConfig).sceneDocument());
}

std::string semanticAuthoring() { return GET_MODULE(ProjectBasicConfig).sceneDataJson(); }

std::uint64_t sceneRevision() { return GET_MODULE(ProjectBasicConfig).sceneDocument().revision().value; }

Json setTransform(std::uint64_t object, double x) {
    return Json::array({{{"op", "set_component_value"},
                         {"object_id", object},
                         {"component_slot", "transform"},
                         {"field_path", "/pos"},
                         {"value", {x, 0.0, 0.0}}}});
}

Json setStep(std::uint64_t object, int step) {
    return Json::array({{{"op", "set_component_value"},
                         {"object_id", object},
                         {"component_slot", "behavior"},
                         {"attachment_index", 1},
                         {"field_path", "/params/step"},
                         {"value", step}}});
}

Json recordedEntry(const Json &committed, std::uint64_t actor) {
    return Json{{"transaction_id", committed.at("transaction_id")}, {"actor", actor}};
}

bool hasObject(Rpc &rpc, std::string_view name) {
    const auto tree = rpc.result("scene_tree");
    for (const auto &object : tree.at("objects"))
        if (object.value("name", std::string{}) == name) return true;
    return false;
}

// An object's x as its runtime transform has it, read through scene_tree
// (an object the editor made has no name binding in the scene loader).
float runtimeX(Rpc &rpc, std::string_view name) {
    const auto tree = rpc.result("scene_tree");
    for (const auto &object : tree.at("objects")) {
        if (object.value("name", std::string{}) != name) continue;
        REQUIRE(object.contains("entity_id"));
        for (const auto &component : object.at("components"))
            if (component.at("name") == "transform")
                return component.at("runtime_json").at("local_trs").at("pos").at(0).get<float>();
    }
    FAIL("scene_tree has no object named " << name << " with a transform");
    return 0.0f;
}

float objectX(std::string_view name) {
    const auto object = GET_MODULE(SceneLoader).objectId(std::string{name});
    REQUIRE(object);
    return GameObjects::localTransform(*object).pos.x;
}

Json moverParams(std::string_view name) {
    const auto entity = GET_MODULE(SceneLoader).objectId(std::string{name});
    REQUIRE(entity);
    for (const auto &info : attachments())
        if (info.entity == *entity) return Json::parse(info.canonical_params);
    FAIL(name << " has no behavior attachment");
    return Json{};
}

#if PELICAN_WITH_AUDIO
void writeU16(std::ofstream &file, std::uint16_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
}

void writeU32(std::ofstream &file, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) file.put(static_cast<char>((value >> shift) & 0xFF));
}

// One second of a quiet square wave, 16-bit mono PCM at 8 kHz.
void writeToneWav(const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    constexpr std::uint32_t sample_rate = 8000;
    constexpr std::uint32_t samples = sample_rate;
    constexpr std::uint32_t data_size = samples * 2U;
    file.write("RIFF", 4);
    writeU32(file, 36 + data_size);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    writeU32(file, 16);
    writeU16(file, 1);
    writeU16(file, 1);
    writeU32(file, sample_rate);
    writeU32(file, sample_rate * 2U);
    writeU16(file, 2);
    writeU16(file, 16);
    file.write("data", 4);
    writeU32(file, data_size);
    for (std::uint32_t index = 0; index < samples; ++index)
        writeU16(file, static_cast<std::uint16_t>((index / 20U) % 2U == 0U ? 512 : -512));
}
#endif

} // namespace

// PM-2 acceptance 1, 3 and 4: a session keeps the authoring and the history
// as they were when it began, and stopping puts both back under a new
// revision with the same ids; the edits made while playing are recorded
// (undo takes one out, redo puts it back); a scene edit in flight refuses
// Play and Stop, and saving, loading or importing a scene waits for the stop.
TEST_CASE("PM-2 stopping a play session puts the authoring and the editor history back",
          "[pm2][play-state][rpc][gpu]") {
    EditLaunch launch;
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");
    const auto actor = rpc.result("open_editor_session", {{"display_name", "PM-2"}})
                           .at("actor_id")
                           .get<std::uint64_t>();
    const auto mover_id = objectId(rpc, "Mover");
    const auto other_id = objectId(rpc, "Other");

    // An edit made before the session, for the undo after it.
    const auto before_session = rpc.edit(actor, setTransform(other_id, 4.0));
    REQUIRE(objectX("Other") == Catch::Approx(4.0f));
    const auto authoring = persistentAuthoring();
    const auto revision = sceneRevision();
    auto play = rpc.play();
    INFO(play.dump());
    REQUIRE(play.at("session") == false);
    REQUIRE(play.at("recorded_edits") == Json::array());

    // Acceptance 4. A live preview in flight refuses Play: while its request
    // waits for the boundary, and once its lease is open. Nothing changes.
    const auto preview = rpc.result("open_preview", {{"actor_id", actor}, {"operations", setTransform(other_id, 8.0)}});
    REQUIRE(preview.at("status") == "accepted");
    auto busy = rpc.error("set_play_state", {{"state", "playing"}});
    INFO(busy.dump());
    REQUIRE(dataCode(busy) == "play_busy");
    REQUIRE(busy.at("data").at("pending_ticket_ids").size() == 1U);
    (void)rpc.result("step_frame");
    busy = rpc.error("set_play_state", {{"state", "playing"}});
    REQUIRE(dataCode(busy) == "play_busy");
    REQUIRE(busy.at("data").at("open_preview_lease") == true);
    REQUIRE(rpc.play().at("state") == "edit");
    REQUIRE(rpc.play().at("session") == false);
    REQUIRE(counts().constructed == 0U);
    (void)rpc.result("abort_preview", {{"actor_id", actor}, {"ticket", preview.at("ticket")}});
    (void)rpc.result("step_frame");
    REQUIRE(persistentAuthoring() == authoring);
    REQUIRE(sceneRevision() == revision);

    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}) ==
            Json{{"state", "playing"}, {"previous", "edit"}});
    play = rpc.play();
    REQUIRE(play.at("session") == true);
    REQUIRE(play.at("recorded_edits") == Json::array());

    // Acceptance 1. Inspector edits while playing: a transform and a
    // behavior's value. Both reach the authoring and are recorded.
    const auto moved = rpc.edit(actor, setTransform(other_id, 9.0));
    const auto stepped = rpc.edit(actor, setStep(mover_id, 6));
    REQUIRE(objectX("Other") == Catch::Approx(9.0f));
    REQUIRE(moverParams("Mover").at("step") == 6);
    REQUIRE(rpc.play().at("recorded_edits") ==
            Json::array({recordedEntry(moved, actor), recordedEntry(stepped, actor)}));

    // Acceptance 3. Undo takes the edit out of the record; redo puts it back,
    // under the redo's own transaction.
    (void)rpc.revert("undo", actor);
    REQUIRE(rpc.play().at("recorded_edits") == Json::array({recordedEntry(moved, actor)}));
    const auto redone = rpc.revert("redo", actor);
    REQUIRE(rpc.play().at("recorded_edits") ==
            Json::array({recordedEntry(moved, actor), recordedEntry(redone, actor)}));
    REQUIRE(moverParams("Mover").at("step") == 6);

    // Acceptance 4. What would replace or write the scene waits for the stop,
    // and a keep naming an edit the session did not record refuses the stop.
    // Nothing changes.
    const auto during = persistentAuthoring();
    const auto during_revision = sceneRevision();
    REQUIRE(during_revision > revision);
    for (const auto &[method, params] : std::vector<std::pair<std::string, Json>>{
             {"save_scene", Json::object()},
             {"load_scene", Json{{"name", "default_scene"}}},
             {"import_scene_snapshot", Json{{"schema_version", 2}}}}) {
        const auto refused = rpc.error(method, params);
        INFO(refused.dump());
        REQUIRE(dataCode(refused) == "play_session_active");
        REQUIRE(refused.at("data").at("method") == method);
        REQUIRE(refused.at("data").at("state") == "playing");
    }
    auto unknown = rpc.error("set_play_state", {{"state", "edit"},
                                                {"keep", Json::array({moved.at("transaction_id"), "txn-999999"})}});
    INFO(unknown.dump());
    REQUIRE(dataCode(unknown) == "play_keep_unknown_transaction");
    REQUIRE(unknown.at("data").at("transaction_id") == "txn-999999");
    // The undone edit is not recorded any more.
    unknown = rpc.error("set_play_state", {{"state", "edit"}, {"keep", Json::array({stepped.at("transaction_id")})}});
    REQUIRE(dataCode(unknown) == "play_keep_unknown_transaction");
    REQUIRE(rpc.play().at("state") == "playing");
    REQUIRE(rpc.play().at("session") == true);
    REQUIRE(persistentAuthoring() == during);
    REQUIRE(sceneRevision() == during_revision);

    // A preview in flight refuses the stop too.
    const auto late_preview =
        rpc.result("open_preview", {{"actor_id", actor}, {"operations", setTransform(other_id, 11.0)}});
    (void)rpc.result("step_frame");
    busy = rpc.error("set_play_state", {{"state", "edit"}});
    REQUIRE(dataCode(busy) == "play_busy");
    REQUIRE(rpc.play().at("state") == "playing");
    (void)rpc.result("abort_preview", {{"actor_id", actor}, {"ticket", late_preview.at("ticket")}});
    (void)rpc.result("step_frame");
    REQUIRE(persistentAuthoring() == during);

    // Stop: the authoring is what it was before the session -- the same
    // content and ids -- under a revision later than any seen, and the
    // runtime is the dormant edit one.
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}) ==
            Json{{"state", "edit"}, {"previous", "playing"}});
    REQUIRE(persistentAuthoring() == authoring);
    REQUIRE(sceneRevision() > during_revision);
    REQUIRE(objectId(rpc, "Mover") == mover_id);
    REQUIRE(objectId(rpc, "Other") == other_id);
    REQUIRE(objectX("Other") == Catch::Approx(4.0f));
    REQUIRE(moverParams("Mover").at("step") == 2);
    const auto dormant = attachments();
    REQUIRE(dormant.size() == 1U);
    REQUIRE_FALSE(dormant.front().active);
    REQUIRE_FALSE(dormant.front().pending);
    play = rpc.play();
    REQUIRE(play.at("session") == false);
    REQUIRE(play.at("recorded_edits") == Json::array());
    REQUIRE(play.at("view_camera") == "editor");

    // The history is as it was too: the next undo is the edit made before
    // the session, and it applies.
    const auto undone_before = rpc.revert("undo", actor);
    REQUIRE(undone_before.at("source_transaction_id") == before_session.at("transaction_id"));
    REQUIRE(objectX("Other") == Catch::Approx(3.0f));
}

// PM-2 acceptance 2 and 3: an object made while playing goes with the stop
// unless its edit is kept, when it stays under the same id; the kept edits
// are one history entry that one undo takes out; an edit to an object whose
// making is not kept cannot be applied, is reported, and the stop succeeds.
TEST_CASE("PM-2 keep carries chosen play-session edits past the stop as one history entry",
          "[pm2][play-state][rpc][gpu]") {
    EditLaunch launch;
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");
    const auto actor = rpc.result("open_editor_session", {{"display_name", "PM-2"}})
                           .at("actor_id")
                           .get<std::uint64_t>();
    const auto spawn = Json::array({{{"op", "spawn"},
                                     {"scene_id", "default_scene"},
                                     {"object", {{"name", "Made"}, {"components", Json::array({transform(5.0)})}}}}});
    const auto semantic = semanticAuthoring();

    // Not kept: gone.
    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}).at("previous") == "edit");
    (void)rpc.edit(actor, spawn);
    const auto first_id = objectId(rpc, "Made");
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}) ==
            Json{{"state", "edit"}, {"previous", "playing"}});
    REQUIRE_FALSE(hasObject(rpc, "Made"));
    REQUIRE(semanticAuthoring() == semantic);

    // Kept with an edit to it: it stays under the id it was made with, and the
    // id is a new one (numbers never go back).
    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}).at("previous") == "edit");
    const auto made = rpc.edit(actor, spawn);
    const auto made_id = objectId(rpc, "Made");
    REQUIRE(made_id > first_id);
    const auto moved = rpc.edit(actor, setTransform(made_id, 6.0));
    const auto kept = rpc.result(
        "set_play_state",
        {{"state", "edit"}, {"keep", Json::array({made.at("transaction_id"), moved.at("transaction_id")})}});
    INFO(kept.dump());
    REQUIRE(kept.at("state") == "edit");
    REQUIRE(kept.at("previous") == "playing");
    REQUIRE(kept.at("not_kept") == Json::array());
    REQUIRE(kept.contains("kept_transaction_id"));
    REQUIRE(objectId(rpc, "Made") == made_id);
    REQUIRE(runtimeX(rpc, "Made") == Catch::Approx(6.0f));
    REQUIRE(rpc.play().at("session") == false);

    // One undo takes both kept edits out.
    const auto undone = rpc.revert("undo", actor);
    REQUIRE(undone.at("source_transaction_id") == kept.at("kept_transaction_id"));
    REQUIRE_FALSE(hasObject(rpc, "Made"));
    REQUIRE(semanticAuthoring() == semantic);

    // Keeping the edit to an object whose making is not kept: it does not
    // apply, it is reported, and the stop succeeds with the object gone.
    REQUIRE(rpc.result("set_play_state", {{"state", "paused"}}).at("previous") == "edit");
    (void)rpc.edit(actor, spawn);
    const auto touched = rpc.edit(actor, setTransform(objectId(rpc, "Made"), 7.0));
    const auto partial =
        rpc.result("set_play_state", {{"state", "edit"}, {"keep", Json::array({touched.at("transaction_id")})}});
    INFO(partial.dump());
    REQUIRE(partial.at("state") == "edit");
    REQUIRE(partial.at("previous") == "paused");
    REQUIRE(partial.at("not_kept") ==
            Json::array({{{"transaction_id", touched.at("transaction_id")}, {"code", "closure_unresolvable"}}}));
    REQUIRE_FALSE(partial.contains("kept_transaction_id"));
    REQUIRE_FALSE(hasObject(rpc, "Made"));
    REQUIRE(semanticAuthoring() == semantic);

    // "all" keeps every recorded edit.
    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}).at("previous") == "edit");
    (void)rpc.edit(actor, spawn);
    const auto all_id = objectId(rpc, "Made");
    const auto everything = rpc.result("set_play_state", {{"state", "edit"}, {"keep", "all"}});
    REQUIRE(everything.at("not_kept") == Json::array());
    REQUIRE(objectId(rpc, "Made") == all_id);
    REQUIRE(attachments().size() == 1U);
    REQUIRE_FALSE(attachments().front().active);
}

// PM-2 acceptance 5, 6 and 7: every session starts from a fresh copy of the
// game's DLL (a game system's own count starts over), a scene the game loaded
// while playing gives way to the one the session began in, and the stop
// leaves no voice behind.
TEST_CASE("PM-2 each session starts a fresh game DLL, and stopping brings back the first scene and silence",
          "[pm2][play-state][rpc][game-logic][gpu]") {
    EditLaunch launch{[](const std::filesystem::path &root) {
        auto scene = Json::parse(readFile(root / "scenes/main.scene.json"));
        auto &scenes = scene.at("scenes");
        Json params{{"step", 0}, {"load_scene", "second_scene"}};
#if PELICAN_WITH_AUDIO
        writeToneWav(root / "sounds/tone.wav");
        params["sound"] = "project://sounds/tone.wav";
#endif
        scenes.at("default_scene")
            .at("objects")
            .push_back({{"name", "Loader"},
                        {"components",
                         Json::array({transform(-2.0), {{"name", "behavior"}, {"type", "pm1b_mover"}, {"params", params}}})}});
        scenes["second_scene"] = {
            {"objects",
             Json::array({{{"name", "SecondCamera"}, {"components", Json::array({transform(0.0), {{"name", "camera"}}})}},
                          {{"name", "Far"}, {"components", Json::array({transform(40.0)})}}})}};
        writeFile(root / "scenes/main.scene.json", scene.dump(2));
    }};
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");
    const auto authoring = persistentAuthoring();
    auto &loader = GET_MODULE(SceneLoader);

    // Acceptance 5 and 6. Playing: the system counts every frame, the Loader
    // asks for second_scene on its first update, and it plays a sound.
    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}).at("previous") == "edit");
    for (int frame = 0; frame < 3; ++frame) (void)rpc.result("step_frame");
    auto seen = counts();
    REQUIRE(seen.system_updates == 3U);
    REQUIRE(seen.scene_loads == 1U);
    REQUIRE(seen.scene_load_errors == 0U);
    REQUIRE(loader.currentScene() == "second_scene");
#if PELICAN_WITH_AUDIO
    // Acceptance 7. The sound is still there (nothing mixed it to its end).
    auto &audio = GET_MODULE(Audio);
    REQUIRE(seen.sounds == 1U);
    REQUIRE(seen.sound_errors == 0U);
    REQUIRE(audio.voiceCountForTesting() >= 1U);
    REQUIRE(audio.backendVoiceCountForTesting() >= 1U);
#endif

    // Stop: the scene the session began in, as it was, and no voice.
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}).at("previous") == "playing");
    REQUIRE(loader.currentScene() == "default_scene");
    REQUIRE(persistentAuthoring() == authoring);
    REQUIRE(hasObject(rpc, "Loader"));
    REQUIRE(hasObject(rpc, "Mover"));
    REQUIRE(attachments().size() == 2U);
#if PELICAN_WITH_AUDIO
    REQUIRE(audio.voiceCountForTesting() == 0U);
    REQUIRE(audio.backendVoiceCountForTesting() == 0U);
    for (int frame = 0; frame < 2; ++frame) (void)rpc.result("step_frame");
    REQUIRE(audio.voiceCountForTesting() == 0U);
#endif

    // Acceptance 5. The next session's DLL is a fresh copy: the system's own
    // count starts again from 1.
    REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}).at("previous") == "edit");
    (void)rpc.result("step_frame");
    REQUIRE(counts().system_updates == 1U);
    REQUIRE(rpc.result("set_play_state", {{"state", "edit"}}).at("previous") == "playing");
    REQUIRE(loader.currentScene() == "default_scene");
}

// PM-2: an engine launched with --start-state edit refuses the input record
// and replay in every state, as its command line refuses --record-input and
// --replay.
TEST_CASE("PM-2 an engine launched in edit refuses the input record and replay",
          "[pm2][play-state][rpc][gpu]") {
    EditLaunch launch;
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    const auto path = (launch.dir.root / "input.jsonl").generic_string();
    for (const auto *state : {"edit", "playing"}) {
        if (std::string_view{state} == "playing")
            REQUIRE(rpc.result("set_play_state", {{"state", "playing"}}).at("previous") == "edit");
        for (const auto *method : {"start_input_record", "start_input_replay"}) {
            const auto refused = rpc.error(method, {{"path", path}});
            INFO(refused.dump());
            REQUIRE(dataCode(refused) == "offline.mode_conflict@1");
            REQUIRE(refused.at("data").at("method") == method);
            REQUIRE(refused.at("data").at("start_state") == "edit");
        }
    }
    REQUIRE_FALSE(std::filesystem::exists(path));
}

} // namespace Pelican
