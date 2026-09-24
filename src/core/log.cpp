#include "log.hpp"
#include "config.hpp"

#include "../project/projectdocumenttransaction.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

std::string &configuredLogFilePath() {
    static std::string path;
    return path;
}

auto createStderrSink() {
    quill::ConsoleSinkConfig config;
    config.set_stream("stderr");
    return quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
        "pelican_stderr_fallback", config);
}

auto createFileSinkOrStderr() {
    const auto &configured = configuredLogFilePath();
    const std::string file_name =
        configured.empty() ? std::string{logFileName} : configured;
    try {
        if (configured.empty()) {
            return quill::Frontend::create_or_get_sink<quill::FileSink>(
                file_name);
        }
        const auto parent = std::filesystem::path{file_name}.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        quill::FileSinkConfig config;
        config.set_open_mode('w');
        return quill::Frontend::create_or_get_sink<quill::FileSink>(
            file_name, config);
    } catch (const std::exception &error) {
        std::fprintf(
            stderr,
            "Pelican: cannot open '%s'; logging to stderr instead: %s\n",
            file_name.c_str(), error.what());
    } catch (...) {
        std::fprintf(
            stderr,
            "Pelican: cannot open '%s'; logging to stderr instead\n",
            file_name.c_str());
    }
    return createStderrSink();
}

std::vector<std::shared_ptr<quill::Sink>> createProtocolSinks() {
    auto file_sink = createFileSinkOrStderr();
    auto stderr_sink = createStderrSink();
    if (file_sink == stderr_sink) {
        return {std::move(stderr_sink)};
    }
    return {std::move(file_sink), std::move(stderr_sink)};
}

} // namespace

quill::Logger *logger = nullptr;

void setLogFilePath(std::string path) {
    configuredLogFilePath() = std::move(path);
}

void setupLogger(bool reserve_stdout_for_protocol) {
    quill::Backend::start();

#ifdef _DEBUG
    if (reserve_stdout_for_protocol) {
        logger = quill::Frontend::create_or_get_logger(
            "pelican", createProtocolSinks());
    } else {
        auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "default_sink");
        logger = quill::Frontend::create_or_get_logger(
            "pelican", std::move(sink));
    }
#else
    (void)reserve_stdout_for_protocol;
    // Keep Release diagnostics visible to launchers and Studio's Engine Log
    // while leaving stdout available for JSON-RPC and other protocol output.
    logger = quill::Frontend::create_or_get_logger(
        "pelican", createProtocolSinks());
#endif

    // The project document transaction lives in pelican_project, which has
    // no logger; its replace-route fallback is reported through this one.
    setProjectDocumentTransactionWarningSink([](std::string_view message) {
        if (logger != nullptr) {
            LOG_WARNING(logger, "{}", message);
        }
    });

    LOG_INFO(logger, "pelican log start");
}

} // namespace Pelican
