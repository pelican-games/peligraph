#include <catch2/catch_test_macros.hpp>

#include "../src/core/communication/rpcserver.hpp"

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Pelican;
using namespace std::chrono_literals;

void waitForReader(WindowedRpcHost &host) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!host.readerFinished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(host.readerFinished());
}

std::vector<nlohmann::json> responseLines(const std::string &bytes) {
    std::vector<nlohmann::json> responses;
    std::istringstream lines{bytes};
    std::string line;
    while (std::getline(lines, line)) responses.push_back(nlohmann::json::parse(line));
    return responses;
}

void configureQueryHandlers(RpcServer &server, int &call_count) {
    server.setHandler("get_status", [&call_count](const nlohmann::json &) {
        ++call_count;
        return nlohmann::json{{"state", "running"}};
    });
    server.setHandler("scene_tree", [&call_count](const nlohmann::json &) {
        ++call_count;
        return nlohmann::json{{"scene_revision", 7}, {"objects", nlohmann::json::array()}};
    });
}

} // namespace

TEST_CASE("windowed RPC dispatches query requests only at a frame boundary",
          "[rpc][windowed][wp156]") {
    const std::string requests =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_status\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"scene_tree\",\"params\":{}}\n";

    std::istringstream dispatcher_input;
    std::ostringstream dispatcher_output;
    RpcServer dispatcher{dispatcher_input, dispatcher_output};
    int windowed_calls = 0;
    configureQueryHandlers(dispatcher, windowed_calls);

    std::istringstream windowed_input{requests};
    std::ostringstream windowed_output;
    WindowedRpcHost host{
        windowed_input, windowed_output,
        [&dispatcher](std::string_view line) { return dispatcher.processLine(line); }, 2};
    waitForReader(host);

    REQUIRE(host.queuedRequestCount() == 2);
    REQUIRE(windowed_calls == 0);
    REQUIRE(windowed_output.str().empty());

    REQUIRE(host.processFrameBoundary() == 2);
    REQUIRE(windowed_calls == 2);

    std::istringstream headless_input{requests};
    std::ostringstream headless_output;
    RpcServer headless{headless_input, headless_output};
    int headless_calls = 0;
    configureQueryHandlers(headless, headless_calls);
    headless.run();

    REQUIRE(headless_calls == 2);
    REQUIRE(windowed_output.str() == headless_output.str());
}

TEST_CASE("windowed RPC queue overflow returns one stable busy error per rejected line",
          "[rpc][windowed][wp156]") {
    std::istringstream dispatcher_input;
    std::ostringstream dispatcher_output;
    RpcServer dispatcher{dispatcher_input, dispatcher_output};
    int calls = 0;
    dispatcher.setHandler("get_status", [&calls](const nlohmann::json &) {
        ++calls;
        return nlohmann::json{{"state", "running"}};
    });

    std::istringstream input{
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_status\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"get_status\",\"params\":{}}\n"
        "{\"jsonrpc\":2.0,\"id\":3,\"method\":\"get_status\",\"params\":{}}\n"};
    std::ostringstream output;
    WindowedRpcHost host{
        input, output,
        [&dispatcher](std::string_view line) { return dispatcher.processLine(line); }, 1};
    waitForReader(host);

    REQUIRE(host.queuedRequestCount() == 1);
    REQUIRE(calls == 0);
    REQUIRE(host.processFrameBoundary() == 1);
    REQUIRE(calls == 1);

    const auto responses = responseLines(output.str());
    REQUIRE(responses.size() == 3);

    std::size_t busy_count = 0;
    std::size_t success_count = 0;
    for (const auto &response : responses) {
        if (response.contains("result")) {
            ++success_count;
            REQUIRE(response.at("id") == 1);
            continue;
        }
        ++busy_count;
        REQUIRE((response.at("id") == 2 || response.at("id") == 3));
        REQUIRE(response.at("error").at("code") == JsonRpcErrorCodes::applicationError);
        REQUIRE(response.at("error").at("message") == "windowed rpc request queue is busy");
        REQUIRE(response.at("error").at("data").at("reason") == "busy");
        REQUIRE(response.at("error").at("data").at("queue_capacity") == 1);
    }
    REQUIRE(busy_count == 2);
    REQUIRE(success_count == 1);
}

TEST_CASE("headless RpcServer blocking loop keeps its exact line protocol bytes",
          "[rpc][headless][wp156]") {
    std::istringstream input{
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\",\"params\":{}}\r\n"};
    std::ostringstream output;
    RpcServer server{input, output};
    server.setHandler("ping", [](const nlohmann::json &) {
        return nlohmann::json{{"ok", true}};
    });

    server.run();

    REQUIRE(output.str() ==
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"ok\":true}}\n");
}
