#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <functional>
#include <set>
#include "logicalrendertype.hpp"
#include "graphinstanceowner.hpp"

namespace Pelican {

// Named failures of the common graph's registration contract, shaped like
// PrefabError: a versioned code plus a JSON detail. A document that names an
// operation or a type no module registered is rejected by code, so a caller
// that removed a module can tell that apart from a malformed document.
class GraphRegistryError : public std::runtime_error {
    std::string code_;
    nlohmann::json detail_;

  public:
    GraphRegistryError(std::string code, nlohmann::json detail);
    const std::string &code() const noexcept { return code_; }
    const nlohmann::json &detail() const noexcept { return detail_; }
};

struct GraphNodeOrigin {
    std::string instance_path;
    std::string definition;
    std::string node;
};
struct ExpandedGraphDocument {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json outputs = nlohmann::json::object();
    std::map<std::string, GraphNodeOrigin> origins;
    GraphInstanceOwners instance_owners;
    // Required live instance scopes, including dependencies crossing an alias.
    // Derived metadata; never written into the authoring document.
    std::map<std::string, std::set<std::string>> node_scopes;
    std::map<std::string, std::set<std::string>> output_scopes;
};

// Backend-independent authoring expansion. The original document remains the
// source of truth; expanded IDs and provenance are derived, not saved back.
ExpandedGraphDocument expandGraphDocument(const nlohmann::json &document);

// Edit owned root instances in the authoring document. References to removed
// instances must be removed explicitly; unrelated nodes and definitions survive.
nlohmann::json editGraphInstances(const nlohmann::json &document,
    const nlohmann::json &attachments, const nlohmann::json &detachments);

struct GraphInputContract {
    LogicalTypePattern accepted_type;
    bool required = true;
};
// Which execution domain evaluates an operation. Declared by the module that
// registers it, so adding a CPU value operation does not mean teaching the
// partition a new operation name.
enum class GraphExecutionDomain : std::uint8_t { values, rendering };
std::string_view graphExecutionDomainName(GraphExecutionDomain domain);
// How a result is produced across evaluations. Recorded here and published in
// the module catalog; checking it belongs to the evaluation-order work.
enum class GraphOperationEvaluation : std::uint8_t { direct, replay, baked, recorded };
std::string_view graphOperationEvaluationName(GraphOperationEvaluation evaluation);
// What changing a parameter costs the backend. Recorded and published; the
// editor acts on it later.
enum class GraphParameterChange : std::uint8_t { value, recompile, rebuild };
std::string_view graphParameterChangeName(GraphParameterChange change);
struct GraphOperationContract {
    std::map<std::string, GraphInputContract> inputs;
    std::map<std::string, LogicalType> outputs;
    GraphExecutionDomain domain = GraphExecutionDomain::values;
    GraphOperationEvaluation evaluation = GraphOperationEvaluation::direct;
    GraphParameterChange parameter_change = GraphParameterChange::value;
    std::map<std::string, GraphParameterChange> parameter_changes;
};
using GraphOperationContracts = std::map<std::string, GraphOperationContract>;
using GraphOperationContractResolver =
    std::function<GraphOperationContract(const nlohmann::json &)>;

// Checks expanded edges, including edges crossing instance boundaries. Operation
// providers own contracts; authoring cannot override an operation's output type.
std::map<std::string, LogicalType> validateGraphPorts(
    const ExpandedGraphDocument &graph, const LogicalTypeRegistry &registry,
    const GraphOperationContracts &operations);
std::map<std::string, LogicalType> validateGraphPorts(
    const ExpandedGraphDocument &graph, const LogicalTypeRegistry &registry,
    const GraphOperationContractResolver &resolve);

} // namespace Pelican
