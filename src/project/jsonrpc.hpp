#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

namespace JsonRpcErrorCodes {
constexpr int parseError = -32700;
constexpr int invalidRequest = -32600;
constexpr int methodNotFound = -32601;
constexpr int invalidParams = -32602;
constexpr int applicationError = -32000;
constexpr int renderDocCaptureError = -32010;
} // namespace JsonRpcErrorCodes

struct JsonRpcRequest {
    nlohmann::json id;
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

struct JsonRpcError {
    nlohmann::json id = nullptr;
    int code = JsonRpcErrorCodes::applicationError;
    std::string message;
    std::optional<nlohmann::json> data;
};

struct JsonRpcParseResult {
    std::optional<JsonRpcRequest> request;
    std::optional<JsonRpcError> error;
};

class JsonRpcInvalidParamsError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

enum class RpcInputInjectionEventType {
    keyDown,
    keyUp,
    mouseMove,
    mouseDown,
    mouseUp,
    axis,
    gamepadButtonDown,
    gamepadButtonUp,
    gamepadAxis,
};

struct RpcInputInjectionEvent {
    RpcInputInjectionEventType type = RpcInputInjectionEventType::keyDown;
    std::string name;
    double x = 0.0;
    double y = 0.0;
    double value = 0.0;
    std::size_t gamepad = 0;
};

struct RpcEventInjection {
    std::string type;
    nlohmann::json payload = nlohmann::json::object();
};

JsonRpcParseResult parseJsonRpcRequest(std::string_view line);
JsonRpcError makeJsonRpcError(nlohmann::json id, int code, std::string message);
JsonRpcError makeJsonRpcError(nlohmann::json id, int code, std::string message, nlohmann::json data);
std::vector<RpcInputInjectionEvent> parseInjectInputParams(const nlohmann::json &params);
RpcEventInjection parseInjectEventParams(const nlohmann::json &params);
std::string serializeJsonRpcResult(const nlohmann::json &id, const nlohmann::json &result);
std::string serializeJsonRpcError(const JsonRpcError &error);

} // namespace Pelican
