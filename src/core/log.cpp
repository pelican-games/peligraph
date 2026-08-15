#include "log.hpp"
#include "config.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

auto createStderrSink() {
    quill::ConsoleSinkConfig config;
    config.set_stream("stderr");
    return quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
        "pelican_stderr_fallback", config);
}

auto createFileSinkOrStderr() {
    try {
        return quill::Frontend::create_or_get_sink<quill::FileSink>(
            logFileName);
    } catch (const std::exception &error) {
        std::fprintf(
            stderr,
            "Pelican: cannot open '%s'; logging to stderr instead: %s\n",
            logFileName, error.what());
    } catch (...) {
        std::fprintf(
            stderr,
            "Pelican: cannot open '%s'; logging to stderr instead\n",
            logFileName);
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
    auto sink = createFileSinkOrStderr();
    logger = quill::Frontend::create_or_get_logger("pelican", std::move(sink));
#endif

    LOG_INFO(logger, "pelican log start");
}

} // namespace Pelican
