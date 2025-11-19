#include "log.hpp"
#include "config.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/JsonSink.h>

namespace Pelican {

quill::Logger *logger;
void setupLogger() {
    quill::Backend::start();

#ifdef _DEBUG
    auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("default_sink");
#else
    auto sink = quill::Frontend::create_or_get_sink<quill::FileSink>(logFileName);
#endif
    logger = quill::Frontend::create_or_get_logger("pelican", std::move(sink));

    LOG_INFO(logger, "pelican log start");
}

} // namespace Pelican
