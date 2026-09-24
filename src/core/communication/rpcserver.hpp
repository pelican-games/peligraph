#pragma once

#include "../../project/jsonrpc.hpp"

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace Pelican {

class PhysWorld;

namespace watch {
class ReloadService;
}

struct PreserveJsonRpcDataOrder {};

#if PELICAN_WITH_OPENXR
namespace OpenXr {
struct XrDiagnosticStatus;
}

// Pure projection used by the headless OpenXR fake fixture. The production
// get_status handler obtains the same snapshot from SessionRuntime.
nlohmann::json openXrStatusJsonForTesting(const OpenXr::XrDiagnosticStatus &status);
#endif

class JsonRpcHandlerError : public std::runtime_error {
    int error_code;
    std::optional<nlohmann::json> error_data;
    std::optional<nlohmann::ordered_json> ordered_error_data;

  public:
    JsonRpcHandlerError(int code, const std::string &message);
    JsonRpcHandlerError(int code, const std::string &message, nlohmann::json data);
    JsonRpcHandlerError(int code, const std::string &message,
                        nlohmann::ordered_json data,
                        PreserveJsonRpcDataOrder);
    int code() const { return error_code; }
    const std::optional<nlohmann::json> &data() const { return error_data; }
    const std::optional<nlohmann::ordered_json> &orderedData() const {
        return ordered_error_data;
    }
};

class RpcServer {
  public:
    using MethodHandler = std::function<nlohmann::json(const nlohmann::json &)>;
    using OrderedMethodHandler =
        std::function<nlohmann::ordered_json(const nlohmann::json &)>;

    // Consulted once per parsed request, ahead of dispatch. A returned string
    // is the refusal message reported as an application error; std::nullopt
    // lets the request through. The transport has no reason of its own to
    // refuse a request, so whoever wires the handlers supplies the reasons it
    // must honour.
    using RequestGate =
        std::function<std::optional<std::string>(std::string_view method)>;

    // An ordered handler's result is normally serialized through
    // nlohmann::json, which sorts object keys. A handler whose response embeds
    // a payload whose key order is part of the wire contract declares that at
    // registration time with this predicate: when it holds for a result, the
    // response is dumped with its insertion order intact. Without a policy the
    // response is key-sorted like any other. The transport therefore never has
    // to recognise which payloads are order-significant.
    using OrderedResponseOrderPolicy =
        std::function<bool(const nlohmann::ordered_json &result)>;

  private:
    struct OrderedHandlerEntry {
        OrderedMethodHandler handler;
        OrderedResponseOrderPolicy preserve_order;
    };
    using Handler = std::variant<MethodHandler, OrderedHandlerEntry>;
    std::istream &input;
    std::ostream &output;
    std::unordered_map<std::string, Handler> handlers;
    RequestGate request_gate;

    std::string handleLine(std::string_view line) const;

  public:
    RpcServer(std::istream &input_stream, std::ostream &output_stream);

    void setRequestGate(RequestGate gate);
    void setHandler(std::string method, MethodHandler handler);
    void setOrderedHandler(std::string method,
                           OrderedMethodHandler handler,
                           OrderedResponseOrderPolicy preserve_order = {});
    std::string processLine(std::string_view line) const;
    void run();
};

namespace internal {

void configurePhysicsRpcHandlers(RpcServer &server, PhysWorld &physics);

struct GameLogicReloadRpcHooks {
    std::function<void()> before_apply;
    std::function<void()> after_apply;
};

// Shared by the full engine endpoint and the non-GPU wire acceptance test.
void configureGameLogicReloadRpcHandler(
    RpcServer &server, watch::ReloadService *reload_service,
    GameLogicReloadRpcHooks hooks = {});
nlohmann::json gameLogicParticipantRpcStatus(
    const watch::ReloadService *reload_service);

struct ShaderReloadRpcHooks {
    std::function<void()> before_apply;
};

// F-5. reload_shaders: the shader participant applied now, on request.
void configureShaderReloadRpcHandler(
    RpcServer &server, watch::ReloadService *reload_service,
    ShaderReloadRpcHooks hooks = {});

// Shared by the production load_scene handler and the non-GPU wire contract
// test. The loader callback may throw a type-preserving resolution failure;
// this boundary is the sole conversion to JSON-RPC error.data.
void loadSceneResolutionBoundary(
    std::string_view name,
    const std::function<void(std::string_view)> &load_scene);

} // namespace internal

// Owns one stateful engine RPC dispatcher. Headless uses run(); the windowed
// host calls processLine() only at a frame boundary.
class EngineRpcEndpoint {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    EngineRpcEndpoint(std::istream &input_stream, std::ostream &output_stream);
    ~EngineRpcEndpoint();

    EngineRpcEndpoint(const EngineRpcEndpoint &) = delete;
    EngineRpcEndpoint &operator=(const EngineRpcEndpoint &) = delete;

    std::string processLine(std::string_view line) const;
    void run();
};

// A single-connection stdio transport for an interactive engine. The reader
// thread only enqueues complete lines. processFrameBoundary() owns dispatch
// and accepted-response flushing on the engine thread.
class WindowedRpcHost {
  public:
    using LineHandler = std::function<std::string(std::string_view)>;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    WindowedRpcHost(std::istream &input_stream, std::ostream &output_stream,
                    LineHandler handler, std::size_t queue_capacity);
    ~WindowedRpcHost();

    WindowedRpcHost(const WindowedRpcHost &) = delete;
    WindowedRpcHost &operator=(const WindowedRpcHost &) = delete;

    std::size_t processFrameBoundary();
    std::size_t queuedRequestCount() const;
    bool readerFinished() const;
};

inline constexpr std::size_t defaultWindowedRpcQueueCapacity = 64;

void runEngineRpcServer(std::istream &input, std::ostream &output);

} // namespace Pelican
