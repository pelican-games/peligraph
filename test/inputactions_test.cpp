#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/os/actionmap.hpp"
#include "../src/core/os/inputstate.hpp"
#include "../src/core/userpublic/userinput.hpp"

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

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "input_actions";
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
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

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

void requireErrorKind(std::string_view message, std::string_view error_kind) {
    if (error_kind == "type") {
        REQUIRE(contains(message, "type"));
    } else if (error_kind == "duplicate") {
        REQUIRE(contains(message, "duplicate"));
    } else if (error_kind == "binding") {
        REQUIRE((contains(message, "binding") || contains(message, "axis2 action")));
    } else if (error_kind == "openxr") {
        REQUIRE(contains(message, "OpenXR"));
    } else {
        FAIL("unknown input action error_kind: " << error_kind);
    }
}

InputActionMap loadGameplayMenuMap() {
    auto actions = parseInputActionsJson(readJson(fixtureRoot() / "valid" / "gameplay_menu.json"));
    return applyInputProfile(actions,
                             parseInputProfileJson(readJson(fixtureRoot() / "valid" / "keyboard.json"), actions));
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    Sandbox() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("pelican_inputactions_" + suffix);
        root = base / "project";
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
};

} // namespace

TEST_CASE("Input action parser fixtures", "[input-actions]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto document = readJson(fixtureRoot() / file);
            const auto mode = entry.value("mode", std::string{});
            const auto expected = entry.at("expect").get<std::string>();

            if (mode == "pose_resolution") {
                const auto map = parseInputActionsJson(document);
                InputStateCore input;
                ActionPose sample;
                sample.position[0] = 4.0F;
                sample.orientation_valid = true;
                sample.position_valid = true;
                sample.orientation_tracked = true;
                sample.valid = true;
                sample.source = ActionPoseSource::action_space;
                sample.reference_space = ActionPoseReferenceSpace::local;
                sample.hand = ActionPoseHand::left;
                input.queuePoseSample({"aim", sample});
                input.beginFrame();
                const auto frame = evaluateInputActions(
                    map, input.currentFrameInput(), std::vector<std::string>{"xr"});
                const auto pose = frame.pose("aim");
                CHECK(pose.position[0] == 4.0F);
                CHECK(pose.valid);
                CHECK(pose.orientation_valid);
                CHECK(pose.position_valid);
                CHECK(pose.orientation_tracked);
                CHECK_FALSE(pose.position_tracked);
                CHECK(pose.source == ActionPoseSource::action_space);
                CHECK(pose.reference_space == ActionPoseReferenceSpace::local);
                CHECK(pose.hand == ActionPoseHand::left);
                return;
            }

            if (expected == "ok") {
                const auto map = parseInputActionsJson(document);
                REQUIRE(map.actionCount() == entry.at("action_count").get<std::size_t>());
            } else {
                std::string message;
                try {
                    (void)parseInputActionsJson(document);
                } catch (const std::exception &ex) {
                    message = ex.what();
                }
                REQUIRE_FALSE(message.empty());
                requireErrorKind(message, entry.at("error_kind").get<std::string>());
            }
        }
    }
}

TEST_CASE("Input actions evaluate keyboard composites, edges, and set stack consumption", "[input-actions]") {
    const auto map = loadGameplayMenuMap();
    InputStateCore input;
    std::vector<std::string> gameplay_stack{"gameplay"};
    std::vector<std::string> gameplay_menu_stack{"gameplay", "menu"};

    input.queueButtonEvent(KeyCode::W, true);
    input.queueButtonEvent(KeyCode::Space, true);
    input.beginFrame();

    auto frame = evaluateInputActions(map, input.currentSnapshot(), gameplay_stack);
    auto move = frame.get("move");
    REQUIRE(move.pressed);
    REQUIRE(move.held);
    REQUIRE_FALSE(move.released);
    REQUIRE(move.axis2.x == 0.0f);
    REQUIRE(move.axis2.y == 1.0f);
    auto jump = frame.get("jump");
    REQUIRE(jump.pressed);
    REQUIRE(jump.held);

    input.beginFrame();
    frame = evaluateInputActions(map, input.currentSnapshot(), gameplay_stack);
    move = frame.get("move");
    jump = frame.get("jump");
    REQUIRE_FALSE(move.pressed);
    REQUIRE(move.held);
    REQUIRE(move.axis2.y == 1.0f);
    REQUIRE_FALSE(jump.pressed);
    REQUIRE(jump.held);

    frame = evaluateInputActions(map, input.currentSnapshot(), gameplay_menu_stack);
    REQUIRE(frame.get("confirm").held);
    REQUIRE_FALSE(frame.get("jump").held);
    REQUIRE(frame.get("move").held);

    input.queueButtonEvent(KeyCode::ArrowUp, true);
    input.beginFrame();
    frame = evaluateInputActions(map, input.currentSnapshot(), gameplay_menu_stack);
    auto navigate = frame.get("navigate");
    REQUIRE(navigate.pressed);
    REQUIRE(navigate.held);
    REQUIRE(navigate.axis2.x == 0.0f);
    REQUIRE(navigate.axis2.y == 1.0f);
    REQUIRE_FALSE(frame.get("jump").held);

    input.queueButtonEvent(KeyCode::Space, false);
    input.beginFrame();
    frame = evaluateInputActions(map, input.currentSnapshot(), gameplay_menu_stack);
    REQUIRE(frame.get("confirm").released);
    REQUIRE_FALSE(frame.get("jump").released);

    input.queueButtonEvent(KeyCode::Space, true);
    input.beginFrame();
    frame = evaluateInputActions(map, input.currentSnapshot(), gameplay_stack);
    REQUIRE(frame.get("jump").pressed);
    REQUIRE(frame.get("jump").held);
}

TEST_CASE("Actions API loads optional input_actions_json through ProjectBasicConfig", "[input-actions]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "input" / "actions.json",
              readText(fixtureRoot() / "valid" / "gameplay_menu.json"));
    writeText(sandbox.root / "input" / "profiles" / "keyboard.json",
              readText(fixtureRoot() / "valid" / "keyboard.json"));

    const auto project = nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "input-actions"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"input_actions_json", "input/actions.json"},
                          {"input_profiles", {{"keyboard", "input/profiles/keyboard.json"}}},
                          {"input_profile", "keyboard"}}},
    };

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());

    REQUIRE(Actions::isConfigured());
    const auto expected_stack = std::vector<std::string>{"gameplay"};
    REQUIRE(Actions::actionSetStack() == expected_stack);

    auto &input = GET_MODULE(InputState);
    input.queueEvent(InputEvent::button(KeyCode::D, true));
    input.beginFrame();
    const auto evaluations_before_queries = internal::inputActionsEvaluationCount();
    REQUIRE(Actions::axis2("move").x == 1.0f);
    REQUIRE(Actions::axis2("move").y == 0.0f);
    REQUIRE(Actions::isHeld("move"));
    REQUIRE(internal::inputActionsEvaluationCount() == evaluations_before_queries + 1);

    Actions::pushActionSet("menu");
    input.queueEvent(InputEvent::button(KeyCode::Space, true));
    input.beginFrame();
    REQUIRE(Actions::isPressed("confirm"));
    REQUIRE(Actions::isHeld("confirm"));
    REQUIRE_FALSE(Actions::isHeld("jump"));
    REQUIRE(internal::inputActionsEvaluationCount() == evaluations_before_queries + 2);

    input.beginFrame();
    input.consumeControlForActions(KeyCode::D);
    REQUIRE(Actions::axis2("move").x == 0.0f);
    REQUIRE_FALSE(Actions::isHeld("move"));
    REQUIRE(input.currentSnapshot().getKey(KeyCode::D));
    REQUIRE(internal::inputActionsEvaluationCount() == evaluations_before_queries + 3);
}

TEST_CASE("Runtime free camera supplies an embedded input profile without project declarations",
          "[input-actions][free-camera][wp265]") {
    ensureLogger();
    Sandbox sandbox;
    std::filesystem::create_directories(sandbox.root);

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(EngineLaunchConfig).free_camera = EngineLaunchFreeCamera{};

    REQUIRE(Actions::isConfigured());
    REQUIRE(Actions::actionSetStack() ==
            std::vector<std::string>{"free_camera"});
    REQUIRE(internal::activeInputProfile() ==
            std::optional<std::string>{"pelican_free_camera"});
    REQUIRE(internal::availableInputProfiles() ==
            std::vector<std::string>{"pelican_free_camera"});
    REQUIRE(internal::gamepadPollingEnabled());

    auto &input = GET_MODULE(InputState);
    input.queueEvent(InputEvent::button(KeyCode::W, true));
    input.queueEvent(InputEvent::button(KeyCode::ArrowRight, true));
    input.beginFrame();
    REQUIRE(Actions::axis2("move").y == 1.0f);
    REQUIRE(Actions::axis2("look").x == 1.0f);
}

TEST_CASE("Input profiles switch gamepad buttons and process axes", "[input-actions][gamepad]") {
    const auto actions = parseInputActionsJson(readJson(fixtureRoot() / "valid" / "gameplay_menu.json"));
    const auto gamepad = parseInputProfileJson(readJson(fixtureRoot() / "valid" / "gamepad.json"), actions);
    const auto arcade = parseInputProfileJson(readJson(fixtureRoot() / "valid" / "arcade.json"), actions);
    REQUIRE(gamepad.uses_gamepad);

    InputStateCore input;
    input.queueEvent(InputEvent::gamepadButton(0, GamepadButton::A, true));
    input.queueEvent(InputEvent::gamepadAxis(0, GamepadAxis::LeftX, 0.1f));
    input.queueEvent(InputEvent::gamepadAxis(0, GamepadAxis::LeftY, 0.1f));
    input.beginFrame();

    auto frame = evaluateInputActions(applyInputProfile(actions, gamepad), input.currentSnapshot(), {"gameplay"});
    REQUIRE(frame.get("jump").held);
    REQUIRE(frame.get("move").axis2.x == 0.0f);
    REQUIRE(frame.get("move").axis2.y == 0.0f);

    input.queueEvent(InputEvent::gamepadAxis(0, GamepadAxis::LeftY, 0.6f));
    input.beginFrame();
    frame = evaluateInputActions(applyInputProfile(actions, gamepad), input.currentSnapshot(), {"gameplay"});
    REQUIRE(frame.get("move").axis2.y < -0.45f);

    frame = evaluateInputActions(applyInputProfile(actions, arcade), input.currentSnapshot(), {"gameplay"});
    REQUIRE_FALSE(frame.get("jump").held);
    input.queueEvent(InputEvent::gamepadButton(0, GamepadButton::B, true));
    input.beginFrame();
    frame = evaluateInputActions(applyInputProfile(actions, arcade), input.currentSnapshot(), {"gameplay"});
    REQUIRE(frame.get("jump").held);

    std::string message;
    try {
        (void)parseInputProfileJson(readJson(fixtureRoot() / "invalid" / "unknown_pad_button.json"), actions);
    } catch (const std::exception &error) {
        message = error.what();
    }
    REQUIRE(contains(message, "turbo"));
    REQUIRE(contains(message, "gamepad button"));

    message.clear();
    try {
        (void)parseInputProfileJson(readJson(fixtureRoot() / "invalid" / "unknown_pad_axis.json"), actions);
    } catch (const std::exception &error) {
        message = error.what();
    }
    REQUIRE(contains(message, "warp_stick"));
    REQUIRE(contains(message, "gamepad axis2"));
}

TEST_CASE("Input action backend frames merge without replacing existing device evaluation",
          "[input-actions][backend]") {
    const auto definitions =
        parseInputActionsJson(readJson(fixtureRoot() / "valid" / "gameplay_menu.json"));
    const auto keyboard =
        parseInputProfileJson(readJson(fixtureRoot() / "valid" / "keyboard.json"), definitions);
    const auto map = applyInputProfile(definitions, keyboard);

    InputStateCore input;
    input.queueEvent(InputEvent::button(KeyCode::D, true));
    input.beginFrame();
    auto frame = evaluateInputActions(map, input.currentSnapshot(), {"gameplay"});
    REQUIRE(frame.get("move").axis2.x == 1.0F);

    InputActionFrame backend;
    backend.action_types.emplace("move", InputActionType::axis2);
    backend.actions.emplace("move",
                            InputActionState{.pressed = true,
                                             .held = true,
                                             .axis2 = {.x = 0.0F, .y = 0.5F}});
    backend.action_types.emplace("jump", InputActionType::button);
    backend.actions.emplace("jump", InputActionState{.pressed = true, .held = true});

    mergeInputActionBackendFrame(frame, backend);
    CHECK(frame.get("move").axis2.x == 1.0F);
    CHECK(frame.get("move").axis2.y == 0.5F);
    CHECK(frame.get("move").held);
    CHECK(frame.get("jump").pressed);
    CHECK(frame.get("jump").held);
}

} // namespace Pelican
