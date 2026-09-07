#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace Pelican {

struct GraphNodeOrigin {
    std::string instance_path;
    std::string definition;
    std::string node;
};
struct ExpandedGraphDocument {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json outputs = nlohmann::json::object();
    std::map<std::string, GraphNodeOrigin> origins;
};

// Backend-independent authoring expansion. The original document remains the
// source of truth; expanded IDs and provenance are derived, not saved back.
ExpandedGraphDocument expandGraphDocument(const nlohmann::json &document);

} // namespace Pelican
