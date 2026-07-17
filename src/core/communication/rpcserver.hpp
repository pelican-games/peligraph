#pragma once

#include "../../project/jsonrpc.hpp"

#include <functional>
#include <iosfwd>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace Pelican {

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

  public:
    JsonRpcHandlerError(int code, const std::string &message);
    int code() const { return error_code; }
};

class RpcServer {
  public:
    using MethodHandler = std::function<nlohmann::json(const nlohmann::json &)>;

  private:
    std::istream &input;
    std::ostream &output;
    std::unordered_map<std::string, MethodHandler> handlers;

    std::string handleLine(std::string_view line) const;

  public:
    RpcServer(std::istream &input_stream, std::ostream &output_stream);

    void setHandler(std::string method, MethodHandler handler);
    void run();
};

void runEngineRpcServer(std::istream &input, std::ostream &output);

} // namespace Pelican
