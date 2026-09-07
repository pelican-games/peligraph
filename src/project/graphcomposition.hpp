#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include "logicalrendertype.hpp"

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

struct GraphInputContract {
    LogicalTypePattern accepted_type;
    bool required = true;
};
struct GraphOperationContract {
    std::map<std::string, GraphInputContract> inputs;
    std::map<std::string, LogicalType> outputs;
};
using GraphOperationContracts = std::map<std::string, GraphOperationContract>;

// Checks expanded edges, including edges crossing instance boundaries. Operation
// providers own contracts; authoring cannot override an operation's output type.
std::map<std::string, LogicalType> validateGraphPorts(
    const ExpandedGraphDocument &graph, const LogicalTypeRegistry &registry,
    const GraphOperationContracts &operations);

} // namespace Pelican
