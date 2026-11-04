#pragma once

#include "../../core/output/outputsinktable.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace Pelican {

struct OutputLoweringResult {
    nlohmann::json document;
    nlohmann::json report;
    OutputSinkTable sinks;
    bool changed = false;
};

// VO-1a: removes every root `output.file` node after resolving its concrete
// producer and target, adds the target's retain, and returns the public sink
// report. The authored document is not modified.
OutputLoweringResult lowerOutputNodes(const nlohmann::json &document);

} // namespace Pelican
