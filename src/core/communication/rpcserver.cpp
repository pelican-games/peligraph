#include "rpcserver.hpp"

#include "../appflow/enginetime.hpp"
#include "../ecs/core.hpp"
#include "../os/inputstate.hpp"
#include "../playback/seqplayer.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/rendertarget.hpp"

#include <filesystem>
#include <istream>
#include <ostream>
#include <utility>

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

nlohmann::json frameResult() {
    const auto &engine_time = GET_MODULE(EngineTime);
    return nlohmann::json{
        {"t", engine_time.now()},
        {"frame", engine_time.frameIndex()},
    };
}

void updateFrameState(EngineTime &engine_time) {
    GET_MODULE(InputState).clear();
    GET_MODULE(ECSCore).update();
    GET_MODULE(SeqPlayer).update(engine_time.now());
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

    server.setHandler("set_time", [](const nlohmann::json &params) {
        const auto t = requireNumberParam(params, "t", "set_time");
        GET_MODULE(EngineTime).setTime(t);
        return frameResult();
    });

    server.setHandler("step_frame", [](const nlohmann::json &params) {
        requireObjectParams(params, "step_frame");
        auto &engine_time = GET_MODULE(EngineTime);
        engine_time.advance();
        updateFrameState(engine_time);
        GET_MODULE(Renderer).render();
        return frameResult();
    });

    server.setHandler("render_frame", [](const nlohmann::json &params) {
        requireObjectParams(params, "render_frame");
        GET_MODULE(InputState).clear();
        GET_MODULE(SeqPlayer).update(GET_MODULE(EngineTime).now());
        GET_MODULE(Renderer).render();
        return frameResult();
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
