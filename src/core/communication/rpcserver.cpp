#include "rpcserver.hpp"

#include "../appflow/enginetime.hpp"
#include "../ecs/core.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/scene.hpp"
#include "../os/inputstate.hpp"
#include "../playback/seqplayer.hpp"
#include "../userpublic/gamecontext.hpp"
#include "../userpublic/details/system/registerer.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/rendertarget.hpp"

#include <array>
#include <filesystem>
#include <iomanip>
#include <istream>
#include <iterator>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>

namespace Pelican {

namespace {

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
    return SceneObjectTransform{
        .pos = requireVec3Field(json, "pos", method),
        .rotation = requireQuatField(json, "rot", method),
        .scale = requireVec3Field(json, "scale", method),
    };
}

struct PendingTransformUpdate {
    std::string object;
    SceneObjectTransform transform;
};

std::vector<PendingTransformUpdate> parseTransformUpdates(const nlohmann::json &params) {
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
    auto &scene_loader = GET_MODULE(SceneLoader);
    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects.at(i).is_string()) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                      "update_transforms objects entries must be strings");
        }
        auto name = objects.at(i).get<std::string>();
        if (!scene_loader.hasObjectTransform(name)) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, "unknown object name: " + name);
        }
        updates.push_back(PendingTransformUpdate{
            .object = std::move(name),
            .transform = parseSceneObjectTransform(transforms.at(i), method),
        });
    }
    return updates;
}

void flushPendingTransforms(std::vector<PendingTransformUpdate> &pending) {
    auto &scene_loader = GET_MODULE(SceneLoader);
    for (const auto &update : pending) {
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
        }
    }
    return events;
}

nlohmann::json frameResult() {
    const auto &engine_time = GET_MODULE(EngineTime);
    return nlohmann::json{
        {"t", engine_time.now()},
        {"frame", engine_time.frameIndex()},
    };
}

void updateFrameState(EngineTime &engine_time) {
    GET_MODULE(InputState).beginFrame();
    GameContext game_context;
    GET_MODULE(ECSCore).update();
    internal::updateRegisteredGameSystems(game_context);
    GET_MODULE(SeqPlayer).update(engine_time.now());
}

std::string projectRootString() {
    const auto resolved = GET_MODULE(PathResolver).resolveProjectRef(".");
    if (const auto path = std::get_if<std::filesystem::path>(&resolved)) {
        return path->generic_string();
    }
    throw std::runtime_error("project root did not resolve to a filesystem path");
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

JsonRpcHandlerError::JsonRpcHandlerError(int code, const std::string &message)
    : std::runtime_error(message), error_code{code} {}

RpcServer::RpcServer(std::istream &input_stream, std::ostream &output_stream)
    : input{input_stream}, output{output_stream} {}

void RpcServer::setHandler(std::string method, MethodHandler handler) {
    handlers.insert_or_assign(std::move(method), std::move(handler));
}

std::string RpcServer::handleLine(std::string_view line) const {
    const auto parsed = parseJsonRpcRequest(line);
    if (parsed.error) {
        return serializeJsonRpcError(*parsed.error);
    }

    const auto &request = *parsed.request;
    const auto handler = handlers.find(request.method);
    if (handler == handlers.end()) {
        return serializeJsonRpcError(makeJsonRpcError(request.id, JsonRpcErrorCodes::methodNotFound,
                                                      "method not found: " + request.method));
    }

    try {
        return serializeJsonRpcResult(request.id, handler->second(request.params));
    } catch (const JsonRpcHandlerError &error) {
        return serializeJsonRpcError(makeJsonRpcError(request.id, error.code(), error.what()));
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

void runEngineRpcServer(std::istream &input, std::ostream &output) {
    RpcServer server{input, output};
    const auto instance_id = generateUuidV4();
    std::vector<PendingTransformUpdate> pending_transforms;

    server.setHandler("get_status", [instance_id](const nlohmann::json &params) {
        requireObjectParams(params, "get_status");
        const auto &engine_time = GET_MODULE(EngineTime);
        return nlohmann::json{
            {"instance_id", instance_id},
            {"project_root", projectRootString()},
            {"frame", engine_time.frameIndex()},
            {"time", engine_time.now()},
        };
    });

    server.setHandler("inject_input", [](const nlohmann::json &params) {
        const auto rpc_events = parseInjectInputParams(params);
        const auto input_events = bindInjectedInputEvents(rpc_events);
        GET_MODULE(InputState).queueEvents(input_events);
        return nlohmann::json{
            {"queued", input_events.size()},
        };
    });

    server.setHandler("set_time", [](const nlohmann::json &params) {
        const auto t = requireNumberParam(params, "t", "set_time");
        GET_MODULE(EngineTime).setTime(t);
        return frameResult();
    });

    server.setHandler("update_transforms", [&pending_transforms](const nlohmann::json &params) {
        auto updates = parseTransformUpdates(params);
        pending_transforms.insert(pending_transforms.end(), std::make_move_iterator(updates.begin()),
                                  std::make_move_iterator(updates.end()));
        return nlohmann::json{
            {"queued", updates.size()},
        };
    });

    server.setHandler("load_gltf", [](const nlohmann::json &params) {
        const auto path_ref = requireStringParam(params, "path", "load_gltf");
        auto name = optionalStringParam(params, "name", "load_gltf");
        requireOptionalObjectName(name, "load_gltf");
        const auto path = GET_MODULE(SceneLoader).loadTransientGltf(path_ref, name);
        nlohmann::json result;
        result["path"] = path.generic_string();
        result["name"] = name ? nlohmann::json(*name) : nlohmann::json(nullptr);
        return result;
    });

    server.setHandler("set_camera", [](const nlohmann::json &params) {
        const auto name = requireStringParam(params, "name", "set_camera");
        GameContext{}.setCamera(name);
        return nlohmann::json{
            {"name", name},
        };
    });

    server.setHandler("step_frame", [&pending_transforms](const nlohmann::json &params) {
        requireObjectParams(params, "step_frame");
        flushPendingTransforms(pending_transforms);
        auto &engine_time = GET_MODULE(EngineTime);
        engine_time.advance();
        updateFrameState(engine_time);
        GET_MODULE(Renderer).render();
        return frameResult();
    });

    server.setHandler("render_frame", [&pending_transforms](const nlohmann::json &params) {
        requireObjectParams(params, "render_frame");
        flushPendingTransforms(pending_transforms);
        GET_MODULE(SeqPlayer).update(GET_MODULE(EngineTime).now());
        GET_MODULE(Renderer).render();
        return frameResult();
    });

    server.setHandler("get_frame_plan", [](const nlohmann::json &params) {
        requireObjectParams(params, "get_frame_plan");
        return GET_MODULE(Renderer).currentFramePlanJson();
    });

    server.setHandler("capture", [](const nlohmann::json &params) {
        const auto path = absoluteCapturePath(requireStringParam(params, "path", "capture"));
        GET_MODULE(RenderTarget).captureLastFrameToPng(path);
        return nlohmann::json{
            {"path", weaklyCanonicalOrAbsolute(path).generic_string()},
        };
    });

    server.run();
}

} // namespace Pelican
