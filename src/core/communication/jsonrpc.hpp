#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace Pelican {

namespace JsonRpcErrorCodes {
constexpr int parseError = -32700;
constexpr int invalidRequest = -32600;
constexpr int methodNotFound = -32601;
constexpr int invalidParams = -32602;
constexpr int applicationError = -32000;
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

JsonRpcParseResult parseJsonRpcRequest(std::string_view line);
JsonRpcError makeJsonRpcError(nlohmann::json id, int code, std::string message);
JsonRpcError makeJsonRpcError(nlohmann::json id, int code, std::string message, nlohmann::json data);
std::string serializeJsonRpcResult(const nlohmann::json &id, const nlohmann::json &result);
std::string serializeJsonRpcError(const JsonRpcError &error);

} // namespace Pelican
