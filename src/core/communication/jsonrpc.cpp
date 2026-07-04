#include "jsonrpc.hpp"

#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view jsonrpcVersion = "2.0";

bool isSupportedId(const nlohmann::json &id) {
    return id.is_null() || id.is_string() || id.is_number_integer() || id.is_number_unsigned();
}

JsonRpcParseResult errorResult(JsonRpcError error) {
    JsonRpcParseResult result;
    result.error = std::move(error);
    return result;
}

JsonRpcParseResult requestResult(JsonRpcRequest request) {
    JsonRpcParseResult result;
    result.request = std::move(request);
    return result;
}

} // namespace

JsonRpcError makeJsonRpcError(nlohmann::json id, int code, std::string message) {
    return JsonRpcError{
        .id = std::move(id),
        .code = code,
        .message = std::move(message),
    };
}

JsonRpcError makeJsonRpcError(nlohmann::json id, int code, std::string message, nlohmann::json data) {
    return JsonRpcError{
        .id = std::move(id),
        .code = code,
        .message = std::move(message),
        .data = std::move(data),
    };
}

JsonRpcParseResult parseJsonRpcRequest(std::string_view line) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(line.begin(), line.end());
    } catch (const std::exception &) {
        return errorResult(makeJsonRpcError(nullptr, JsonRpcErrorCodes::parseError, "parse error"));
    }

    if (document.is_array()) {
        return errorResult(
            makeJsonRpcError(nullptr, JsonRpcErrorCodes::invalidRequest, "batch requests are not supported"));
    }
    if (!document.is_object()) {
        return errorResult(makeJsonRpcError(nullptr, JsonRpcErrorCodes::invalidRequest, "request must be an object"));
    }
    if (!document.contains("id")) {
        return errorResult(
            makeJsonRpcError(nullptr, JsonRpcErrorCodes::invalidRequest, "notifications are not supported"));
    }

    const auto id = document.at("id");
    if (!isSupportedId(id)) {
        return errorResult(
            makeJsonRpcError(nullptr, JsonRpcErrorCodes::invalidRequest, "request id must be string, integer, or null"));
    }
    if (document.value("jsonrpc", std::string{}) != jsonrpcVersion) {
        return errorResult(makeJsonRpcError(id, JsonRpcErrorCodes::invalidRequest, "jsonrpc must be 2.0"));
    }
    if (!document.contains("method") || !document.at("method").is_string()) {
        return errorResult(makeJsonRpcError(id, JsonRpcErrorCodes::invalidRequest, "method must be a string"));
    }

    JsonRpcRequest request;
    request.id = id;
    request.method = document.at("method").get<std::string>();
    if (document.contains("params")) {
        request.params = document.at("params");
    }
    return requestResult(std::move(request));
}

std::string serializeJsonRpcResult(const nlohmann::json &id, const nlohmann::json &result) {
    nlohmann::ordered_json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    return response.dump();
}

std::string serializeJsonRpcError(const JsonRpcError &error) {
    nlohmann::ordered_json error_object;
    error_object["code"] = error.code;
    error_object["message"] = error.message;
    if (error.data) {
        error_object["data"] = *error.data;
    }

    nlohmann::ordered_json response;
    response["jsonrpc"] = "2.0";
    response["id"] = error.id;
    response["error"] = std::move(error_object);
    return response.dump();
}

} // namespace Pelican
