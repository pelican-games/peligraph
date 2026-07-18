#define GLM_ENABLE_EXPERIMENTAL
#include "rpcserver.hpp"
#include "editorcommandservice.hpp"
#include "editorruntimefactory.hpp"
#include "../startup.hpp"

#include "../appflow/framephase.hpp"

#include "../appflow/enginetime.hpp"
#include "../asset/model.hpp"
#include "../ecs/archetypemigration.hpp"
#include "../ecs/core.hpp"
#include "../ecs/componentinfo.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/editorprojectionadapters.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/scene.hpp"
#include "../gamelogic/gamelogicreload.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../launchconfig.hpp"
#include "../material/materialcontainer.hpp"
#include "../light/lightcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../os/inputsequence.hpp"
#include "../os/inputstate.hpp"
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrsession.hpp"
#endif
#include "../playback/seqplayer.hpp"
#include "../phys/physworld.hpp"
#include "../renderingpass/renderingpassjsonhelpers.hpp"
#include "../renderdoc/renderdoccapture.hpp"
#include "../renderer/spritescene.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../userpublic/gamecontext.hpp"
#include "../userpublic/userinput.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../userpublic/details/system/registerer.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/rendertarget.hpp"
#include "../vkcore/rendertiming.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/memorydiagnostics.hpp"
#include "../watch/reloadgate.hpp"
#include "../watch/reloadservice.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <istream>
#include <iterator>
#include <optional>
#include <ostream>
#include <random>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Pelican {

namespace {

nlohmann::json inactiveOpenXrStatusJson() {
    return {
        {"active", false},
        {"reference_space", nullptr},
        {"floor_semantics", "not_applicable"},
        {"floor_level_guaranteed", false},
        {"applied_floor_offset_m", nullptr},
        {"session_state", nullptr},
        {"view_configuration", nullptr},
        {"should_render", nullptr},
        {"timing", {{"wait_frame_count", 0},
                    {"should_render_false_count", 0},
                    {"should_render_false_rate", 0.0},
                    {"begin_frame_discarded_count", 0},
                    {"session_loss_pending_count", 0},
                    {"mirror_presented", 0},
                    {"mirror_dropped", 0},
                    {"mirror_failures", 0},
                    {"time_conversion", {{"available", false},
                                         {"reason", "xr_inactive"},
                                         {"source", nullptr},
                                         {"failures", 0},
                                         {"after_wait_margin_ms", {{"count", 0}, {"min", nullptr}, {"median", nullptr}, {"p95", nullptr}}},
                                         {"before_submit_margin_ms", {{"count", 0}, {"min", nullptr}, {"median", nullptr}, {"p95", nullptr}}},
                                         {"after_end_frame_margin_ms", {{"count", 0}, {"min", nullptr}, {"median", nullptr}, {"p95", nullptr}}}}}}},
    };
}

#if PELICAN_WITH_OPENXR
nlohmann::json xrMarginJson(const OpenXr::XrTimingMarginStatus &margin) {
    return {
        {"count", margin.count},
        {"min", margin.minimum_ms ? nlohmann::json(*margin.minimum_ms)
                                    : nlohmann::json(nullptr)},
        {"median", margin.median_ms ? nlohmann::json(*margin.median_ms)
                                     : nlohmann::json(nullptr)},
        {"p95", margin.p95_ms ? nlohmann::json(*margin.p95_ms)
                               : nlohmann::json(nullptr)},
    };
}

nlohmann::json openXrStatusJson(const OpenXr::XrDiagnosticStatus &status) {
    return {
        {"active", true},
        {"reference_space", status.reference_space.reference_space},
        {"floor_semantics", status.reference_space.floor_semantics},
        {"floor_level_guaranteed", status.reference_space.floor_level_guaranteed},
        {"applied_floor_offset_m", status.reference_space.applied_floor_offset_m},
        {"session_state", status.session_state},
        {"view_configuration", status.view_configuration},
        {"should_render", status.should_render ? nlohmann::json(*status.should_render)
                                                : nlohmann::json(nullptr)},
        {"timing", {{"wait_frame_count", status.timing.wait_frame_count},
                    {"should_render_false_count", status.timing.should_render_false_count},
                    {"should_render_false_rate", status.timing.should_render_false_rate},
                    {"begin_frame_discarded_count", status.timing.begin_frame_discarded_count},
                    {"session_loss_pending_count", status.timing.session_loss_pending_count},
                    {"mirror_presented", status.timing.mirror_presented},
                    {"mirror_dropped", status.timing.mirror_dropped},
                    {"mirror_failures", status.timing.mirror_failures},
                    {"time_conversion", {{"available", status.timing.time_conversion_available},
                                         {"reason", status.timing.time_conversion_reason},
                                         {"source", status.timing.time_conversion_available
                                                        ? nlohmann::json("XR_KHR_win32_convert_performance_counter_time")
                                                        : nlohmann::json(nullptr)},
                                         {"failures", status.timing.time_conversion_failures},
                                         {"after_wait_margin_ms", xrMarginJson(status.timing.after_wait_margin)},
                                         {"before_submit_margin_ms", xrMarginJson(status.timing.before_submit_margin)},
                                         {"after_end_frame_margin_ms", xrMarginJson(status.timing.after_end_frame_margin)}}}}},
    };
}
#endif

nlohmann::json openXrStatusJson(const EngineLaunchConfig &launch_config) {
#if PELICAN_WITH_OPENXR
    if (launch_config.xr_active) {
        if (const auto *runtime = FastModuleContainer::tryGet<OpenXr::SessionRuntime>()) {
            return openXrStatusJson(runtime->diagnosticStatus());
        }
    }
#endif
    return inactiveOpenXrStatusJson();
}

const nlohmann::json &requireObjectParams(const nlohmann::json &params, const std::string &method) {
    if (!params.is_object()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams, method + " params must be an object");
    }
    return params;
}

double requireNumberParam(const nlohmann::json &params, const char *name, const std::string &method) {
    const auto &object = requireObjectParams(params, method);
    if (!object.contains(name) || !object.at(name).is_number()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " params requires numeric field '" + std::string{name} + "'");
    }
    return object.at(name).get<double>();
}

std::uint64_t requireUnsignedIntegerParam(const nlohmann::json &params, const char *name,
                                          const std::string &method) {
    const auto &object = requireObjectParams(params, method);
    if (!object.contains(name)) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " params requires unsigned integer field '" + std::string{name} + "'");
    }

    const auto &value = object.at(name);
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) {
            return static_cast<std::uint64_t>(signed_value);
        }
    }

    throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                              method + " params field '" + std::string{name} +
                                  "' must be a non-negative integer");
}

std::string requireStringParam(const nlohmann::json &params, const char *name, const std::string &method) {
    const auto &object = requireObjectParams(params, method);
    if (!object.contains(name) || !object.at(name).is_string()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " params requires string field '" + std::string{name} + "'");
    }

    auto value = object.at(name).get<std::string>();
    if (value.empty()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " params field '" + std::string{name} + "' must not be empty");
    }
    return value;
}

std::optional<std::string> optionalStringParam(const nlohmann::json &params, const char *name,
                                               const std::string &method) {
    const auto &object = requireObjectParams(params, method);
    if (!object.contains(name)) {
        return std::nullopt;
    }
    if (!object.at(name).is_string()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " params field '" + std::string{name} + "' must be a string");
    }
    return object.at(name).get<std::string>();
}

bool isR7Identifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        const bool is_digit = ch >= '0' && ch <= '9';
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_lower = ch >= 'a' && ch <= 'z';
        if (!is_digit && !is_upper && !is_lower && ch != '_') {
            return false;
        }
    }
    return true;
}

void requireOptionalObjectName(const std::optional<std::string> &name, const std::string &method) {
    if (name && !isR7Identifier(*name)) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " params field 'name' must match [a-zA-Z0-9_]");
    }
}

const nlohmann::json &requireArrayField(const nlohmann::json &object, const char *name, const std::string &method) {
    if (!object.contains(name) || !object.at(name).is_array()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " params requires array field '" + std::string{name} + "'");
    }
    return object.at(name);
}

float numberAt(const nlohmann::json &array, size_t index, const std::string &field, const std::string &method) {
    if (!array.at(index).is_number()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " transform field '" + field + "' must contain only numbers");
    }
    return array.at(index).get<float>();
}

glm::vec3 requireVec3Field(const nlohmann::json &object, const char *name, const std::string &method) {
    if (!object.contains(name) || !object.at(name).is_array() || object.at(name).size() != 3) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " transform requires vec3 field '" + std::string{name} + "'");
    }
    const auto &array = object.at(name);
    return glm::vec3{
        numberAt(array, 0, name, method),
        numberAt(array, 1, name, method),
        numberAt(array, 2, name, method),
    };
}

glm::quat requireQuatField(const nlohmann::json &object, const char *name, const std::string &method) {
    if (!object.contains(name) || !object.at(name).is_array() || object.at(name).size() != 4) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " transform requires quat field '" + std::string{name} + "'");
    }
    const auto &array = object.at(name);
    const auto x = numberAt(array, 0, name, method);
    const auto y = numberAt(array, 1, name, method);
    const auto z = numberAt(array, 2, name, method);
    const auto w = numberAt(array, 3, name, method);
    return glm::quat{w, x, y, z};
}

SceneObjectTransform parseSceneObjectTransform(const nlohmann::json &json, const std::string &method) {
    if (!json.is_object()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams, method + " transforms entries must be objects");
    }
    if (json.contains("rot")) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  method + " transform field 'rot' is not supported in v1. Use 'rotation'");
    }
    return SceneObjectTransform{
        .pos = requireVec3Field(json, "pos", method),
        .rotation = requireQuatField(json, "rotation", method),
        .scale = requireVec3Field(json, "scale", method),
    };
}

struct PendingTransformUpdate {
    std::string object;
    GameObjectId object_id;
    SceneObjectTransform transform;
};

struct EngineRpcModules {
    RenderDocCapture &renderdoc_capture;
    EngineTime &engine_time;
    ECSCore &ecs_core;
    ComponentInfoManager &component_info;
    ModelAssetContainer &models;
    PolygonInstanceContainer &polygon_instances;
    Camera &camera;
    LightContainer &lights;
    PhysWorld &physics;
    PathResolver &path_resolver;
    ProjectBasicConfig &project_config;
    SceneLoader &scene_loader;
    RenderTarget &render_target;
    StartupMetrics &startup_metrics;
    InputSequenceRuntime &input_sequence;
    InputState &input_state;
    EngineLaunchConfig &launch_config;
    watch::ReloadGate &reload_gate;
    watch::ReloadService *reload_service;
    SpriteScene *sprite_scene;
    Renderer &renderer;
    SeqPlayer &seq_player;
    VulkanManageCore &vulkan;
};

EngineRpcModules resolveEngineRpcModules() {
    return {
        GET_MODULE(RenderDocCapture),
        GET_MODULE(EngineTime),
        GET_MODULE(ECSCore),
        GET_MODULE(ComponentInfoManager),
        GET_MODULE(ModelAssetContainer),
        GET_MODULE(PolygonInstanceContainer),
        GET_MODULE(Camera),
        GET_MODULE(LightContainer),
        GET_MODULE(PhysWorld),
        GET_MODULE(PathResolver),
        GET_MODULE(ProjectBasicConfig),
        GET_MODULE(SceneLoader),
        GET_MODULE(RenderTarget),
        GET_MODULE(StartupMetrics),
        GET_MODULE(InputSequenceRuntime),
        GET_MODULE(InputState),
        GET_MODULE(EngineLaunchConfig),
        GET_MODULE(watch::ReloadGate),
        FastModuleContainer::tryGet<watch::ReloadService>(),
        FastModuleContainer::tryGet<SpriteScene>(),
        GET_MODULE(Renderer),
        GET_MODULE(SeqPlayer),
        GET_MODULE(VulkanManageCore),
    };
}


template <class Invoke> nlohmann::json invokeEditorRpc(Invoke &&invoke) {
    try {
        return nlohmann::json(std::forward<Invoke>(invoke)());
    } catch (const EditorCommandError &error) {
        const auto rpc_code = error.code() == EditorCommandErrorCode::InvalidParams
                                  ? JsonRpcErrorCodes::invalidParams
                                  : JsonRpcErrorCodes::applicationError;
        nlohmann::json data{
            {"code", editorCommandErrorCodeName(error.code())}};
        if (error.detail()) data["detail"] = *error.detail();
        throw JsonRpcHandlerError{rpc_code, error.what(), std::move(data)};
    } catch (const std::invalid_argument &error) {
        throw JsonRpcHandlerError{JsonRpcErrorCodes::invalidParams, error.what()};
    }
}

std::vector<PendingTransformUpdate> parseTransformUpdates(const nlohmann::json &params,
                                                          SceneLoader &scene_loader) {
    constexpr auto method = "update_transforms";
    const auto &object = requireObjectParams(params, method);
    const auto &objects = requireArrayField(object, "objects", method);
    const auto &transforms = requireArrayField(object, "transforms", method);
    if (objects.size() != transforms.size()) {
        throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                  "update_transforms objects and transforms counts must match");
    }

    std::vector<PendingTransformUpdate> updates;
    updates.reserve(objects.size());
    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects.at(i).is_string()) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                      "update_transforms objects entries must be strings");
        }
        auto name = objects.at(i).get<std::string>();
        const auto object_id = scene_loader.objectId(name);
        if (!object_id.has_value()) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, "unknown object name: " + name);
        }
        updates.push_back(PendingTransformUpdate{
            .object = std::move(name),
            .object_id = *object_id,
            .transform = parseSceneObjectTransform(transforms.at(i), method),
        });
    }
    return updates;
}

void flushPendingTransforms(std::vector<PendingTransformUpdate> &pending, SceneLoader &scene_loader) {
    for (const auto &update : pending) {
        const auto current_id = scene_loader.objectId(update.object);
        if (!current_id.has_value() || *current_id != update.object_id) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError,
                                      "update_transforms object was deleted: " + update.object);
        }
        scene_loader.applyObjectTransform(update.object, update.transform);
    }
    pending.clear();
}

std::string lowerAscii(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            lowered.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            lowered.push_back(ch);
        }
    }
    return lowered;
}

std::optional<KeyCode> injectedKeyboardKey(std::string_view key_name) {
    const auto key = lowerAscii(key_name);
    if (key.size() == 1) {
        const char ch = key[0];
        if (ch >= 'a' && ch <= 'z') {
            return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (ch - 'a'));
        }
        if (ch >= '0' && ch <= '9') {
            return static_cast<KeyCode>(static_cast<int>(KeyCode::Num0) + (ch - '0'));
        }
    }

    if (key == "space") {
        return KeyCode::Space;
    }
    if (key == "enter") {
        return KeyCode::Enter;
    }
    if (key == "escape" || key == "esc") {
        return KeyCode::Escape;
    }
    if (key == "tab") {
        return KeyCode::Tab;
    }
    if (key == "backspace") {
        return KeyCode::Backspace;
    }
    if (key == "left_shift") {
        return KeyCode::LeftShift;
    }
    if (key == "right_shift") {
        return KeyCode::RightShift;
    }
    if (key == "left_control" || key == "left_ctrl") {
        return KeyCode::LeftControl;
    }
    if (key == "right_control" || key == "right_ctrl") {
        return KeyCode::RightControl;
    }
    if (key == "left_alt") {
        return KeyCode::LeftAlt;
    }
    if (key == "right_alt") {
        return KeyCode::RightAlt;
    }
    if (key == "left_super") {
        return KeyCode::LeftSuper;
    }
    if (key == "right_super") {
        return KeyCode::RightSuper;
    }
    if (key == "arrow_up" || key == "up") {
        return KeyCode::ArrowUp;
    }
    if (key == "arrow_down" || key == "down") {
        return KeyCode::ArrowDown;
    }
    if (key == "arrow_left" || key == "left") {
        return KeyCode::ArrowLeft;
    }
    if (key == "arrow_right" || key == "right") {
        return KeyCode::ArrowRight;
    }
    if (key.size() >= 2 && key[0] == 'f') {
        int value = 0;
        bool numeric = true;
        for (std::size_t i = 1; i < key.size(); ++i) {
            const char ch = key[i];
            if (ch < '0' || ch > '9') {
                numeric = false;
                break;
            }
            value = value * 10 + (ch - '0');
        }
        if (numeric && value >= 1 && value <= 12) {
            return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (value - 1));
        }
    }
    if (key.size() == 4 && key.substr(0, 3) == "num") {
        const char ch = key[3];
        if (ch >= '0' && ch <= '9') {
            return static_cast<KeyCode>(static_cast<int>(KeyCode::Num0) + (ch - '0'));
        }
    }

    return std::nullopt;
}

std::optional<KeyCode> injectedMouseButton(std::string_view button_name) {
    const auto button = lowerAscii(button_name);
    if (button == "left") {
        return KeyCode::MouseLeft;
    }
    if (button == "right") {
        return KeyCode::MouseRight;
    }
    if (button == "middle") {
        return KeyCode::MouseMiddle;
    }
    if (button == "button4") {
        return KeyCode::MouseButton4;
    }
    if (button == "button5") {
        return KeyCode::MouseButton5;
    }
    if (button == "button6") {
        return KeyCode::MouseButton6;
    }
    if (button == "button7") {
        return KeyCode::MouseButton7;
    }
    if (button == "button8") {
        return KeyCode::MouseButton8;
    }
    return std::nullopt;
}

InputEvent injectedAxisEvent(std::string_view axis_name, double value) {
    const auto axis = lowerAscii(axis_name);
    const auto amount = static_cast<float>(value);
    if (axis == "mouse_delta_x" || axis == "mouse:delta_x" || axis == "delta_x") {
        return InputEvent::axis(amount, 0.0f);
    }
    if (axis == "mouse_delta_y" || axis == "mouse:delta_y" || axis == "delta_y") {
        return InputEvent::axis(0.0f, amount);
    }
    throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                              "inject_input unknown axis control: " + std::string{axis_name});
}

std::vector<InputEvent> bindInjectedInputEvents(const std::vector<RpcInputInjectionEvent> &rpc_events) {
    std::vector<InputEvent> events;
    events.reserve(rpc_events.size());
    for (const auto &rpc_event : rpc_events) {
        switch (rpc_event.type) {
        case RpcInputInjectionEventType::keyDown:
        case RpcInputInjectionEventType::keyUp: {
            const auto key = injectedKeyboardKey(rpc_event.name);
            if (!key) {
                throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                          "inject_input unknown key: " + rpc_event.name);
            }
            events.push_back(InputEvent::button(*key, rpc_event.type == RpcInputInjectionEventType::keyDown));
            break;
        }
        case RpcInputInjectionEventType::mouseMove:
            events.push_back(InputEvent::cursorMove(static_cast<float>(rpc_event.x), static_cast<float>(rpc_event.y)));
            break;
        case RpcInputInjectionEventType::mouseDown:
        case RpcInputInjectionEventType::mouseUp: {
            const auto button = injectedMouseButton(rpc_event.name);
            if (!button) {
                throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                          "inject_input unknown mouse button: " + rpc_event.name);
            }
            events.push_back(InputEvent::button(*button, rpc_event.type == RpcInputInjectionEventType::mouseDown));
            break;
        }
        case RpcInputInjectionEventType::axis:
            events.push_back(injectedAxisEvent(rpc_event.name, rpc_event.value));
            break;
        case RpcInputInjectionEventType::gamepadButtonDown:
        case RpcInputInjectionEventType::gamepadButtonUp: {
            const auto name = lowerAscii(rpc_event.name);
            const auto button = gamepadButtonFromName(name);
            if (!button) {
                throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                          "inject_input unknown gamepad button: " + rpc_event.name);
            }
            if (rpc_event.gamepad >= gamepad_slot_count) {
                throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                          "inject_input gamepad slot is out of range");
            }
            events.push_back(InputEvent::gamepadButton(
                static_cast<std::uint8_t>(rpc_event.gamepad), *button,
                rpc_event.type == RpcInputInjectionEventType::gamepadButtonDown));
            break;
        }
        case RpcInputInjectionEventType::gamepadAxis: {
            const auto name = lowerAscii(rpc_event.name);
            const auto axis = gamepadAxisFromName(name);
            if (!axis) {
                throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                          "inject_input unknown gamepad axis: " + rpc_event.name);
            }
            if (rpc_event.gamepad >= gamepad_slot_count) {
                throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                          "inject_input gamepad slot is out of range");
            }
            events.push_back(InputEvent::gamepadAxis(static_cast<std::uint8_t>(rpc_event.gamepad), *axis,
                                                      static_cast<float>(rpc_event.value)));
            break;
        }
        }
    }
    return events;
}

nlohmann::json frameResult(const EngineTime &engine_time) {
    return nlohmann::json{
        {"t", engine_time.now()},
        {"frame", engine_time.frameIndex()},
    };
}

std::string projectRootString(const PathResolver &path_resolver) {
    const auto resolved = path_resolver.resolveProjectRef(".");
    if (const auto path = std::get_if<std::filesystem::path>(&resolved)) {
        return path->generic_string();
    }
    throw std::runtime_error("project root did not resolve to a filesystem path");
}

nlohmann::json assetStoresStatus(const PathResolver &path_resolver) {
    nlohmann::json stores = nlohmann::json::object();
    for (const auto &store : path_resolver.stores()) {
        stores[store.name] = store.root.generic_string();
    }
    return stores;
}

std::string_view moduleRuntimePhaseName(ModuleRuntimePhase phase) {
    switch (phase) {
    case ModuleRuntimePhase::booting:
        return "booting";
    case ModuleRuntimePhase::running:
        return "running";
    case ModuleRuntimePhase::shutting_down:
        return "shutting_down";
    }
    return "unknown";
}

std::string generateUuidV4() {
    std::array<uint8_t, 16> bytes{};
    std::random_device random_device;
    for (auto &byte : bytes) {
        byte = static_cast<uint8_t>(random_device());
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            stream << '-';
        }
        stream << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return stream.str();
}

std::filesystem::path absoluteCapturePath(const std::string &value) {
    auto path = std::filesystem::path{value};
    if (path.is_relative()) {
        path = std::filesystem::current_path() / path;
    }
    return path;
}

std::filesystem::path weaklyCanonicalOrAbsolute(const std::filesystem::path &path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path);
}

} // namespace

#if PELICAN_WITH_OPENXR
nlohmann::json openXrStatusJsonForTesting(const OpenXr::XrDiagnosticStatus &status) {
    return openXrStatusJson(status);
}
#endif

JsonRpcHandlerError::JsonRpcHandlerError(int code, const std::string &message)
    : std::runtime_error(message), error_code{code} {}

JsonRpcHandlerError::JsonRpcHandlerError(int code, const std::string &message,
                                         nlohmann::json data)
    : std::runtime_error(message), error_code{code}, error_data{std::move(data)} {}

RpcServer::RpcServer(std::istream &input_stream, std::ostream &output_stream)
    : input{input_stream}, output{output_stream} {}

void RpcServer::setHandler(std::string method, MethodHandler handler) {
    handlers.insert_or_assign(std::move(method), std::move(handler));
}

std::string RpcServer::processLine(std::string_view line) const {
    return handleLine(line);
}

std::string RpcServer::handleLine(std::string_view line) const {
    const auto parsed = parseJsonRpcRequest(line);
    if (parsed.error) {
        return serializeJsonRpcError(*parsed.error);
    }

    const auto &request = *parsed.request;
    if (isGameLogicReloadInProgress()) {
        return serializeJsonRpcError(makeJsonRpcError(
            request.id, JsonRpcErrorCodes::applicationError,
            "RPC method '" + request.method + "' rejected while game logic DLL reload is in progress"));
    }
    const auto handler = handlers.find(request.method);
    if (handler == handlers.end()) {
        return serializeJsonRpcError(makeJsonRpcError(request.id, JsonRpcErrorCodes::methodNotFound,
                                                      "method not found: " + request.method));
    }

    try {
        return serializeJsonRpcResult(request.id, handler->second(request.params));
    } catch (const JsonRpcHandlerError &error) {
        if (error.data()) {
            return serializeJsonRpcError(
                makeJsonRpcError(request.id, error.code(), error.what(), *error.data()));
        }
        return serializeJsonRpcError(makeJsonRpcError(request.id, error.code(),
                                                       error.what()));
    } catch (const JsonRpcInvalidParamsError &error) {
        return serializeJsonRpcError(
            makeJsonRpcError(request.id, JsonRpcErrorCodes::invalidParams, error.what()));
    } catch (const std::exception &error) {
        return serializeJsonRpcError(
            makeJsonRpcError(request.id, JsonRpcErrorCodes::applicationError, error.what()));
    }
}

void RpcServer::run() {
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        output << handleLine(line) << '\n';
        output.flush();
    }
}

void configureEngineRpcHandlers(RpcServer &server, EngineRpcModules &modules,
                                const std::string &instance_id,
                                std::vector<PendingTransformUpdate> &pending_transforms,
                                EditorCommandRpcAdapter &editor_rpc) {
    server.setHandler("reload_game_logic", [&pending_transforms, &modules, &editor_rpc](const nlohmann::json &params) {
        requireObjectParams(params, "reload_game_logic");
        const auto before = configuredGameLogicStatus();
        if (!before.configured) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError,
                                      "reload_game_logic: no game logic DLL is configured");
        }
        if (modules.reload_service == nullptr) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError,
                                      "reload_game_logic: reload service is unavailable");
        }
        (void)editor_rpc.forceAbortPreview("reload");
        const auto result = modules.reload_service->applyRuntimeNow(
            watch::gameLogicReloadParticipantName);
        pending_transforms.clear();
        const auto status = configuredGameLogicStatus();
        if (!result.committed) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError,
                                      "reload_game_logic: " +
                                          (result.error.empty() ? status.last_error : result.error));
        }
        return nlohmann::json{{"generation", status.generation},
                              {"systems", status.system_count},
                              {"source", status.source.generic_string()},
                              {"participant", watch::gameLogicReloadParticipantName}};
    });

    server.setHandler("get_status", [instance_id, &modules](const nlohmann::json &params) {
        requireObjectParams(params, "get_status");
        const auto color_caps = modules.render_target.caps();
        const auto startup = modules.startup_metrics.snapshot();
        const auto module_graph = FastModuleContainer::graphSnapshot();
        const auto &debug_utils = modules.vulkan.getDebugUtils().getStatus();
        const auto renderdoc = modules.renderdoc_capture.status();
        const auto *render_timing = FastModuleContainer::tryGet<RenderTiming>();
        auto gpu_timing = render_timing != nullptr
                              ? render_timing->statusJson()
                              : disabledGpuTimingStatusJson();
        auto memory = memoryStatusJson(collectMemoryStatus(
            modules.vulkan, FastModuleContainer::tryGet<VertBufContainer>(),
            FastModuleContainer::tryGet<MaterialContainer>()));
        const nlohmann::json renderdoc_diagnostics{
            {"status", renderdoc.status},
            {"state", renderDocCaptureStateName(renderdoc.state)},
            {"reason", renderdoc.reason.empty() ? nlohmann::json(nullptr)
                                                  : nlohmann::json(renderdoc.reason)},
            {"api_version", renderdoc.api_version.empty()
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(renderdoc.api_version)},
            {"source", renderdoc.source
                           ? nlohmann::json(renderDocCaptureSourceName(*renderdoc.source))
                           : nlohmann::json(nullptr)},
        };
        return nlohmann::json{
            {"instance_id", instance_id},
            {"project_root", projectRootString(modules.path_resolver)},
            {"scene", modules.scene_loader.currentScene()},
            {"scene_source", {{"hot_reload", false},
                              {"save_method", "save_scene"}}},
            {"frame", modules.engine_time.frameIndex()},
            {"time", modules.engine_time.now()},
            {"seed", GameContext{}.seed()},
            {"xr", openXrStatusJson(modules.launch_config)},
            {"renderdoc", renderdoc.status},
            {"diagnostics", {{"renderdoc", std::move(renderdoc_diagnostics)}}},
            {"debug_utils", {{"available", debug_utils.available},
                             {"enabled", debug_utils.enabled},
                             {"reason", debug_utils.reason},
                             {"capabilities", {{"object_name", debug_utils.object_name},
                                               {"command_label", debug_utils.command_label},
                                               {"queue_label", debug_utils.queue_label}}}}},
            {"gpu_timing", std::move(gpu_timing)},
            {"memory", std::move(memory)},
            {"input", {{"recording", modules.input_sequence.isRecording()},
                       {"replaying", modules.input_sequence.isReplaying()},
                       {"replay_frame", modules.input_sequence.replayFrameIndex()},
                       {"hot_reload", modules.reload_gate.enabled()},
                       {"profile", internal::activeInputProfile()
                                       ? nlohmann::json(*internal::activeInputProfile())
                                       : nlohmann::json(nullptr)},
                       {"profiles", internal::availableInputProfiles()},
                       {"gamepad_polling", internal::gamepadPollingEnabled()}}},
            {"stores", assetStoresStatus(modules.path_resolver)},
            {"reload", modules.reload_service != nullptr
                           ? modules.reload_service->statusJson()
                           : nlohmann::json{{"state", "disabled"},
                                            {"epoch", modules.reload_gate.snapshot().epoch},
                                            {"error", nullptr}}},
            {"sprite", modules.sprite_scene != nullptr
                           ? modules.sprite_scene->statusJson()
                           : nlohmann::json{{"enabled", false}}},
            {"startup", {{"config_ms", startup.config_ms},
                         {"vulkan_ms", startup.vulkan_ms},
                         {"shaders_ms", startup.shaders_ms},
                         {"shader_cache_hits", startup.shader_cache_hits},
                         {"shader_cache_requests", startup.shader_cache_requests},
                         {"models_ms", startup.models_ms},
                         {"total_ms", startup.total_ms},
                         {"complete", startup.complete}}},
            {"modules", {{"phase", moduleRuntimePhaseName(module_graph.phase)},
                         {"creation_frozen", module_graph.creation_frozen},
                         {"initialized", module_graph.initialized_modules.size()},
                         {"dependencies", module_graph.dependencies.size()},
                         {"initialized_after_runtime_start",
                          module_graph.initialized_after_runtime_start}}},
            {"color", {{"contract", 2},
                       {"swapchain_format", formatToString(color_caps.color_format)},
                       {"path", color_caps.color_path},
                       {"readback_encoding", "srgb"},
                       {"capture", color_caps.capture_available ? "available"
                                                                 : "unavailable_windowed"}}},
        };
    });

    server.setHandler("scene_tree", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.sceneTree(params); });
    });
    server.setHandler("get_components", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getComponents(params); });
    });
    server.setHandler("list_assets", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.listAssets(params); });
    });
    server.setHandler("export_scene_snapshot", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.exportSceneSnapshot(params); });
    });
    server.setHandler("import_scene_snapshot", [&pending_transforms, &editor_rpc](
                                                   const nlohmann::json &params) {
        return invokeEditorRpc([&] {
            auto result = editor_rpc.importSceneSnapshot(params);
            pending_transforms.clear();
            return result;
        });
    });
    server.setHandler("save_scene", [&pending_transforms, &editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] {
            if (!pending_transforms.empty()) {
                throw EditorCommandError{
                    EditorCommandErrorCode::SaveBusy,
                    "scene save is busy while runtime transform updates are pending",
                };
            }
            return editor_rpc.saveScene(params);
        });
    });
    server.setHandler("open_editor_session", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.openEditorSession(params); });
    });
    server.setHandler("resume_editor_session", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.resumeEditorSession(params); });
    });
    server.setHandler("can_edit", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.canEdit(params); });
    });
    server.setHandler("can_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.canPreview(params); });
    });
    server.setHandler("edit", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.edit(params); });
    });
    server.setHandler("undo", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.undo(params); });
    });
    server.setHandler("redo", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.redo(params); });
    });
    server.setHandler("open_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.openPreview(params); });
    });
    server.setHandler("update_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.updatePreview(params); });
    });
    server.setHandler("commit_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.commitPreview(params); });
    });
    server.setHandler("abort_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.abortPreview(params); });
    });
    server.setHandler("get_edit_result", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getEditResult(params); });
    });
    server.setHandler("get_preview_result", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getPreviewResult(params); });
    });
    server.setHandler("query_journal", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.queryJournal(params); });
    });

    server.setHandler("set_seed", [](const nlohmann::json &params) {
        const auto seed = requireUnsignedIntegerParam(params, "seed", "set_seed");
        GameContext{}.setSeed(seed);
        return nlohmann::json{
            {"seed", seed},
        };
    });

    server.setHandler("set_input_profile", [](const nlohmann::json &params) {
        const auto name = requireStringParam(params, "name", "set_input_profile");
        try {
            internal::selectInputProfile(name);
        } catch (const std::exception &error) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError,
                                      "set_input_profile " + std::string{error.what()});
        }
        return nlohmann::json{{"name", name}, {"gamepad_polling", internal::gamepadPollingEnabled()}};
    });

    server.setHandler("inject_input", [&modules](const nlohmann::json &params) {
        if (modules.input_sequence.isReplaying()) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError,
                                      "inject_input cannot be combined with start_input_replay");
        }
        const auto rpc_events = parseInjectInputParams(params);
        const auto input_events = bindInjectedInputEvents(rpc_events);
        modules.input_state.queueEvents(input_events);
        return nlohmann::json{
            {"queued", input_events.size()},
        };
    });

    server.setHandler("start_input_record", [&modules](const nlohmann::json &params) {
        const auto path = absoluteCapturePath(requireStringParam(params, "path", "start_input_record"));
        try {
            const auto pose_action_names = internal::poseInputActionNames();
            modules.input_sequence.startRecording(path, modules.launch_config.fps,
                                                  pose_action_names);
        } catch (const std::exception &error) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, error.what());
        }
        return nlohmann::json{{"path", weaklyCanonicalOrAbsolute(path).generic_string()}};
    });

    server.setHandler("stop_input_record", [&modules](const nlohmann::json &params) {
        requireObjectParams(params, "stop_input_record");
        try {
            const auto result = modules.input_sequence.stopRecording();
            return nlohmann::json{{"path", weaklyCanonicalOrAbsolute(result.path).generic_string()},
                                  {"frames", result.frames},
                                  {"events", result.events}};
        } catch (const std::exception &error) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, error.what());
        }
    });

    server.setHandler("start_input_replay", [&modules, &editor_rpc](const nlohmann::json &params) {
        const auto path = absoluteCapturePath(requireStringParam(params, "path", "start_input_replay"));
        try {
            if (modules.input_state.pendingEventCount() != 0) {
                throw std::runtime_error(
                    "start_input_replay cannot begin while inject_input events are pending");
            }
            const auto pose_action_names = internal::poseInputActionNames();
            modules.input_sequence.startReplay(path, pose_action_names);
            (void)editor_rpc.forceAbortPreview("replay");
            modules.input_state.clear();
            auto &config = modules.launch_config;
            config.input_replay = true;
            modules.reload_gate.setReason(watch::ReloadGateReason::replay, true);
            config.fps = modules.input_sequence.replayFps();
            modules.engine_time.setup(EngineTime::Mode::fixed_step, 1.0 / config.fps);
        } catch (const std::exception &error) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, error.what());
        }
        return nlohmann::json{{"path", weaklyCanonicalOrAbsolute(path).generic_string()},
                              {"frames", modules.input_sequence.replayFrameCount()},
                              {"hot_reload", false}};
    });

    server.setHandler("stop_input_replay", [&modules](const nlohmann::json &params) {
        requireObjectParams(params, "stop_input_replay");
        try {
            modules.input_sequence.stopReplay();
            modules.input_state.clear();
            modules.launch_config.input_replay = false;
            modules.reload_gate.setReason(watch::ReloadGateReason::replay, false);
        } catch (const std::exception &error) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, error.what());
        }
        return nlohmann::json{{"stopped", true}};
    });

    server.setHandler("inject_event", [](const nlohmann::json &params) {
        const auto event = parseInjectEventParams(params);
        std::size_t queued = 0;
        try {
            queued = internal::emitEventByName(event.type, static_cast<const void *>(&event.payload));
        } catch (const std::exception &error) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                      "inject_event " + std::string{error.what()});
        }
        return nlohmann::json{
            {"queued", queued},
        };
    });

    server.setHandler("set_time", [&modules](const nlohmann::json &params) {
        const auto t = requireNumberParam(params, "t", "set_time");
        modules.engine_time.setTime(t);
        modules.renderer.resetTemporalHistory();
        return frameResult(modules.engine_time);
    });

    server.setHandler("update_transforms", [&pending_transforms, &modules](const nlohmann::json &params) {
        auto updates = parseTransformUpdates(params, modules.scene_loader);
        pending_transforms.insert(pending_transforms.end(), std::make_move_iterator(updates.begin()),
                                  std::make_move_iterator(updates.end()));
        return nlohmann::json{
            {"queued", updates.size()},
        };
    });

    server.setHandler("load_gltf", [&modules](const nlohmann::json &params) {
        const auto path_ref = requireStringParam(params, "path", "load_gltf");
        auto name = optionalStringParam(params, "name", "load_gltf");
        requireOptionalObjectName(name, "load_gltf");
        const auto path = modules.scene_loader.loadTransientGltf(path_ref, name);
        nlohmann::json result;
        result["path"] = path.generic_string();
        result["name"] = name ? nlohmann::json(*name) : nlohmann::json(nullptr);
        return result;
    });

    server.setHandler("load_scene", [&pending_transforms, &modules, &editor_rpc](const nlohmann::json &params) {
        const auto name = requireStringParam(params, "name", "load_scene");
        (void)editor_rpc.forceAbortPreview("scene_transition");
        modules.scene_loader.load(name);
        pending_transforms.clear();
        return nlohmann::json{
            {"name", modules.scene_loader.currentScene()},
        };
    });

    server.setHandler("set_camera", [](const nlohmann::json &params) {
        const auto name = requireStringParam(params, "name", "set_camera");
        GameContext{}.setCamera(name);
        return nlohmann::json{
            {"name", name},
        };
    });

    server.setHandler("step_frame", [&pending_transforms, &modules, &editor_rpc](const nlohmann::json &params) {
        requireObjectParams(params, "step_frame");
        flushPendingTransforms(pending_transforms, modules.scene_loader);
        modules.engine_time.advance();
        modules.vulkan.setCurrentFrameIndex(modules.engine_time.frameIndex());
        updateFrameState();
        modules.renderer.render();
        auto result = frameResult(modules.engine_time);
        result["edit_results"] = editor_rpc.takeCompletedEditResults();
        return result;
    });

    server.setHandler("render_frame", [&pending_transforms, &modules](const nlohmann::json &params) {
        requireObjectParams(params, "render_frame");
        flushPendingTransforms(pending_transforms, modules.scene_loader);
        modules.seq_player.update(modules.engine_time.now());
        modules.renderer.render();
        return frameResult(modules.engine_time);
    });

    server.setHandler("capture_gpu", [&pending_transforms, &modules](const nlohmann::json &params) {
        requireObjectParams(params, "capture_gpu");
        try {
            modules.renderdoc_capture.request(RenderDocCaptureSource::rpc,
                                              modules.launch_config.xr_active);
            const auto raw_instance = reinterpret_cast<void *>(
                static_cast<VkInstance>(modules.vulkan.getInstance()));
            const auto result = modules.renderdoc_capture.captureArmedFrame(
                modules.engine_time.frameIndex(),
                renderDocDevicePointerFromVulkanInstance(raw_instance), nullptr, [&] {
                    flushPendingTransforms(pending_transforms, modules.scene_loader);
                    modules.seq_player.update(modules.engine_time.now());
                    modules.renderer.render();
                });
            return nlohmann::json{
                {"path", result.path.generic_string()},
                {"capture_index", result.capture_index},
                {"frame", result.frame_index},
                {"timestamp", result.timestamp},
            };
        } catch (const RenderDocCaptureError &error) {
            const auto state = modules.renderdoc_capture.status();
            throw JsonRpcHandlerError{
                JsonRpcErrorCodes::renderDocCaptureError,
                "capture_gpu failed: " + std::string{error.what()},
                {{"reason", error.reason()},
                 {"state", renderDocCaptureStateName(state.state)},
                 {"source", "rpc"}}};
        }
    });

    server.setHandler("get_frame_plan", [&modules](const nlohmann::json &params) {
        requireObjectParams(params, "get_frame_plan");
        return modules.renderer.currentFramePlanJson();
    });

    server.setHandler("capture", [&modules](const nlohmann::json &params) {
        const auto path = absoluteCapturePath(requireStringParam(params, "path", "capture"));
        modules.render_target.captureLastFrameToPng(path);
        return nlohmann::json{
            {"path", weaklyCanonicalOrAbsolute(path).generic_string()},
            {"encoding", "srgb"},
            {"contract", 2},
        };
    });
}

struct EngineRpcEndpoint::Impl {
    EngineRpcModules modules;
    RpcServer server;
    std::string instance_id;
    std::vector<PendingTransformUpdate> pending_transforms;
    std::unique_ptr<EditorCommandService> editor_service;
    EditorCommandRpcAdapter editor_rpc;

    Impl(std::istream &input, std::ostream &output)
        : modules{resolveEngineRpcModules()}, server{input, output},
          instance_id{generateUuidV4()},
          editor_service{makeEditorRuntimeService()},
          editor_rpc{*editor_service} {
        configureEngineRpcHandlers(server, modules, instance_id,
                                   pending_transforms, editor_rpc);
    }
};

EngineRpcEndpoint::EngineRpcEndpoint(std::istream &input_stream,
                                     std::ostream &output_stream)
    : impl_{std::make_unique<Impl>(input_stream, output_stream)} {}

EngineRpcEndpoint::~EngineRpcEndpoint() = default;

std::string EngineRpcEndpoint::processLine(std::string_view line) const {
    return impl_->server.processLine(line);
}

void EngineRpcEndpoint::run() {
    impl_->server.run();
}

void runEngineRpcServer(std::istream &input, std::ostream &output) {
    EngineRpcEndpoint endpoint{input, output};
    endpoint.run();
}

} // namespace Pelican
