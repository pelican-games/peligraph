#include "log.hpp"
#include "config.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/JsonSink.h>

#include <utility>

namespace Pelican {

quill::Logger *logger;
void setupLogger(bool reserve_stdout_for_protocol) {
    quill::Backend::start();

#ifdef _DEBUG
    auto sink = reserve_stdout_for_protocol ? quill::Frontend::create_or_get_sink<quill::FileSink>(logFileName)
                                            : quill::Frontend::create_or_get_sink<quill::ConsoleSink>("default_sink");
#else
    auto sink = quill::Frontend::create_or_get_sink<quill::FileSink>(logFileName);
#endif
    logger = quill::Frontend::create_or_get_logger("pelican", std::move(sink));

    LOG_INFO(logger, "pelican log start");
}

} // namespace Pelican
