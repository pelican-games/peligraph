#include "log.hpp"
#include "config.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

#include <cstdio>
#include <exception>
#include <utility>

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

} // namespace

quill::Logger *logger = nullptr;

void setupLogger(bool reserve_stdout_for_protocol) {
    quill::Backend::start();

#ifdef _DEBUG
    auto sink =
        reserve_stdout_for_protocol
            ? createFileSinkOrStderr()
            : quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                  "default_sink");
#else
    (void)reserve_stdout_for_protocol;
    auto sink = createFileSinkOrStderr();
#endif
    logger = quill::Frontend::create_or_get_logger("pelican", std::move(sink));

    LOG_INFO(logger, "pelican log start");
}

} // namespace Pelican
