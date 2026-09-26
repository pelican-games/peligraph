#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <functional>
#include <set>
#include <optional>
#include "logicalrendertype.hpp"
#include "graphinstanceowner.hpp"
#include "graphloop.hpp"

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

// LP-1b: graph.loop_expression_invalid@1 {node, pointer, reason}. `pointer`
// is the expression's JSON pointer in the authoring document; `reason` is one
// of the graphloopexpression.hpp reasons.
GraphRegistryError graphLoopExpressionInvalid(const std::string &node, const std::string &pointer,
                                              std::string_view reason);

struct GraphNodeOrigin {
    std::string instance_path;
    std::string definition;
    std::string node;
    // LP-1a: a node of a graph.loop branch body's template. `loop` is the
    // loop's id and `loop_branch` the branch; `instance_path` is the
    // template's prefix `<loop>/$<branch>/`. The template is expanded per
    // element after composition, where each copy is named
    // `<loop>/<element>/<node>` and its origin is a RenderLoopOrigin.
    std::string loop;
    std::optional<std::size_t> loop_branch;
};
// LP-1a: one branch body of a graph.loop, expanded once as a template. Its
// nodes are in ExpandedGraphDocument::nodes with ids `<prefix><node>`, so
// ports and types are checked like any other node; the resolution stage
// copies them per element.
struct GraphLoopBodyTemplate {
    std::string definition;
    // `<loop>/$<branch>/`.
    std::string prefix;
    // The body's outputs: port -> expanded endpoint inside the template.
    nlohmann::json outputs = nlohmann::json::object();
};
struct GraphLoopExpansion {
    GraphLoopShape shape;
    // One entry per branch; empty for `"body": null`.
    std::vector<std::optional<GraphLoopBodyTemplate>> branches;
    // LP-1b: the loop's input edges, resolved as the templates received them
    // (a carried input carries graphLoopCarryMarker).
    nlohmann::json inputs = nlohmann::json::object();
};
// An endpoint that reads a loop's output carries this member beside `node` and
// `port`: {"loop": "<loop>", "port": "<output>"}. The endpoint itself points
// into the first non-null branch's template (that is what its type is checked
// against); the resolution stage re-points it at the last element's copy.
// LP-1b: an edge that reads one element of `p[]` adds
//   "element":  the edge's element (an integer or an expression object that
//               carries its document pointer as "$pointer"),
//   "receiver": the node the edge enters, "input": the port it enters,
//   "receiver_loop": true when the receiver is a graph.loop (its `element`
//               variable is the receiver's element).
inline constexpr const char *graphLoopOutputMarker = "$loop_output";
// LP-1b: a body's input that `carry` feeds carries this member:
// {"loop": "<loop>", "input": "<to>"}. The endpoint itself is the loop's input
// edge, which is what the first element reads (and what its type is checked
// against); every later element reads the previous element's `from`.
inline constexpr const char *graphLoopCarryMarker = "$loop_carry";
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
    // LP-1a: every root graph.loop, by id.
    std::map<std::string, GraphLoopExpansion> loops;
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
//   {"op":"set_input","node":"<id>","port":"<port>","source":{"node","port"[,"element"]}|null}
//       (LP-1b: `element`, an integer or {"$expr": ...}, names one element of a
//       graph.loop's `p[]` output)
//   {"op":"set_node_parameters","node":"<id>","parameters":{...}}
//   {"op":"set_node_label","node":"<id>","label":"<text>"}   ("" clears)
//   {"op":"set_layout","layout":{...}|null}                  (null clears)
// and three that act on the document's `definitions` and take no scope (LP-0c):
//   {"op":"add_definition","name":"<name>","definition":{...}?}
//       an empty body {"nodes":[],"outputs":{}} unless one is given
//   {"op":"remove_definition","name":"<name>"}
//   {"op":"rename_definition","from":"<name>","to":"<name>"}
//       moves the body and rewrites every reference to it in the same op
// Each has an inverse in the same vocabulary: add <-> remove, rename(a->b) <->
// rename(b->a); the inverse of a remove is an add carrying the body the
// document held before (an editor reads it before it sends the remove).
// A definition's name follows the node id rule and may not be "root".
// Ops are applied in order to one candidate document. The agreement between an
// instance and its definition's boundary (graph.input_unknown@1,
// graph.input_referenced@1) and whether a removed definition is still named
// (graph.definition_referenced@1) are checked once after the last op, so an
// editor may send "declare the port" and "supply it" in either order. The
// candidate is then expanded once: either every op lands or the document is
// rejected whole. Failures that an editor must tell apart are named --
// graph.node_duplicate@1 / graph.node_missing@1 / graph.input_unknown@1 /
// graph.input_referenced@1 / graph.output_referenced@1 /
// graph.definition_immutable@1 / graph.nested_instance@1 /
// graph.input_scope@1 / graph.layout_invalid@1 / graph.definition_duplicate@1 /
// graph.definition_missing@1 / graph.definition_referenced@1 -- as
// GraphRegistryError.
//
// LP-1a: a graph.loop's branch bodies are its definitions the way an
// instance's definition is: rename_definition rewrites
// `parameters.branches[i].body`, remove_definition of a body in use is
// graph.definition_referenced@1, a loop's `inputs` are checked against every
// branch body's graph.input ids at the end of the list (graph.input_unknown@1
// / graph.input_referenced@1, whose `instance` field then names the loop),
// and a graph.loop added inside a definition is graph.loop_nested@1. The
// loop's own form (graph.loop_* codes) is checked by the expansion at the end.
//
// `set_layout` is the one op that writes nothing the engine reads. It replaces
// the addressed scope's `layout` member -- where the editor put the nodes, the
// control points its edges are drawn through, and the bundles that carry
// several edges side by side -- which every loader, the expansion and the
// composition carry without looking at. Its shape is
//   {"nodes":    {"<id>": {"x":<number>,"y":<number>}, ...},
//    "reroutes": [{"edge":{"node":"<id>","port":"<port>"},
//                  "points":[[<x>,<y>], ...]}, ...],
//    "ties":     [{"x":<number>,"y":<number>,
//                  "edges":[{"node":"<id>","port":"<port>"}, ...]}, ...]}
// where an edge is named by the *input* end it lands on, which is unique. Every
// node and every edge it names has to be one the addressed scope holds: a
// placement that points at nothing is graph.layout_invalid@1, because it would
// come back as a drawing of a graph that is not there.
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
