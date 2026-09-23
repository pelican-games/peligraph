#pragma once

#include <quill/LogMacros.h>
#include <quill/Logger.h>

#include <string>

namespace Pelican {

extern quill::Logger *logger;
// Where the file sink writes. Call before setupLogger. The default is
// config.hpp's logFileName in the working directory, appended to; an explicit
// path is truncated per process so one file holds one run.
void setLogFilePath(std::string path);
void setupLogger(bool reserve_stdout_for_protocol = false);

} // namespace Pelican
