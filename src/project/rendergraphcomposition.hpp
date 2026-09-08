#pragma once

#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace Pelican {
// Derives native render declarations from common authoring. The caller retains
// the original document; generated resource names and shader bindings are not saved.
nlohmann::json lowerRenderGraphDocument(const nlohmann::json &document,
    const std::set<std::string> &inactive_scopes = {});
}
