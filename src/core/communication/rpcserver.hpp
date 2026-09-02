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

  private:
    using Handler = std::variant<MethodHandler, OrderedMethodHandler>;
    std::istream &input;
    std::ostream &output;
    std::unordered_map<std::string, Handler> handlers;

    std::string handleLine(std::string_view line) const;

  public:
    RpcServer(std::istream &input_stream, std::ostream &output_stream);

    void setHandler(std::string method, MethodHandler handler);
    void setOrderedHandler(std::string method,
                           OrderedMethodHandler handler);
    std::string processLine(std::string_view line) const;
    void run();
};

namespace internal {

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
