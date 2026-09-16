#pragma once

#include <nlohmann/json.hpp>
#include <set>
#include <string>

#include "graphmodule.hpp"

namespace Pelican {
// Derives native render declarations from common authoring. The caller retains
// the original document; generated resource names and shader bindings are not saved.
// Operations come from registered modules; a rendering operation this backend
// cannot lower is rejected by name, not lowered as if it were a pass.
nlohmann::json lowerRenderGraphDocument(const nlohmann::json &document,
    const std::set<std::string> &inactive_scopes = {},
    const GraphModuleRegistry &modules = builtinGraphModuleRegistry());
}
