#pragma once

#include <quill/LogMacros.h>
#include <quill/Logger.h>

namespace Pelican {

extern quill::Logger *logger;
void setupLogger(bool reserve_stdout_for_protocol = false);

} // namespace Pelican
