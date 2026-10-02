#include "../src/project/jsonrpc.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace Pelican {

namespace {

nlohmann::json parseResponse(const std::string &line) { return nlohmann::json::parse(line); }

std::string invalidParamsMessage(const nlohmann::json &params) {
    try {
        (void)parseInjectInputParams(params);
    } catch (const JsonRpcInvalidParamsError &error) {
        return error.what();
    }
    return {};
}

std::string invalidEventParamsMessage(const nlohmann::json &params) {
    try {
        (void)parseInjectEventParams(params);
    } catch (const JsonRpcInvalidParamsError &error) {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("JSON-RPC request parser accepts valid request objects", "[jsonrpc]") {
    const auto parsed =
        parseJsonRpcRequest(R"json({"jsonrpc":"2.0","id":7,"method":"set_time","params":{"t":1.5}})json");

    REQUIRE(parsed.request.has_value());
    REQUIRE_FALSE(parsed.error.has_value());
    REQUIRE(parsed.request->id == 7);
    REQUIRE(parsed.request->method == "set_time");
    REQUIRE(parsed.request->params.at("t") == 1.5);
}

TEST_CASE("JSON-RPC parser maps malformed protocol input to required error codes", "[jsonrpc]") {
    const auto parse_error = parseJsonRpcRequest("{not json");
    REQUIRE(parse_error.error.has_value());
    REQUIRE(parse_error.error->code == JsonRpcErrorCodes::parseError);

    const auto batch_error = parseJsonRpcRequest(R"json([{"jsonrpc":"2.0","id":1,"method":"step_frame"}])json");
    REQUIRE(batch_error.error.has_value());
    REQUIRE(batch_error.error->code == JsonRpcErrorCodes::invalidRequest);

    const auto notification_error = parseJsonRpcRequest(R"json({"jsonrpc":"2.0","method":"step_frame"})json");
    REQUIRE(notification_error.error.has_value());
    REQUIRE(notification_error.error->code == JsonRpcErrorCodes::invalidRequest);

    const auto version_error = parseJsonRpcRequest(R"json({"jsonrpc":"1.0","id":1,"method":"step_frame"})json");
    REQUIRE(version_error.error.has_value());
    REQUIRE(version_error.error->id == 1);
    REQUIRE(version_error.error->code == JsonRpcErrorCodes::invalidRequest);

    const auto version_type_error =
        parseJsonRpcRequest(
            R"json({"jsonrpc":2.0,"id":2,"method":"step_frame"})json");
    REQUIRE(version_type_error.error.has_value());
    REQUIRE(version_type_error.error->id == 2);
    REQUIRE(version_type_error.error->code ==
            JsonRpcErrorCodes::invalidRequest);
}

TEST_CASE("JSON-RPC response serialization emits success and all required error codes", "[jsonrpc]") {
    const auto success = parseResponse(serializeJsonRpcResult(3, nlohmann::json{{"ok", true}}));
    REQUIRE(success.at("jsonrpc") == "2.0");
    REQUIRE(success.at("id") == 3);
    REQUIRE(success.at("result").at("ok") == true);

    const auto parse_error =
        parseResponse(serializeJsonRpcError(makeJsonRpcError(nullptr, JsonRpcErrorCodes::parseError, "parse error")));
    REQUIRE(parse_error.at("error").at("code") == JsonRpcErrorCodes::parseError);

    const auto invalid_request = parseResponse(
        serializeJsonRpcError(makeJsonRpcError(nullptr, JsonRpcErrorCodes::invalidRequest, "invalid request")));
    REQUIRE(invalid_request.at("error").at("code") == JsonRpcErrorCodes::invalidRequest);

    const auto method_not_found = parseResponse(
        serializeJsonRpcError(makeJsonRpcError("abc", JsonRpcErrorCodes::methodNotFound, "method not found")));
    REQUIRE(method_not_found.at("id") == "abc");
    REQUIRE(method_not_found.at("error").at("code") == JsonRpcErrorCodes::methodNotFound);

    const auto invalid_params = parseResponse(
        serializeJsonRpcError(makeJsonRpcError(4, JsonRpcErrorCodes::invalidParams, "invalid params")));
    REQUIRE(invalid_params.at("error").at("code") == JsonRpcErrorCodes::invalidParams);

    const auto app_error = parseResponse(serializeJsonRpcError(makeJsonRpcError(
        5, JsonRpcErrorCodes::applicationError, "application error", nlohmann::json{{"detail", "capture failed"}})));
    REQUIRE(app_error.at("error").at("code") == JsonRpcErrorCodes::applicationError);
    REQUIRE(app_error.at("error").at("data").at("detail") == "capture failed");
}

TEST_CASE("inject_input params parser accepts key, mouse, and axis events", "[jsonrpc]") {
    const auto events = parseInjectInputParams(nlohmann::json{
        {"events",
         nlohmann::json::array({
             {{"type", "key_down"}, {"key", "W"}},
             {{"type", "key_up"}, {"key", "w"}},
             {{"type", "mouse_move"}, {"x", 12.5}, {"y", 7.0}},
             {{"type", "mouse_down"}, {"button", "left"}},
             {{"type", "mouse_up"}, {"button", "left"}},
             {{"type", "axis"}, {"axis", "mouse_delta_x"}, {"value", -4.0}},
             {{"type", "pad_button_down"}, {"button", "a"}, {"pad", 1}},
             {{"type", "pad_axis"}, {"axis", "left_x"}, {"value", 0.75}},
         })},
    });

    REQUIRE(events.size() == 8);
    REQUIRE(events.at(0).type == RpcInputInjectionEventType::keyDown);
    REQUIRE(events.at(0).name == "W");
    REQUIRE(events.at(1).type == RpcInputInjectionEventType::keyUp);
    REQUIRE(events.at(2).type == RpcInputInjectionEventType::mouseMove);
    REQUIRE(events.at(2).x == 12.5);
    REQUIRE(events.at(2).y == 7.0);
    REQUIRE(events.at(3).type == RpcInputInjectionEventType::mouseDown);
    REQUIRE(events.at(4).type == RpcInputInjectionEventType::mouseUp);
    REQUIRE(events.at(5).type == RpcInputInjectionEventType::axis);
    REQUIRE(events.at(5).name == "mouse_delta_x");
    REQUIRE(events.at(5).value == -4.0);
    REQUIRE(events.at(6).type == RpcInputInjectionEventType::gamepadButtonDown);
    REQUIRE(events.at(6).gamepad == 1);
    REQUIRE(events.at(7).type == RpcInputInjectionEventType::gamepadAxis);
}

TEST_CASE("inject_input params parser reports named invalid params", "[jsonrpc]") {
    auto message = invalidParamsMessage(nlohmann::json::array());
    REQUIRE(message.find("inject_input params") != std::string::npos);

    message = invalidParamsMessage(nlohmann::json{{"events", nlohmann::json::array({{{"type", "teleport"}}})}});
    REQUIRE(message.find("teleport") != std::string::npos);

    message = invalidParamsMessage(nlohmann::json{{"events", nlohmann::json::array({{{"type", "key_down"}}})}});
    REQUIRE(message.find("key") != std::string::npos);

    message = invalidParamsMessage(
        nlohmann::json{{"events", nlohmann::json::array({{{"type", "axis"}, {"axis", "mouse_delta_x"}}})}});
    REQUIRE(message.find("value") != std::string::npos);
}

TEST_CASE("inject_event params parser accepts type and payload", "[jsonrpc]") {
    const auto event = parseInjectEventParams(nlohmann::json{
        {"type", "Wp56InjectedEvent"},
        {"payload", {{"seed", 5609}}},
    });

    REQUIRE(event.type == "Wp56InjectedEvent");
    REQUIRE(event.payload.at("seed").get<int>() == 5609);
}

TEST_CASE("inject_event params parser reports named invalid params", "[jsonrpc]") {
    auto message = invalidEventParamsMessage(nlohmann::json{{"payload", nlohmann::json::object()}});
    REQUIRE(message.find("inject_event") != std::string::npos);
    REQUIRE(message.find("type") != std::string::npos);

    message = invalidEventParamsMessage(nlohmann::json{{"type", "Wp56InjectedEvent"}});
    REQUIRE(message.find("inject_event") != std::string::npos);
    REQUIRE(message.find("payload") != std::string::npos);
}

namespace {

// The envelopes as they were written before FP-2: the response copied into an
// ordered_json and dumped. FP-2 writes the same bytes without the copy.
std::string orderedEnvelopeResult(const nlohmann::json &id, const nlohmann::json &result) {
    nlohmann::ordered_json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    return response.dump();
}

std::string orderedEnvelopeError(const JsonRpcError &error) {
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

// A frame-plan-like payload whose keys are written out of order, with the
// number, string and container shapes the real responses carry.
nlohmann::ordered_json orderedPlanLikePayload() {
    nlohmann::ordered_json nodes = nlohmann::ordered_json::array();
    for (int i = 0; i < 300; ++i) {
        nodes.push_back({
            {"name", "pass_" + std::to_string(i)},
            {"kind", i % 2 == 0 ? "render" : "compute"},
            {"level", i},
            {"extent", {{"width", 1920u}, {"height", 1080u}}},
            {"scale", 1.0 / static_cast<double>(i + 3)},
            {"after", nlohmann::ordered_json::array({i - 1})},
            {"diagnostics", nlohmann::ordered_json::array()},
            {"bindings", nlohmann::ordered_json::object()},
        });
    }
    return {
        {"version", 1},
        {"nodes", std::move(nodes)},
        {"zeta", nullptr},
        {"alpha", true},
        {"numbers",
         {0.1, 1.0, -0.0, 1e300, 5e-324, 1.5f, std::nan(""),
          std::numeric_limits<std::int64_t>::min(), -1,
          std::numeric_limits<std::uint64_t>::max()}},
        {"strings", {"", "\u3042\u00e9", "\"\\n\t\x01/", "\xF0\x9F\x99\x82"}},
        {"Upper", "sorts before lower"},
        {"_under", {{"b", 2}, {"a", 1}}},
    };
}

} // namespace

TEST_CASE("JSON-RPC FP-2 responses are the same bytes as the ordered_json envelope they replace",
          "[jsonrpc]") {
    const auto ordered = orderedPlanLikePayload();
    // Both ways a result reaches serializeJsonRpcResult: built as json, and an
    // ordered handler's result converted to json (its keys then sort).
    const std::vector<nlohmann::json> results{
        nlohmann::json(ordered),
        nlohmann::json::parse(ordered.dump()),
        nlohmann::json{{"ok", true}},
        nlohmann::json::object(),
        nlohmann::json::array(),
        nlohmann::json(nullptr),
        nlohmann::json("text \u3042"),
        nlohmann::json(2.5),
    };
    const std::vector<nlohmann::json> ids{
        nlohmann::json(0),
        nlohmann::json(-7),
        nlohmann::json(std::numeric_limits<std::uint64_t>::max()),
        nlohmann::json("s-\u00e9 \u3042\"\n"),
        nlohmann::json(""),
        nlohmann::json(nullptr),
    };
    REQUIRE(nlohmann::json(ordered).dump() != ordered.dump());
    for (const auto &id : ids) {
        for (const auto &result : results) {
            REQUIRE(serializeJsonRpcResult(id, result) == orderedEnvelopeResult(id, result));
        }
    }
    REQUIRE(serializeJsonRpcResult(3, nlohmann::json{{"ok", true}}) ==
            R"({"jsonrpc":"2.0","id":3,"result":{"ok":true}})");

    const std::vector<JsonRpcError> errors{
        makeJsonRpcError(nullptr, JsonRpcErrorCodes::parseError, "parse error"),
        makeJsonRpcError("abc", JsonRpcErrorCodes::methodNotFound, "method not found: x\n\"\u3042"),
        makeJsonRpcError(4, JsonRpcErrorCodes::invalidParams, ""),
        makeJsonRpcError(std::numeric_limits<std::uint64_t>::max(), 1, "positive code"),
        makeJsonRpcError(5, JsonRpcErrorCodes::applicationError, "with data",
                         nlohmann::json{{"error_fields", {{"z", 1}, {"a", {1.25, "x"}}}},
                                        {"error_code", "busy@1"}}),
        makeJsonRpcError(6, JsonRpcErrorCodes::applicationError, "array data", nlohmann::json::array({3, 1})),
        makeJsonRpcError(7, JsonRpcErrorCodes::applicationError, "null data", nlohmann::json(nullptr)),
        makeJsonRpcError("big", JsonRpcErrorCodes::applicationError, "plan data", nlohmann::json(ordered)),
    };
    for (const auto &error : errors) {
        REQUIRE(serializeJsonRpcError(error) == orderedEnvelopeError(error));
    }
    REQUIRE(serializeJsonRpcError(makeJsonRpcError(nullptr, JsonRpcErrorCodes::parseError, "parse error")) ==
            R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse error"}})");

    // A string that is not UTF-8 is refused by both, as before.
    const nlohmann::json bad{{"s", std::string{"\xFF"}}};
    REQUIRE_THROWS_AS(orderedEnvelopeResult(1, bad), nlohmann::json::type_error);
    REQUIRE_THROWS_AS(serializeJsonRpcResult(1, bad), nlohmann::json::type_error);
    REQUIRE_THROWS_AS(serializeJsonRpcError(makeJsonRpcError(1, 1, std::string{"\xFF"})),
                      nlohmann::json::type_error);
}

} // namespace Pelican
