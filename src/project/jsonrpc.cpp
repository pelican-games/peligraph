#include "jsonrpc.hpp"

#include <cstddef>
#include <cstdint>
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

std::string eventContext(std::size_t index) {
    return "inject_input events[" + std::to_string(index) + "]";
}

const nlohmann::json &requireObjectParams(const nlohmann::json &params, const std::string &method) {
    if (!params.is_object()) {
        throw JsonRpcInvalidParamsError(method + " params must be an object");
    }
    return params;
}

const nlohmann::json &requireArrayField(const nlohmann::json &object, const char *name, const std::string &method) {
    if (!object.contains(name) || !object.at(name).is_array()) {
        throw JsonRpcInvalidParamsError(method + " params requires array field '" + std::string{name} + "'");
    }
    return object.at(name);
}

std::string requireStringField(const nlohmann::json &object, const char *name, const std::string &context) {
    if (!object.contains(name) || !object.at(name).is_string()) {
        throw JsonRpcInvalidParamsError(context + " requires string field '" + std::string{name} + "'");
    }
    auto value = object.at(name).get<std::string>();
    if (value.empty()) {
        throw JsonRpcInvalidParamsError(context + " field '" + std::string{name} + "' must not be empty");
    }
    return value;
}

double requireNumberField(const nlohmann::json &object, const char *name, const std::string &context) {
    if (!object.contains(name) || !object.at(name).is_number()) {
        throw JsonRpcInvalidParamsError(context + " requires numeric field '" + std::string{name} + "'");
    }
    return object.at(name).get<double>();
}

std::size_t optionalGamepadField(const nlohmann::json &object, const std::string &context) {
    if (!object.contains("pad")) {
        return 0;
    }
    if (!object.at("pad").is_number_integer() || object.at("pad").get<std::int64_t>() < 0) {
        throw JsonRpcInvalidParamsError(context + " field 'pad' must be an unsigned integer");
    }
    return object.at("pad").get<std::size_t>();
}

RpcInputInjectionEvent parseInjectInputEvent(const nlohmann::json &event_json, std::size_t index) {
    const auto context = eventContext(index);
    if (!event_json.is_object()) {
        throw JsonRpcInvalidParamsError(context + " must be an object");
    }

    const auto type = requireStringField(event_json, "type", context);
    if (type == "key_down" || type == "key_up") {
        return RpcInputInjectionEvent{
            .type = type == "key_down" ? RpcInputInjectionEventType::keyDown : RpcInputInjectionEventType::keyUp,
            .name = requireStringField(event_json, "key", context + " type '" + type + "'"),
        };
    }
    if (type == "mouse_move") {
        return RpcInputInjectionEvent{
            .type = RpcInputInjectionEventType::mouseMove,
            .x = requireNumberField(event_json, "x", context + " type 'mouse_move'"),
            .y = requireNumberField(event_json, "y", context + " type 'mouse_move'"),
        };
    }
    if (type == "mouse_down" || type == "mouse_up") {
        return RpcInputInjectionEvent{
            .type = type == "mouse_down" ? RpcInputInjectionEventType::mouseDown
                                         : RpcInputInjectionEventType::mouseUp,
            .name = requireStringField(event_json, "button", context + " type '" + type + "'"),
        };
    }
    if (type == "axis") {
        return RpcInputInjectionEvent{
            .type = RpcInputInjectionEventType::axis,
            .name = requireStringField(event_json, "axis", context + " type 'axis'"),
            .value = requireNumberField(event_json, "value", context + " type 'axis'"),
        };
    }
    if (type == "pad_button_down" || type == "pad_button_up") {
        return RpcInputInjectionEvent{
            .type = type == "pad_button_down" ? RpcInputInjectionEventType::gamepadButtonDown
                                               : RpcInputInjectionEventType::gamepadButtonUp,
            .name = requireStringField(event_json, "button", context + " type '" + type + "'"),
            .gamepad = optionalGamepadField(event_json, context + " type '" + type + "'"),
        };
    }
    if (type == "pad_axis") {
        return RpcInputInjectionEvent{
            .type = RpcInputInjectionEventType::gamepadAxis,
            .name = requireStringField(event_json, "axis", context + " type 'pad_axis'"),
            .value = requireNumberField(event_json, "value", context + " type 'pad_axis'"),
            .gamepad = optionalGamepadField(event_json, context + " type 'pad_axis'"),
        };
    }

    throw JsonRpcInvalidParamsError(context + " has unknown event type '" + type + "'");
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

std::vector<RpcInputInjectionEvent> parseInjectInputParams(const nlohmann::json &params) {
    const auto &object = requireObjectParams(params, "inject_input");
    const auto &events_json = requireArrayField(object, "events", "inject_input");

    std::vector<RpcInputInjectionEvent> events;
    events.reserve(events_json.size());
    for (std::size_t i = 0; i < events_json.size(); ++i) {
        events.push_back(parseInjectInputEvent(events_json.at(i), i));
    }
    return events;
}

RpcEventInjection parseInjectEventParams(const nlohmann::json &params) {
    const auto &object = requireObjectParams(params, "inject_event");
    const auto type = requireStringField(object, "type", "inject_event");
    if (!object.contains("payload") || !object.at("payload").is_object()) {
        throw JsonRpcInvalidParamsError("inject_event params requires object field 'payload'");
    }
    return RpcEventInjection{
        .type = type,
        .payload = object.at("payload"),
    };
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
