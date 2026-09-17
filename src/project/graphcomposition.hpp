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
// Where one instance input port takes its value from. A connected port leaves
// no node of its own behind in the expansion -- the consumer inside the
// definition points straight at the outer producer -- so the answer is read
// from the authoring document while it is still being expanded, never from the
// expanded nodes. `edge > parameters > default` is the order; a `parameters`
// entry under an edge is kept as the value the port returns to when the edge
// goes away.
enum class GraphInstanceInputSource : std::uint8_t { connected, parameter, defaulted };
std::string_view graphInstanceInputSourceName(GraphInstanceInputSource source);
struct GraphInstanceInput {
    // Instance path without a trailing slash, e.g. `left` or `outer/left`.
    std::string scope;
    // The definition's graph.input id, e.g. `gain`.
    std::string port;
    GraphInstanceInputSource source = GraphInstanceInputSource::defaulted;
    // Expanded endpoint producing this port's value: the outer producer when
    // connected, the generated value.parameter otherwise.
    nlohmann::json endpoint;
    // The value.parameter name for parameter/defaulted; empty when connected.
    // (Which is also the name the port would have had, spelled `scope/port`.)
    std::string parameter;
    // Live instance scopes this port's value depends on, like node_scopes.
    std::set<std::string> scopes;
};
// Keyed by `scope + "/" + port`, which is exactly the value.parameter name an
// unconnected port receives.
using GraphInstanceInputs = std::map<std::string, GraphInstanceInput>;
struct ExpandedGraphDocument {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json outputs = nlohmann::json::object();
    std::map<std::string, GraphNodeOrigin> origins;
    GraphInstanceOwners instance_owners;
    // Required live instance scopes, including dependencies crossing an alias.
    // Derived metadata; never written into the authoring document.
    std::map<std::string, std::set<std::string>> node_scopes;
    std::map<std::string, std::set<std::string>> output_scopes;
    GraphInstanceInputs instance_inputs;
};

// Backend-independent authoring expansion. The original document remains the
// source of truth; expanded IDs and provenance are derived, not saved back.
ExpandedGraphDocument expandGraphDocument(const nlohmann::json &document);

// Apply an ordered op list to the `nodes` array of an authoring document.
// Each op edits the root by default; an optional `"scope":"<definition>"` key
// aims it at that definition body instead (resolved per op, since an earlier
// op in the same list may have changed the body). `ops` is an array of
//   {"op":"add_node","node":{...}}
//   {"op":"remove_node","node":"<id>"}
//   {"op":"set_input","node":"<id>","port":"<port>","source":{"node","port"}|null}
//   {"op":"set_node_parameters","node":"<id>","parameters":{...}}
//   {"op":"set_node_label","node":"<id>","label":"<text>"}   ("" clears)
// applied in order to one candidate document, which is expanded once at the
// end: either every op lands or the document is rejected whole. Failures that
// an editor must tell apart are named -- graph.node_duplicate@1 /
// graph.node_missing@1 / graph.input_unknown@1 / graph.input_referenced@1 /
// graph.output_referenced@1 / graph.definition_immutable@1 /
// graph.nested_instance@1 / graph.input_scope@1 -- as GraphRegistryError.
nlohmann::json editGraphDocument(const nlohmann::json &document, const nlohmann::json &ops);

// Edit owned root instances in the authoring document. References to removed
// instances must be removed explicitly; unrelated nodes and definitions survive.
// Expressed as editGraphDocument ops, so the two entry points cannot disagree
// about what a valid edit is.
nlohmann::json editGraphInstances(const nlohmann::json &document,
    const nlohmann::json &attachments, const nlohmann::json &detachments);

struct GraphInputContract {
    LogicalTypePattern accepted_type;
    bool required = true;
};
// Which execution domain evaluates an operation. Declared by the module that
// registers it, so adding a CPU value operation does not mean teaching the
// partition a new operation name. `animation` runs inside the animation phase,
// one step earlier in the same frame than `values`, so a pose operation reads
// this frame's scalars and never the other way round.
enum class GraphExecutionDomain : std::uint8_t { values, rendering, animation };
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
