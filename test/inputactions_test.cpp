#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/os/actionmap.hpp"
#include "../src/core/os/inputstate.hpp"
#include "../src/core/userpublic/userinput.hpp"
#include "../src/project/inputactionoverlay.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

TEST_CASE("Input profile v1 evaluates wheel axes and modifier button chords",
          "[input-actions][wp271]") {
    const auto definitions = parseInputActionsString(R"json({
      "schema": "pelican.input_actions",
      "version": 1,
      "action_sets": [{
        "name": "viewport",
        "actions": [
          {"name": "orbit", "type": "button"},
          {"name": "pan", "type": "button"},
          {"name": "shortcut", "type": "button"},
          {"name": "zoom_x", "type": "axis1"},
          {"name": "zoom_y", "type": "axis1"}
        ]
      }]
    })json");
    const auto profile_document = nlohmann::json::parse(R"json({
      "schema": "pelican.input_profile",
      "version": 1,
      "name": "viewport",
      "bindings": [
        {"action": "orbit", "binding": "mouse:middle"},
        {"action": "pan", "binding": "mouse:shift+middle"},
        {"action": "shortcut", "binding": "kbd:ctrl+shift+k"},
        {"action": "zoom_x", "binding": "mouse:wheel_x"},
        {"action": "zoom_y", "binding": "mouse:wheel_y"}
      ]
    })json");
    const auto profile = parseInputProfileJson(profile_document, definitions);
    const auto map = applyInputProfile(definitions, profile);

    InputStateCore input;
    input.queueEvent(InputEvent::button(KeyCode::LeftShift, true));
    input.queueEvent(InputEvent::button(KeyCode::MouseMiddle, true));
    input.queueEvent(InputEvent::scroll(0.25F, -0.5F));
    input.queueEvent(InputEvent::scroll(0.25F, -0.75F));
    input.beginFrame();
    {
        const auto frame = evaluateInputActions(map, input.currentFrameInput(), {"viewport"});
        CHECK(frame.get("pan").pressed);
        CHECK(frame.get("pan").held);
        CHECK(frame.get("orbit").held);
        CHECK(frame.get("zoom_x").axis1 == 0.5F);
        CHECK(frame.get("zoom_y").axis1 == -1.0F);
    }

    input.beginFrame();
    {
        const auto frame = evaluateInputActions(map, input.currentFrameInput(), {"viewport"});
        CHECK(frame.get("pan").held);
        CHECK(frame.get("zoom_x").axis1 == 0.0F);
        CHECK(frame.get("zoom_y").axis1 == 0.0F);
        CHECK_FALSE(frame.get("zoom_y").held);
    }

    input.queueEvent(InputEvent::button(KeyCode::LeftShift, false));
    input.beginFrame();
    {
        const auto frame = evaluateInputActions(map, input.currentFrameInput(), {"viewport"});
        CHECK(frame.get("pan").released);
        CHECK_FALSE(frame.get("pan").held);
        CHECK(frame.get("orbit").held);
    }

    input.queueEvent(InputEvent::button(KeyCode::RightControl, true));
    input.queueEvent(InputEvent::button(KeyCode::RightShift, true));
    input.queueEvent(InputEvent::button(KeyCode::K, true));
    input.beginFrame();
    {
        const auto frame = evaluateInputActions(map, input.currentFrameInput(), {"viewport"});
        CHECK(frame.get("pan").pressed);
        CHECK(frame.get("pan").held);
        CHECK(frame.get("shortcut").pressed);
        CHECK(frame.get("shortcut").held);
    }

    auto unsupported_version = profile_document;
    unsupported_version["version"] = 2;
    CHECK_THROWS(parseInputProfileJson(unsupported_version, definitions));
}

TEST_CASE("Repository input profiles remain readable as v1", "[input-actions][wp271]") {
    const auto projects_root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects";
    std::size_t profile_count = 0;
    for (const auto &project_entry : std::filesystem::directory_iterator{projects_root}) {
        const auto project_path = project_entry.path() / "project.json";
        if (!project_entry.is_directory() || !std::filesystem::is_regular_file(project_path)) {
            continue;
        }

        const auto project = readJson(project_path);
        if (!project.contains("basic_config")) {
            continue;
        }
        const auto &basic_config = project.at("basic_config");
        if (!basic_config.contains("input_actions_json") || !basic_config.contains("input_profiles")) {
            continue;
        }

        const auto definitions = parseInputActionsJson(
            readJson(project_entry.path() / basic_config.at("input_actions_json").get<std::string>()));
        for (const auto &[profile_name, relative_path] : basic_config.at("input_profiles").items()) {
            CAPTURE(project_path, profile_name);
            const auto profile = parseInputProfileJson(
                readJson(project_entry.path() / relative_path.get<std::string>()), definitions);
            CHECK(profile.name == profile_name);
            CHECK(applyInputProfile(definitions, profile).actionCount() == definitions.actionCount());
            ++profile_count;
        }
    }
    REQUIRE(profile_count > 0);
}

TEST_CASE("startup input action overlay is explicit, additive, and rejects named collisions",
          "[input-actions][overlay][wp290]") {
    const auto authored = readText(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example" / "input" / "actions.json");
    const std::vector<std::string> no_overlays;
    bool loader_called = false;
    const auto unchanged = applyInputActionOverlays(
        authored, no_overlays,
        [&loader_called](std::string_view) {
            loader_called = true;
            return std::string{};
        });
    REQUIRE_FALSE(loader_called);
    REQUIRE(unchanged.effective_actions_json == authored);
    REQUIRE(unchanged.overlays.empty());

    const std::string overlay_reference = "project://tool.json";
    const std::string actions_reference = "project://tool-actions.json";
    const std::string profile_reference = "project://tool-profile.json";
    const std::unordered_map<std::string, std::string> documents{
        {overlay_reference,
         R"json({"schema":"pelican.input_action_overlay","version":1,"name":"tool","actions":"project://tool-actions.json","profile":"project://tool-profile.json"})json"},
        {actions_reference,
         R"json({"schema":"pelican.input_actions","version":1,"action_sets":[{"name":"tool","actions":[{"name":"tool_action","type":"button"}]}]})json"},
        {profile_reference,
         R"json({"schema":"pelican.input_profile","version":1,"name":"tool","bindings":[{"action":"tool_action","binding":"kbd:t"}]})json"},
    };
    const auto load = [&documents](std::string_view reference) {
        const auto found = documents.find(std::string{reference});
        if (found == documents.end()) {
            throw std::runtime_error("missing fixture");
        }
        return found->second;
    };
    const std::vector<std::string> overlays{overlay_reference};
    const auto applied = applyInputActionOverlays(authored, overlays, load);
    REQUIRE(applied.effective_actions_json.has_value());
    const auto effective =
        parseInputActionsString(*applied.effective_actions_json);
    REQUIRE(effective.findAction("move") != nullptr);
    REQUIRE(effective.findAction("jump") != nullptr);
    REQUIRE(effective.findAction("tool_action") != nullptr);
    REQUIRE(applied.overlays.size() == 1);
    REQUIRE(applied.overlays.front().action_set_names ==
            std::vector<std::string>{"tool"});
    REQUIRE(applied.overlays.front().profile_json.has_value());
    const auto overlay_definitions =
        parseInputActionsString(applied.overlays.front().actions_json);
    const auto overlay_profile = parseInputProfileString(
        *applied.overlays.front().profile_json, overlay_definitions);
    REQUIRE(overlay_profile.bindings.front().action == "tool_action");

    auto collision_documents = documents;
    collision_documents.at(actions_reference) =
        R"json({"schema":"pelican.input_actions","version":1,"action_sets":[{"name":"tool","actions":[{"name":"move","type":"axis2"}]}]})json";
    const auto load_collision = [&collision_documents](
                                    std::string_view reference) {
        return collision_documents.at(std::string{reference});
    };
    REQUIRE_THROWS_WITH(
        applyInputActionOverlays(authored, overlays, load_collision),
        Catch::Matchers::ContainsSubstring("action name collision: move"));

    collision_documents.at(actions_reference) =
        R"json({"schema":"pelican.input_actions","version":1,"action_sets":[{"name":"gameplay","actions":[{"name":"tool_action","type":"button"}]}]})json";
    REQUIRE_THROWS_WITH(
        applyInputActionOverlays(authored, overlays, load_collision),
        Catch::Matchers::ContainsSubstring(
            "action set name collision: gameplay"));
}

TEST_CASE("startup input action overlay accepts only its strict v1 envelope",
          "[input-actions][overlay][format][wp290]") {
    const std::optional<std::string> no_authored_actions;
    const std::vector<std::string> overlays{"project://overlay.json"};
    const auto apply = [&](std::string bytes) {
        return applyInputActionOverlays(
            no_authored_actions, overlays,
            [bytes = std::move(bytes)](std::string_view) {
                return bytes;
            });
    };

    REQUIRE_THROWS_WITH(
        apply(R"json({"schema":"pelican.input_action_overlay","version":2,"name":"tool","actions":"project://actions.json"})json"),
        Catch::Matchers::ContainsSubstring("version must be exactly 1"));
    REQUIRE_THROWS_WITH(
        apply(R"json({"schema":"pelican.input_action_overlay","version":1,"name":"tool","actions":"project://actions.json","project":true})json"),
        Catch::Matchers::ContainsSubstring("unknown key 'project'"));
    REQUIRE_THROWS_WITH(
        apply(R"json({"schema":"pelican.input_action_overlay","version":1,"name":"tool","actions":""})json"),
        Catch::Matchers::ContainsSubstring(
            "non-empty string actions reference"));
}

TEST_CASE("Editor transform presets add named actions while grab disables their bindings",
          "[input-actions][overlay][modal][negative-contrast][wp286]") {
    struct Observation {
        bool translate_pressed = false;
        std::size_t translate_bindings = 0;
        std::vector<std::string> action_sets;
    };
    const auto observe = [](EditorTransformInputPreset preset) {
        ensureLogger();
        Sandbox sandbox;
        writeText(sandbox.root / "input" / "actions.json",
                  readText(fixtureRoot() / "valid" /
                           "gameplay_menu.json"));
        writeText(sandbox.root / "input" / "keyboard.json",
                  readText(fixtureRoot() / "valid" / "keyboard.json"));
        const auto project = nlohmann::json{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "editor-transform-input-overlay"},
            {"engine_min_version", "0.1.0"},
            {"basic_config",
             {{"input_actions_json", "input/actions.json"},
              {"input_profiles",
               {{"keyboard", "input/keyboard.json"}}},
              {"input_profile", "keyboard"}}},
        };
        const auto project_bytes = project.dump();
        writeText(sandbox.root / "project.json", project_bytes);

        FastModuleContainer modules;
        GET_MODULE(PathResolver).setup(sandbox.root, false);
        GET_MODULE(ProjectSource).setProjectData(project_bytes);
        GET_MODULE(EngineLaunchConfig).input_action_overlays.emplace_back(
            editorTransformInputActionOverlayReference(preset));

        REQUIRE(Actions::isConfigured());
        const auto *map = internal::inputActionMap();
        REQUIRE(map != nullptr);
        const auto *translate = map->findAction("gizmo.translate");
        REQUIRE(translate != nullptr);
        auto &input = GET_MODULE(InputState);
        input.queueEvent(InputEvent::button(KeyCode::G, true));
        input.beginFrame();
        return Observation{
            .translate_pressed = Actions::isPressed("gizmo.translate"),
            .translate_bindings = translate->bindings.size(),
            .action_sets = Actions::actionSetStack(),
        };
    };

    REQUIRE(defaultEditorTransformInputPreset ==
            EditorTransformInputPreset::Grab);
    REQUIRE(std::string{editorTransformInputPresetName(
                defaultEditorTransformInputPreset)} == "grab");
    const Observation blender =
        observe(EditorTransformInputPreset::Blender);
    const Observation grab = observe(EditorTransformInputPreset::Grab);
    REQUIRE(blender.action_sets.back() == "gizmo_modal");
    REQUIRE(grab.action_sets.back() == "gizmo_modal");
    REQUIRE(blender.translate_bindings == 1);
    REQUIRE(grab.translate_bindings == 0);
    REQUIRE(blender.translate_pressed);
    REQUIRE_FALSE(grab.translate_pressed);
    // Rule 10: identical physical input produces a distinct result when the
    // binding preset is the handle-only negative control.
    REQUIRE(blender.translate_pressed != grab.translate_pressed);
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

TEST_CASE("Runtime free camera overlays project actions and keeps project profile selection",
          "[input-actions][free-camera][overlay][wp290]") {
    struct PresetCase {
        EngineLaunchFreeCameraPreset preset;
        std::string_view overlay;
        std::string_view name;
        std::string_view orbit;
        std::string_view pan;
        std::string_view zoom_drag;
        std::optional<KeyCode> modifier;
        KeyCode orbit_button;
    };
    const PresetCase cases[] = {
        {EngineLaunchFreeCameraPreset::Blender,
         freeCameraBlenderInputActionOverlayReference, "blender", "mouse:middle",
         "mouse:shift+middle", "mouse:ctrl+middle", std::nullopt,
         KeyCode::MouseMiddle},
        {EngineLaunchFreeCameraPreset::Unity,
         freeCameraUnityInputActionOverlayReference, "unity", "mouse:alt+left",
         "mouse:middle", "mouse:alt+right", KeyCode::LeftAlt,
         KeyCode::MouseLeft},
    };

    ensureLogger();
    for (const auto &preset : cases) {
        DYNAMIC_SECTION(std::string{preset.name}) {
            Sandbox sandbox;
            writeText(sandbox.root / "input" / "actions.json",
                      readText(fixtureRoot() / "valid" /
                               "gameplay_menu.json"));
            writeText(sandbox.root / "input" / "keyboard.json",
                      readText(fixtureRoot() / "valid" / "keyboard.json"));
            writeText(sandbox.root / "input" / "gamepad.json",
                      readText(fixtureRoot() / "valid" / "gamepad.json"));
            const auto project = nlohmann::json{
                {"schema", "pelican.project"},
                {"version", 1},
                {"name", "input-action-overlay"},
                {"engine_min_version", "0.1.0"},
                {"basic_config",
                 {{"input_actions_json", "input/actions.json"},
                  {"input_profiles",
                   {{"keyboard", "input/keyboard.json"},
                    {"gamepad", "input/gamepad.json"}}},
                  {"input_profile", "keyboard"}}},
            };
            const auto project_bytes = project.dump(2);
            writeText(sandbox.root / "project.json", project_bytes);

            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(sandbox.root, false);
            GET_MODULE(ProjectSource).setProjectData(project_bytes);
            auto &launch = GET_MODULE(EngineLaunchConfig);
            launch.input_profile = "keyboard";
            launch.free_camera = EngineLaunchFreeCamera{
                .preset = preset.preset,
            };
            launch.input_action_overlays.emplace_back(preset.overlay);

            REQUIRE(Actions::isConfigured());
            REQUIRE(Actions::actionSetStack() ==
                    std::vector<std::string>{"gameplay", "free_camera"});
            REQUIRE(internal::activeInputProfile() ==
                    std::optional<std::string>{"keyboard"});
            REQUIRE(internal::availableInputProfiles() ==
                    std::vector<std::string>{"gamepad", "keyboard"});
            REQUIRE_FALSE(internal::gamepadPollingEnabled());

            const auto *map = internal::inputActionMap();
            REQUIRE(map != nullptr);
            REQUIRE(map->findAction("move") != nullptr);
            REQUIRE(map->findAction("jump") != nullptr);
            const auto require_binding = [map](std::string_view action,
                                               std::string_view binding) {
                const auto *definition = map->findAction(action);
                REQUIRE(definition != nullptr);
                REQUIRE(definition->bindings.size() == 1);
                REQUIRE(definition->bindings.front().text == std::string{binding});
            };
            require_binding("pelican_view_orbit", preset.orbit);
            require_binding("pelican_view_pan", preset.pan);
            require_binding("pelican_view_zoom_drag", preset.zoom_drag);
            require_binding("pelican_view_pointer_x", "mouse:delta_x");
            require_binding("pelican_view_pointer_y", "mouse:delta_y");
            require_binding("pelican_view_wheel", "mouse:wheel_y");

            auto &input = GET_MODULE(InputState);
            input.queueEvent(InputEvent::button(KeyCode::D, true));
            input.queueEvent(InputEvent::button(KeyCode::Space, true));
            if (preset.modifier) {
                input.queueEvent(InputEvent::button(*preset.modifier, true));
            }
            input.queueEvent(InputEvent::button(preset.orbit_button, true));
            input.queueEvent(InputEvent::axis(0.25f, -0.5f));
            input.queueEvent(InputEvent::scroll(0.0f, 0.75f));
            input.beginFrame();
            REQUIRE(Actions::axis2("move").x == 1.0f);
            REQUIRE(Actions::isHeld("jump"));
            REQUIRE(Actions::isHeld("pelican_view_orbit"));
            REQUIRE(Actions::axis1("pelican_view_pointer_x") == 0.25f);
            REQUIRE(Actions::axis1("pelican_view_pointer_y") == -0.5f);
            REQUIRE(Actions::axis1("pelican_view_wheel") == 0.75f);
            REQUIRE_THROWS_WITH(
                Actions::axis1("missing_action"),
                Catch::Matchers::ContainsSubstring(
                    "unknown input action: missing_action"));

            internal::selectInputProfile("gamepad");
            REQUIRE(internal::activeInputProfile() ==
                    std::optional<std::string>{"gamepad"});
            REQUIRE(internal::gamepadPollingEnabled());
            const auto *switched = internal::inputActionMap();
            REQUIRE(switched != nullptr);
            REQUIRE(switched->findAction("pelican_view_orbit")
                        ->bindings.front().text == std::string{preset.orbit});
            REQUIRE(readText(sandbox.root / "project.json") == project_bytes);
        }
    }
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
