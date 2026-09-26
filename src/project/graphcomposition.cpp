#include "graphcomposition.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <set>
#include <stdexcept>
#include <vector>

namespace Pelican {
namespace {
using Json = nlohmann::json;
std::string identifier(const Json &value) {
    if (!value.is_string()) throw std::invalid_argument("graph identifier must be a string");
    const auto result = value.get<std::string>();
    if (result.empty() || result.find('/') != std::string::npos)
        throw std::invalid_argument("graph identifier must be nonempty and contain no slash");
    return result;
}
void object(const Json &value, const char *description) {
    if (!value.is_object()) throw std::invalid_argument(description);
}

class Expander {
    const Json &definitions;
    std::set<std::string> active_definitions;
  public:
    ExpandedGraphDocument result;
    explicit Expander(const Json &definitions) : definitions(definitions) {}

    Json scope(const Json &document, const std::string &path,
               const std::string &definition, const Json &arguments, const Json &parameters) {
        const auto &nodes = document.at("nodes");
        if (!nodes.is_array()) throw std::invalid_argument("graph nodes must be an array");
        std::map<std::string, const Json *> local;
        std::set<std::string> inputs;
        for (const auto &node : nodes) {
            const auto id = identifier(node.at("id"));
            if (!local.emplace(id, &node).second) throw std::invalid_argument("duplicate graph node: " + id);
            if (node.at("op") == "graph.input") inputs.insert(id);
        }
        for (const auto &[input, endpoint] : arguments.items()) {
            (void)endpoint;
            if (!inputs.contains(input)) throw std::invalid_argument("unknown subgraph input: " + input);
        }
        object(parameters, "instance parameters must be an object");
        for (const auto &[parameter, value] : parameters.items()) {
            if (!inputs.contains(parameter)) throw std::invalid_argument("unknown subgraph parameter: " + parameter);
            // edge > parameters > default. A parameter under an edge is not a
            // contradiction: it is the value the port returns to when the edge
            // is removed, so it is still checked, just not used.
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("subgraph parameter must be a finite scalar: " + parameter);
        }
        Json defaults = Json::object();
        for (const auto &input : inputs) {
            if (arguments.contains(input)) continue;
            const auto &node = *local.at(input);
            if (!parameters.contains(input) && !node.contains("default"))
                throw std::invalid_argument("unconnected subgraph input: " + input);
            const auto &value = parameters.contains(input) ? parameters.at(input) : node.at("default");
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("subgraph parameter must be a finite scalar: " + input);
            defaults[input] = value;
        }
        // Record the supply of every instance input port while both halves are
        // still in hand. The root document's own parameters are not an
        // instance's, so they are left out.
        if (!path.empty()) {
            const auto scope = path.substr(0, path.size() - 1);
            for (const auto &input : inputs) {
                GraphInstanceInput record{scope, input};
                if (arguments.contains(input)) {
                    record.source = GraphInstanceInputSource::connected;
                    record.endpoint = arguments.at(input);
                } else {
                    record.source = parameters.contains(input) ? GraphInstanceInputSource::parameter
                                                              : GraphInstanceInputSource::defaulted;
                    record.endpoint = Json{{"node", path + input}, {"port", "value"}};
                    record.parameter = path + input;
                }
                result.instance_inputs.emplace(path + input, std::move(record));
            }
        }
        std::map<std::string, Json> instances;
        std::set<std::string> active_instances;
        std::function<Json(const Json &)> resolve;
        std::function<const Json &(const std::string &)> instantiate;
        instantiate = [&](const std::string &id) -> const Json & {
            if (instances.contains(id)) return instances.at(id);
            if (!active_instances.insert(id).second) throw std::invalid_argument("cyclic subgraph connection: " + id);
            const auto &node = *local.at(id);
            const auto ref = identifier(node.at("definition"));
            if (!definitions.contains(ref)) throw std::invalid_argument("unknown graph definition: " + ref);
            if (node.contains("owner")) {
                const auto &owner = node.at("owner");
                object(owner, "graph owner must be an object");
                if (owner.size() != 2 || !owner.contains("scene") || !owner.contains("object") ||
                    !owner.at("scene").is_string() || owner.at("scene").get_ref<const std::string &>().empty() ||
                    !owner.at("object").is_number_integer() || owner.at("object") <= 0)
                    throw std::invalid_argument("graph owner requires scene and positive authoring object id");
                result.instance_owners.emplace(path + id, GraphInstanceOwner{
                    owner.at("scene").get<std::string>(), owner.at("object").get<std::uint64_t>()});
            }
            Json bound = Json::object();
            const auto authored_inputs = node.value("inputs", Json::object());
            object(authored_inputs, "instance inputs must be an object");
            for (const auto &[port, endpoint] : authored_inputs.items()) bound[port] = resolve(endpoint);
            if (!active_definitions.insert(ref).second) throw std::invalid_argument("recursive graph definition: " + ref);
            auto outputs = scope(definitions.at(ref), path + id + "/", ref, bound,
                                 node.value("parameters", Json::object()));
            active_definitions.erase(ref);
            active_instances.erase(id);
            return instances.emplace(id, std::move(outputs)).first->second;
        };
        resolve = [&](const Json &endpoint) -> Json {
            const auto id = identifier(endpoint.at("node"));
            const auto port = identifier(endpoint.at("port"));
            const auto found = local.find(id);
            if (found == local.end()) throw std::invalid_argument("unknown graph source: " + id);
            const auto &node = *found->second;
            if (node.at("op") == "graph.input") {
                if (port != "value") throw std::invalid_argument("graph input output port must be value");
                return arguments.contains(id) ? arguments.at(id)
                    : Json{{"node", path + id}, {"port", "value"}};
            }
            if (node.at("op") == "graph.instance") {
                const auto &outputs = instantiate(id);
                if (!outputs.contains(port)) throw std::invalid_argument("unknown subgraph output: " + port);
                return outputs.at(port);
            }
            const auto &ports = node.at("outputs");
            if (!ports.is_array() || std::find(ports.begin(), ports.end(), port) == ports.end())
                throw std::invalid_argument("unknown graph output port: " + id + "." + port);
            return Json{{"node", path + id}, {"port", port}};
        };
        for (const auto &[id, node] : local) {
            if (node->at("op") == "graph.input") {
                if (defaults.contains(id)) {
                    result.nodes.push_back({{"id", path + id}, {"op", "value.parameter"},
                        {"parameters", {{"name", path + id}, {"default", defaults.at(id)}}},
                        {"inputs", Json::object()}, {"outputs", {"value"}}});
                    result.origins.emplace(path + id, GraphNodeOrigin{path, definition, id});
                }
                continue;
            }
            if (node->at("op") == "graph.instance") { (void)instantiate(id); continue; }
            auto expanded = *node;
            expanded["id"] = path + id;
            const auto authored_inputs = node->value("inputs", Json::object());
            object(authored_inputs, "node inputs must be an object");
            expanded["inputs"] = Json::object();
            for (const auto &[port, endpoint] : authored_inputs.items()) expanded["inputs"][port] = resolve(endpoint);
            result.nodes.push_back(std::move(expanded));
            result.origins.emplace(path + id, GraphNodeOrigin{path, definition, id});
        }
        const auto &outputs = document.at("outputs");
        object(outputs, "graph outputs must be an object");
        Json exposed = Json::object();
        for (const auto &[port, endpoint] : outputs.items()) {
            auto resolved = resolve(endpoint);
            // Even an input-to-output alias is unavailable when its instance
            // is inactive. Keep that boundary until dependency compilation.
            if (!path.empty()) resolved["$scopes"].push_back(path.substr(0, path.size() - 1));
            exposed[identifier(port)] = std::move(resolved);
        }
        return exposed;
    }
};
}

ExpandedGraphDocument expandGraphDocument(const Json &document) {
    if (document.at("schema") != "pelican.graph" || document.at("version") != 1)
        throw std::invalid_argument("unsupported graph schema/version");
    const auto definitions = document.value("definitions", Json::object());
    object(definitions, "graph definitions must be an object");
    Expander expander{definitions};
    expander.result.outputs = expander.scope(document, "", "", Json::object(),
                                            document.value("parameters", Json::object()));
    auto &result = expander.result;
    const auto addAncestors = [&](std::set<std::string> &scopes, std::string_view path) {
        for (const auto &[scope, owner] : result.instance_owners) {
            if (path == scope || (path.starts_with(scope) && path.size() > scope.size() && path[scope.size()] == '/'))
                scopes.insert(scope);
        }
    };
    const auto takeScopes = [&](Json &endpoint, std::set<std::string> &scopes) {
        if (const auto found = endpoint.find("$scopes"); found != endpoint.end()) {
            for (const auto &scope : *found) addAncestors(scopes, scope.get_ref<const std::string &>());
            endpoint.erase(found);
        }
    };
    std::map<std::string, std::vector<std::string>> consumers;
    for (auto &node : result.nodes) {
        const auto id = node.at("id").get<std::string>();
        auto &scopes = result.node_scopes[id];
        addAncestors(scopes, id);
        for (auto &[port, endpoint] : node.at("inputs").items()) {
            takeScopes(endpoint, scopes);
            consumers[endpoint.at("node").get<std::string>()].push_back(id);
        }
    }
    // Propagate once per newly discovered node/scope pair. Declaration order
    // and duplicate edges do not affect the result.
    std::vector<std::pair<std::string, std::string>> pending;
    for (const auto &[id, scopes] : result.node_scopes)
        for (const auto &scope : scopes) pending.emplace_back(id, scope);
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto [id, scope] = pending[i];
        for (const auto &consumer : consumers[id])
            if (result.node_scopes.at(consumer).insert(scope).second)
                pending.emplace_back(consumer, scope);
    }
    for (auto &[port, endpoint] : result.outputs.items()) {
        auto &scopes = result.output_scopes[port];
        takeScopes(endpoint, scopes);
        const auto &source = result.node_scopes.at(endpoint.at("node").get<std::string>());
        scopes.insert(source.begin(), source.end());
    }
    // A port's value is unavailable when its own instance is, not only when its
    // producer is; an outer producer carries none of the instance's scopes.
    for (auto &[key, input] : result.instance_inputs) {
        (void)key;
        takeScopes(input.endpoint, input.scopes);
        addAncestors(input.scopes, input.scope);
        const auto &source = result.node_scopes.at(input.endpoint.at("node").get<std::string>());
        input.scopes.insert(source.begin(), source.end());
    }
    return std::move(expander.result);
}

GraphRegistryError::GraphRegistryError(std::string code, nlohmann::json detail)
    : std::runtime_error(code + " " + detail.dump()), code_(std::move(code)),
      detail_(std::move(detail)) {}

std::string_view graphExecutionDomainName(GraphExecutionDomain domain) {
    switch (domain) {
    case GraphExecutionDomain::values: return "values";
    case GraphExecutionDomain::rendering: return "rendering";
    case GraphExecutionDomain::animation: return "animation";
    }
    throw std::invalid_argument("unknown graph execution domain");
}

std::string_view graphOperationEvaluationName(GraphOperationEvaluation evaluation) {
    switch (evaluation) {
    case GraphOperationEvaluation::direct: return "direct";
    case GraphOperationEvaluation::replay: return "replay";
    case GraphOperationEvaluation::baked: return "baked";
    case GraphOperationEvaluation::recorded: return "recorded";
    }
    throw std::invalid_argument("unknown graph operation evaluation");
}

std::string_view graphParameterChangeName(GraphParameterChange change) {
    switch (change) {
    case GraphParameterChange::value: return "value";
    case GraphParameterChange::recompile: return "recompile";
    case GraphParameterChange::rebuild: return "rebuild";
    }
    throw std::invalid_argument("unknown graph parameter change");
}

std::map<std::string, LogicalType> validateGraphPorts(
    const ExpandedGraphDocument &graph, const LogicalTypeRegistry &registry,
    const GraphOperationContracts &operations) {
    return validateGraphPorts(graph, registry, GraphOperationContractResolver{
        [&](const Json &node) {
            const auto op = node.at("op").get<std::string>();
            const auto found = operations.find(op);
            if (found == operations.end())
                throw GraphRegistryError("graph.operation_unregistered@1",
                    {{"node", node.at("id").get<std::string>()}, {"op", op}});
            return found->second;
        }});
}

std::map<std::string, LogicalType> validateGraphPorts(
    const ExpandedGraphDocument &graph, const LogicalTypeRegistry &registry,
    const GraphOperationContractResolver &resolve) {
    std::map<std::string, GraphOperationContract> nodes;
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        const auto [found, inserted] = nodes.emplace(id, resolve(node));
        if (!inserted)
            throw std::invalid_argument("duplicate expanded graph node: " + id);
        std::set<std::string> ports;
        const auto &outputs = node.at("outputs");
        if (!outputs.is_array()) throw std::invalid_argument("graph outputs must be an array: " + id);
        for (const auto &port : outputs) {
            const auto name = port.get<std::string>();
            if (!found->second.outputs.contains(name) || !ports.insert(name).second)
                throw std::invalid_argument("invalid graph output: " + id + "." + name);
        }
        if (ports.size() != found->second.outputs.size())
            throw std::invalid_argument("missing graph output: " + id);
        for (const auto &[port, type] : found->second.outputs) registry.requireCanonical(type);
    }
    const auto sourceType = [&](const Json &endpoint) -> const LogicalType & {
        const auto id = endpoint.at("node").get<std::string>();
        const auto port = endpoint.at("port").get<std::string>();
        const auto node = nodes.find(id);
        if (node == nodes.end() || !node->second.outputs.contains(port))
            throw std::invalid_argument("unknown graph source: " + id + "." + port);
        return node->second.outputs.at(port);
    };
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        const auto &contract = nodes.at(id);
        const auto &inputs = node.at("inputs");
        object(inputs, "graph inputs must be an object");
        for (const auto &[port, endpoint] : inputs.items()) {
            const auto input = contract.inputs.find(port);
            if (input == contract.inputs.end())
                throw std::invalid_argument("unknown graph input: " + id + "." + port);
            const auto match = matchLogicalType(registry, sourceType(endpoint), input->second.accepted_type);
            if (match.status != LogicalTypeMatchStatus::exact)
                throw std::invalid_argument("graph type mismatch at " + id + "." + port +
                    " from " + endpoint.at("node").get<std::string>() + "." +
                    endpoint.at("port").get<std::string>() + ": " + match.reason_code);
        }
        for (const auto &[port, input] : contract.inputs)
            if (input.required && !inputs.contains(port))
                throw std::invalid_argument("unconnected graph input: " + id + "." + port);
    }
    std::map<std::string, LogicalType> outputs;
    for (const auto &[port, endpoint] : graph.outputs.items()) outputs.emplace(port, sourceType(endpoint));
    return outputs;
}

std::string_view graphInstanceInputSourceName(GraphInstanceInputSource source) {
    switch (source) {
    case GraphInstanceInputSource::connected: return "connected";
    case GraphInstanceInputSource::parameter: return "parameter";
    case GraphInstanceInputSource::defaulted: return "default";
    }
    throw std::invalid_argument("unknown graph instance input source");
}

namespace {
// The scope token that names the root document. "root" and an absent `scope`
// are the same thing; a definition that is itself called "root" is therefore
// unaddressable, which is the price of a default that reads.
constexpr auto kRootScope = "root";

// Which body an op addresses: the root `nodes` array, or one definition's. The
// outputs map travels with it, because "this node is still the scope's output"
// is a property of the scope, not of the document.
struct EditScope {
    bool root = true;
    std::string name;
    Json *nodes = nullptr;
    const Json *outputs = nullptr;
    // The object the scope's own members live on: the document for the root,
    // the definition body otherwise. `set_layout` is the one op that writes
    // beside `nodes` rather than into it.
    Json *container = nullptr;
};

// A node named across an instance path belongs to no single body, so it is
// refused by name rather than reported as missing: the two call for different
// fixes. Inside the root scope a node that lives in a definition is named as
// exactly that, so the caller learns which scope to address instead.
Json &locateNode(const EditScope &scope, const Json &definitions, const std::string &id) {
    if (id.find('/') != std::string::npos)
        throw GraphRegistryError("graph.definition_immutable@1",
            {{"node", id}, {"reason", "instance_path"}});
    const auto found = std::find_if(scope.nodes->begin(), scope.nodes->end(), [&](const Json &node) {
        return node.contains("id") && node.at("id") == id;
    });
    if (found != scope.nodes->end()) return *found;
    if (scope.root) {
        for (const auto &[definition, body] : definitions.items())
            for (const auto &node : body.value("nodes", Json::array()))
                if (node.value("id", std::string{}) == id)
                    throw GraphRegistryError("graph.definition_immutable@1",
                        {{"node", id}, {"definition", definition}});
        throw GraphRegistryError("graph.node_missing@1", {{"node", id}});
    }
    throw GraphRegistryError("graph.node_missing@1",
        {{"node", id}, {"scope", scope.name}});
}

// Resolve one op's `scope` against the candidate. The definition body has to be
// looked up per op: an earlier op in the same list may have changed it.
EditScope resolveEditScope(Json &candidate, const Json &operation) {
    std::string name = kRootScope;
    if (const auto found = operation.find("scope"); found != operation.end() && !found->is_null()) {
        if (!found->is_string())
            throw std::invalid_argument("graph document op scope must be a string");
        name = found->get<std::string>();
    }
    if (name.empty() || name == kRootScope) {
        EditScope scope;
        scope.nodes = &candidate.at("nodes");
        if (!scope.nodes->is_array()) throw std::invalid_argument("graph nodes must be an array");
        const auto outputs = candidate.find("outputs");
        if (outputs != candidate.end()) scope.outputs = &*outputs;
        scope.container = &candidate;
        return scope;
    }
    const auto definitions = candidate.find("definitions");
    if (definitions == candidate.end() || !definitions->is_object() ||
        !definitions->contains(name))
        throw std::invalid_argument("unknown graph definition scope: " + name);
    auto &body = definitions->at(name);
    object(body, "graph definition must be an object");
    EditScope scope;
    scope.root = false;
    scope.name = name;
    scope.nodes = &body.at("nodes");
    if (!scope.nodes->is_array())
        throw std::invalid_argument("graph nodes must be an array: " + name);
    const auto outputs = body.find("outputs");
    if (outputs != body.end()) scope.outputs = &*outputs;
    scope.container = &body;
    return scope;
}

// --- Who names a definition. LP-0c.
//
// One walk answers it for every consumer: remove_definition refuses a
// definition something still names, rename_definition rewrites every name in
// the same op, and the boundary-port check (requireInputUnreferenced) finds the
// instances that supply a port. A new kind of referrer is added here once and
// all three see it. The root document is walked first, then the definition
// bodies in the document's key order; a nested instance inside a definition
// counts the same as a root one.
//
// `Document` is `Json` or `const Json`, so the same walk both reads (the
// checks) and writes (rename). `visit(scope, node, field)` gets the scope that
// holds the referring node -- the empty string for the root document, else the
// definition's name -- the node, and where in the node the name is written.
template <typename Document, typename Visit>
void visitDefinitionReferences(Document &document, const std::string &definition,
                               Visit &&visit) {
    const auto walk = [&](auto &nodes, const std::string &scope) {
        if (!nodes.is_array()) return;
        for (auto &node : nodes) {
            if (!node.is_object()) continue;
            if (node.value("op", std::string{}) == "graph.instance") {
                const auto found = node.find("definition");
                if (found != node.end() && found->is_string() && *found == definition)
                    visit(scope, node, Json::json_pointer{"/definition"});
            }
            // LP-1a adds graph.loop here: every `parameters.branches[i].body`
            // that names this definition is a reference, written at
            // `/parameters/branches/<i>/body`. remove / rename / the port
            // check then cover a loop's branches without further change.
        }
    };
    if (const auto nodes = document.find("nodes"); nodes != document.end())
        walk(*nodes, std::string{});
    const auto definitions = document.find("definitions");
    if (definitions == document.end() || !definitions->is_object()) return;
    for (auto body = definitions->begin(); body != definitions->end(); ++body) {
        if (!body->is_object()) continue;
        if (const auto nodes = body->find("nodes"); nodes != body->end())
            walk(*nodes, body.key());
    }
}

struct DefinitionReference {
    // Empty for the root document, else the definition holding the referrer.
    std::string scope;
    std::string node;
    Json::json_pointer field;
    // Into the document the list was taken from; valid while it is unchanged.
    const Json *referrer = nullptr;
};

std::vector<DefinitionReference> definitionReferences(const Json &document,
                                                      const std::string &definition) {
    std::vector<DefinitionReference> references;
    visitDefinitionReferences(document, definition,
        [&](const std::string &scope, const Json &node, const Json::json_pointer &field) {
            references.push_back({scope, node.value("id", std::string{}), field, &node});
        });
    return references;
}

// Every reference to this definition that supplies this boundary port -- with
// a stored parameter or with an edge. Only a `graph.instance` supplies ports by
// the definition's input names (LP-1a decides how a graph.loop's `inputs` /
// `carry` reach its branch bodies, and adds that here).
void requireInputUnreferenced(const Json &candidate, const std::string &definition,
                              const std::string &input) {
    for (const auto &reference : definitionReferences(candidate, definition)) {
        const auto &node = *reference.referrer;
        if (node.value("op", std::string{}) != "graph.instance") continue;
        const auto supplies = [&](const char *key) {
            const auto found = node.find(key);
            return found != node.end() && found->is_object() && found->contains(input);
        };
        if (!supplies("parameters") && !supplies("inputs")) continue;
        throw GraphRegistryError("graph.input_referenced@1",
            {{"definition", definition}, {"input", input}, {"instance", reference.node}});
    }
}

std::string opNodeName(const Json &value) {
    if (!value.is_string() || value.get_ref<const std::string &>().empty())
        throw std::invalid_argument("graph document op requires a nonempty node id");
    return value.get<std::string>();
}

// The declared inputs of an instance's definition. Empty when the definition is
// unknown: the expansion names that failure itself.
std::set<std::string> declaredInstanceInputs(const Json &node, const Json &definitions) {
    std::set<std::string> declared;
    const auto reference = node.value("definition", std::string{});
    if (!definitions.contains(reference)) return declared;
    for (const auto &entry : definitions.at(reference).value("nodes", Json::array()))
        if (entry.value("op", std::string{}) == "graph.input")
            declared.insert(entry.value("id", std::string{}));
    return declared;
}

// --- set_layout. Nothing here is read by the engine; what is checked is that
// every name the placement uses is one the addressed scope really holds. A
// layout that points at a node or an edge that is not there would come back to
// an editor as a drawing of a graph that does not exist, and the editor cannot
// tell that apart from its own bug.

GraphRegistryError layoutInvalid(const char *reason, Json detail) {
    detail["reason"] = reason;
    return GraphRegistryError("graph.layout_invalid@1", std::move(detail));
}

// The node one layout entry names, found under exactly the rules every other op
// is bound by: an instance path belongs to no single body, and a node that lives
// in a definition is not reachable from the root scope.
const Json &locateLayoutNode(const EditScope &scope, const Json &definitions,
                             const std::string &id) {
    if (id.find('/') != std::string::npos)
        throw GraphRegistryError("graph.definition_immutable@1",
            {{"node", id}, {"reason", "instance_path"}});
    for (const auto &node : *scope.nodes)
        if (node.contains("id") && node.at("id") == id) return node;
    if (scope.root)
        for (const auto &[definition, body] : definitions.items())
            for (const auto &node : body.value("nodes", Json::array()))
                if (node.value("id", std::string{}) == id)
                    throw GraphRegistryError("graph.definition_immutable@1",
                        {{"node", id}, {"definition", definition}});
    throw layoutInvalid("unknown_node",
        {{"node", id}, {"scope", scope.root ? std::string{kRootScope} : scope.name}});
}

double layoutCoordinate(const Json &value, const char *what) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw layoutInvalid("coordinate", {{"member", what}});
    return value.get<double>();
}

void requireLayoutPoint(const Json &point) {
    if (!point.is_array() || point.size() != 2)
        throw layoutInvalid("point", {{"point", point}});
    (void)layoutCoordinate(point.at(0), "x");
    (void)layoutCoordinate(point.at(1), "y");
}

// An edge is named by the input end it lands on, which is unique: one port of
// one node takes at most one edge. So the check is that the port is one this
// node really has an edge into.
void requireLayoutEdge(const EditScope &scope, const Json &definitions,
                       const Json &edge) {
    object(edge, "set_layout edge must be an object");
    if (edge.size() != 2 || !edge.contains("node") || !edge.contains("port"))
        throw std::invalid_argument("set_layout edge requires exactly node and port");
    const auto id = opNodeName(edge.at("node"));
    const auto port = identifier(edge.at("port"));
    const Json &node = locateLayoutNode(scope, definitions, id);
    const auto inputs = node.find("inputs");
    if (inputs == node.end() || !inputs->is_object() || !inputs->contains(port))
        throw layoutInvalid("unknown_edge", {{"node", id}, {"port", port}});
}

// N-8. The Frame Plan canvas draws passes and resources, and neither of those
// is a node of this document: a pass name comes from the rendering preset, a
// feature, or an authored fragment, and it can be renamed there without this
// document hearing about it. So this member is checked for SHAPE only --
// finite coordinates, objects where objects are expected -- and no name in it
// is looked up. `locateLayoutNode` deliberately never runs over it: a pass the
// document cannot see is not a broken placement, it is a placement about
// something else.
void requireFramePlanEdge(const Json &edge) {
    object(edge, "set_layout frame_plan edge must be an object");
    if (edge.size() != 2 || !edge.contains("node") || !edge.contains("resource"))
        throw layoutInvalid("frame_plan_edge", {{"edge", edge}});
    for (const auto *member : {"node", "resource"})
        if (!edge.at(member).is_string() ||
            edge.at(member).get_ref<const std::string &>().empty())
            throw layoutInvalid("frame_plan_edge", {{"member", member}});
}

void requireFramePlanLayout(const Json &frame_plan) {
    object(frame_plan, "set_layout frame_plan must be an object");
    for (const auto &[graph, placement] : frame_plan.items()) {
        object(placement, "set_layout frame_plan graph must be an object");
        for (const auto &[key, value] : placement.items()) {
            (void)value;
            if (key != "nodes" && key != "reroutes" && key != "ties" &&
                key != "groups")
                throw layoutInvalid("frame_plan_unknown_key",
                                    {{"graph", graph}, {"key", key}});
        }
        if (const auto nodes = placement.find("nodes"); nodes != placement.end()) {
            object(*nodes, "set_layout frame_plan nodes must be an object");
            for (const auto &[name, place] : nodes->items()) {
                if (!place.is_object() || !place.contains("x") ||
                    !place.contains("y"))
                    throw layoutInvalid("frame_plan_node_placement",
                                        {{"graph", graph}, {"node", name}});
                (void)layoutCoordinate(place.at("x"), "x");
                (void)layoutCoordinate(place.at("y"), "y");
            }
        }
        if (const auto reroutes = placement.find("reroutes");
            reroutes != placement.end()) {
            if (!reroutes->is_array())
                throw std::invalid_argument(
                    "set_layout frame_plan reroutes must be an array");
            for (const auto &entry : *reroutes) {
                object(entry, "set_layout frame_plan reroute must be an object");
                if (!entry.contains("edge") || !entry.contains("points"))
                    throw layoutInvalid("frame_plan_reroute",
                                        {{"graph", graph}, {"reroute", entry}});
                requireFramePlanEdge(entry.at("edge"));
                if (!entry.at("points").is_array())
                    throw std::invalid_argument(
                        "set_layout frame_plan reroute points must be an array");
                for (const auto &point : entry.at("points"))
                    requireLayoutPoint(point);
            }
        }
        if (const auto ties = placement.find("ties"); ties != placement.end()) {
            if (!ties->is_array())
                throw std::invalid_argument(
                    "set_layout frame_plan ties must be an array");
            for (const auto &tie : *ties) {
                object(tie, "set_layout frame_plan tie must be an object");
                if (!tie.contains("x") || !tie.contains("y") ||
                    !tie.contains("edges"))
                    throw layoutInvalid("frame_plan_tie",
                                        {{"graph", graph}, {"tie", tie}});
                (void)layoutCoordinate(tie.at("x"), "x");
                (void)layoutCoordinate(tie.at("y"), "y");
                if (!tie.at("edges").is_array())
                    throw std::invalid_argument(
                        "set_layout frame_plan tie edges must be an array");
                for (const auto &edge : tie.at("edges"))
                    requireFramePlanEdge(edge);
            }
        }
        if (const auto groups = placement.find("groups"); groups != placement.end()) {
            if (!groups->is_array())
                throw std::invalid_argument(
                    "set_layout frame_plan groups must be an array");
            for (const auto &group : *groups) {
                object(group, "set_layout frame_plan group must be an object");
                if (!group.contains("id") || !group.at("id").is_string() ||
                    !group.contains("members") || !group.at("members").is_array())
                    throw layoutInvalid("frame_plan_group",
                                        {{"graph", graph}, {"group", group}});
                for (const auto &member : group.at("members"))
                    if (!member.is_string())
                        throw layoutInvalid("frame_plan_group",
                                            {{"graph", graph}, {"member", member}});
            }
        }
    }
}

void requireLayout(const EditScope &scope, const Json &definitions, const Json &layout) {
    object(layout, "set_layout requires a layout object or null");
    for (const auto &[key, value] : layout.items()) {
        (void)value;
        if (key != "nodes" && key != "reroutes" && key != "ties" &&
            key != "frame_plan")
            throw layoutInvalid("unknown_key", {{"key", key}});
    }
    if (const auto frame_plan = layout.find("frame_plan");
        frame_plan != layout.end())
        requireFramePlanLayout(*frame_plan);
    if (const auto nodes = layout.find("nodes"); nodes != layout.end()) {
        object(*nodes, "set_layout nodes must be an object");
        for (const auto &[id, place] : nodes->items()) {
            (void)locateLayoutNode(scope, definitions, id);
            if (!place.is_object() || !place.contains("x") || !place.contains("y"))
                throw layoutInvalid("node_placement", {{"node", id}});
            (void)layoutCoordinate(place.at("x"), "x");
            (void)layoutCoordinate(place.at("y"), "y");
        }
    }
    if (const auto reroutes = layout.find("reroutes"); reroutes != layout.end()) {
        if (!reroutes->is_array())
            throw std::invalid_argument("set_layout reroutes must be an array");
        for (const auto &entry : *reroutes) {
            object(entry, "set_layout reroute must be an object");
            if (!entry.contains("edge") || !entry.contains("points"))
                throw layoutInvalid("reroute", {{"reroute", entry}});
            requireLayoutEdge(scope, definitions, entry.at("edge"));
            const auto &points = entry.at("points");
            if (!points.is_array())
                throw std::invalid_argument("set_layout reroute points must be an array");
            for (const auto &point : points) requireLayoutPoint(point);
        }
    }
    if (const auto ties = layout.find("ties"); ties != layout.end()) {
        if (!ties->is_array()) throw std::invalid_argument("set_layout ties must be an array");
        for (const auto &tie : *ties) {
            object(tie, "set_layout tie must be an object");
            if (!tie.contains("x") || !tie.contains("y") || !tie.contains("edges"))
                throw layoutInvalid("tie", {{"tie", tie}});
            (void)layoutCoordinate(tie.at("x"), "x");
            (void)layoutCoordinate(tie.at("y"), "y");
            const auto &edges = tie.at("edges");
            if (!edges.is_array())
                throw std::invalid_argument("set_layout tie edges must be an array");
            for (const auto &edge : edges) requireLayoutEdge(scope, definitions, edge);
        }
    }
}

// Topology and its placement are one edit. Leaving stale placement for the
// canvas to repair would create a second history entry whose inverse refers
// to an already removed node or edge and therefore cannot pass set_layout.
void pruneScopeLayout(const EditScope &scope) {
    const auto found = scope.container->find("layout");
    if (found == scope.container->end() || !found->is_object()) return;
    auto &layout = *found;
    std::set<std::string> present;
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto &node : *scope.nodes) {
        const auto id = node.at("id").get<std::string>();
        present.insert(id);
        if (const auto inputs = node.find("inputs");
            inputs != node.end() && inputs->is_object())
            for (const auto &[port, endpoint] : inputs->items()) {
                (void)endpoint;
                edges.emplace(id, port);
            }
    }
    const auto liveEdge = [&](const Json &edge) {
        return edge.is_object() && edge.contains("node") && edge.at("node").is_string() &&
            edge.contains("port") && edge.at("port").is_string() &&
            edges.contains({edge.at("node").get<std::string>(), edge.at("port").get<std::string>()});
    };
    if (const auto nodes = layout.find("nodes"); nodes != layout.end() && nodes->is_object()) {
        for (auto entry = nodes->begin(); entry != nodes->end();)
            if (!present.contains(entry.key())) entry = nodes->erase(entry);
            else ++entry;
    }
    if (const auto reroutes = layout.find("reroutes");
        reroutes != layout.end() && reroutes->is_array())
        reroutes->erase(std::remove_if(reroutes->begin(), reroutes->end(), [&](const Json &entry) {
            return entry.is_object() && entry.contains("edge") &&
                (!liveEdge(entry.at("edge")) ||
                 (entry.contains("points") && entry.at("points").empty()));
        }), reroutes->end());
    if (const auto ties = layout.find("ties"); ties != layout.end() && ties->is_array()) {
        for (auto &tie : *ties) {
            if (!tie.is_object() || !tie.contains("edges") || !tie.at("edges").is_array()) continue;
            auto &members = tie["edges"];
            members.erase(std::remove_if(members.begin(), members.end(), [&](const Json &edge) {
                return !liveEdge(edge);
            }), members.end());
            if (members.size() != 1 || !tie.contains("x") || !tie.at("x").is_number() ||
                !tie.contains("y") || !tie.at("y").is_number()) continue;
            // Match the canvas: a surviving line keeps the former bundle's
            // control point, ordered by x with its existing reroute points.
            if (!layout.contains("reroutes")) layout["reroutes"] = Json::array();
            auto &reroutes = layout["reroutes"];
            if (!reroutes.is_array()) continue;
            auto entry = std::find_if(reroutes.begin(), reroutes.end(), [&](const Json &reroute) {
                return reroute.is_object() && reroute.value("edge", Json{}) == members.front();
            });
            if (entry == reroutes.end()) {
                reroutes.push_back({{"edge", members.front()}, {"points", Json::array()}});
                entry = std::prev(reroutes.end());
            }
            auto &points = (*entry)["points"];
            if (!points.is_array()) continue;
            points.push_back(Json::array({tie.at("x"), tie.at("y")}));
            std::stable_sort(points.begin(), points.end(), [](const Json &a, const Json &b) {
                return a.at(0).get<double>() < b.at(0).get<double>();
            });
            members.clear();
        }
        ties->erase(std::remove_if(ties->begin(), ties->end(), [](const Json &tie) {
            return tie.is_object() && tie.contains("edges") && tie.at("edges").empty();
        }), ties->end());
    }
}

void requireInstancePort(const Json &node, const Json &definitions, const std::string &port) {
    if (node.value("op", std::string{}) != "graph.instance") return;
    const auto declared = declaredInstanceInputs(node, definitions);
    if (declared.empty() || declared.contains(port)) return;
    throw GraphRegistryError("graph.input_unknown@1",
        {{"node", node.at("id")}, {"definition", node.value("definition", std::string{})},
         {"port", port}});
}

// --- Checks on the whole document, run once after the last op. LP-0c.
//
// An instance's ports and its definition's boundary are two bodies, and an
// editor changes them with two ops: "add a graph.input to the definition" and
// "supply it on the instance", in either order, or the removal of both. The
// agreement between the two -- and whether a removed definition is still named
// -- is therefore checked on the candidate the whole list produced, not after
// each op. Each op records what it would have checked; if the op itself then
// fails for another reason, its own records are checked first, so an op that
// is refused still reports the same code it always did.
struct DeferredCheck {
    enum class Kind : std::uint8_t {
        // `port` of instance `node` in `scope` is a graph.input of its
        // definition (set_input / set_node_parameters).
        instance_port,
        // No reference to `definition` still supplies its removed graph.input
        // `port` (remove_node of a graph.input), unless it was declared again.
        input_unreferenced,
        // Nothing names the removed `definition` any more (remove_definition),
        // unless a definition of that name is there again.
        definition_unreferenced,
    };
    Kind kind;
    // instance_port: the scope holding the instance, empty for the root.
    std::string scope;
    std::string node;
    std::string definition;
    std::string port;
    // instance_port recorded by a `set_input` that clears the port, when the
    // port was the definition's at that point: declared then, or declared at
    // the start of the list and removed by an earlier op. Clearing such a port
    // is how a supply is taken away, so it holds even if the port is gone by
    // the end -- but only if nothing supplies it again.
    bool cleared_known_port = false;
};

const Json *findScopeNode(const Json &candidate, const std::string &scope,
                          const std::string &id) {
    const Json *nodes = nullptr;
    if (scope.empty()) {
        if (const auto found = candidate.find("nodes"); found != candidate.end()) nodes = &*found;
    } else if (const auto definitions = candidate.find("definitions");
               definitions != candidate.end() && definitions->is_object() &&
               definitions->contains(scope) && definitions->at(scope).is_object()) {
        const auto &body = definitions->at(scope);
        if (const auto found = body.find("nodes"); found != body.end()) nodes = &*found;
    }
    if (nodes == nullptr || !nodes->is_array()) return nullptr;
    for (const auto &node : *nodes)
        if (node.is_object() && node.value("id", std::string{}) == id) return &node;
    return nullptr;
}

bool declaresInput(const Json &candidate, const std::string &definition,
                   const std::string &input) {
    const auto definitions = candidate.find("definitions");
    if (definitions == candidate.end() || !definitions->is_object() ||
        !definitions->contains(definition))
        return false;
    for (const auto &node : definitions->at(definition).value("nodes", Json::array()))
        if (node.value("op", std::string{}) == "graph.input" &&
            node.value("id", std::string{}) == input)
            return true;
    return false;
}

// `in_place` is the run for an op that failed on its own: the check is asked
// of the document as that op found it, where the removed port is still there.
void runDeferredCheck(const Json &candidate, const DeferredCheck &check, bool in_place = false) {
    static const Json no_definitions = Json::object();
    const auto definitions = candidate.find("definitions");
    const auto &known = definitions == candidate.end() || !definitions->is_object()
        ? no_definitions : *definitions;
    switch (check.kind) {
    case DeferredCheck::Kind::instance_port: {
        // An instance a later op removed supplies nothing.
        const auto *node = findScopeNode(candidate, check.scope, check.node);
        if (node == nullptr) return;
        const auto supplies = [&](const char *key) {
            const auto found = node->find(key);
            return found != node->end() && found->is_object() && found->contains(check.port);
        };
        if (check.cleared_known_port && !supplies("inputs") && !supplies("parameters")) return;
        requireInstancePort(*node, known, check.port);
        return;
    }
    case DeferredCheck::Kind::input_unreferenced:
        if (!in_place && (!known.contains(check.definition) ||
                          declaresInput(candidate, check.definition, check.port)))
            return;
        requireInputUnreferenced(candidate, check.definition, check.port);
        return;
    case DeferredCheck::Kind::definition_unreferenced: {
        if (known.contains(check.definition)) return;
        auto referenced_by = Json::array();
        for (const auto &reference : definitionReferences(candidate, check.definition))
            if (std::find(referenced_by.begin(), referenced_by.end(), reference.node) ==
                referenced_by.end())
                referenced_by.push_back(reference.node);
        if (referenced_by.empty()) return;
        throw GraphRegistryError("graph.definition_referenced@1",
            {{"definition", check.definition}, {"referenced_by", std::move(referenced_by)}});
    }
    }
}

// --- add_definition / remove_definition / rename_definition. LP-0c.
//
// A definition's name follows the node id rule: a nonempty string without a
// slash. "root" is refused as well, because that token names the root document
// in an op's `scope`: a definition called that could never be edited.

bool isDefinitionOp(const std::string &kind) {
    return kind == "add_definition" || kind == "remove_definition" ||
           kind == "rename_definition";
}

std::string definitionName(const Json &operation, const char *member) {
    const auto found = operation.find(member);
    if (found == operation.end())
        throw std::invalid_argument(std::string{"graph definition op requires "} + member);
    return identifier(*found);
}

void requireNewDefinitionName(const std::string &name) {
    if (name == kRootScope)
        throw std::invalid_argument(
            "graph definition cannot be named \"root\": that scope names the root document");
}

// The same rules add_node applies inside a definition scope, so a body that
// arrives whole cannot hold what could not have been added one node at a time.
void requireDefinitionBody(const std::string &name, const Json &body) {
    object(body, "add_definition definition must be an object");
    const auto nodes = body.find("nodes");
    if (nodes == body.end() || !nodes->is_array())
        throw std::invalid_argument("graph definition requires a nodes array: " + name);
    const auto outputs = body.find("outputs");
    if (outputs == body.end() || !outputs->is_object())
        throw std::invalid_argument("graph definition requires an outputs object: " + name);
    std::set<std::string> ids;
    for (const auto &node : *nodes) {
        object(node, "graph definition node must be an object");
        const auto id = opNodeName(node.value("id", Json{}));
        if (id.find('/') != std::string::npos)
            throw GraphRegistryError("graph.definition_immutable@1",
                {{"node", id}, {"reason", "instance_path"}});
        if (!ids.insert(id).second)
            throw GraphRegistryError("graph.node_duplicate@1", {{"node", id}, {"scope", name}});
        if (node.value("op", std::string{}) == "graph.instance")
            throw GraphRegistryError("graph.nested_instance@1", {{"node", id}, {"scope", name}});
    }
}

void applyDefinitionOp(Json &candidate, const Json &operation, const std::string &kind,
                       std::vector<DeferredCheck> &checks,
                       std::vector<DeferredCheck> &recorded) {
    // A definition op acts on the document, not inside one body.
    if (const auto scope = operation.find("scope");
        scope != operation.end() && !scope->is_null() && *scope != kRootScope)
        throw std::invalid_argument("graph definition ops take no scope: " + kind);
    const auto definitions = candidate.find("definitions");
    const bool present = definitions != candidate.end();
    const auto exists = [&](const std::string &name) {
        return present && definitions->contains(name);
    };
    if (kind == "add_definition") {
        const auto name = definitionName(operation, "name");
        requireNewDefinitionName(name);
        if (exists(name))
            throw GraphRegistryError("graph.definition_duplicate@1", {{"definition", name}});
        Json body{{"nodes", Json::array()}, {"outputs", Json::object()}};
        if (const auto found = operation.find("definition");
            found != operation.end() && !found->is_null()) {
            requireDefinitionBody(name, *found);
            body = *found;
        }
        candidate["definitions"][name] = std::move(body);
    } else if (kind == "remove_definition") {
        const auto name = definitionName(operation, "name");
        if (!exists(name))
            throw GraphRegistryError("graph.definition_missing@1", {{"definition", name}});
        definitions->erase(name);
        checks.push_back({DeferredCheck::Kind::definition_unreferenced, {}, {}, name, {}});
    } else {
        const auto from = definitionName(operation, "from");
        const auto to = definitionName(operation, "to");
        if (!exists(from))
            throw GraphRegistryError("graph.definition_missing@1", {{"definition", from}});
        requireNewDefinitionName(to);
        if (exists(to))
            throw GraphRegistryError("graph.definition_duplicate@1", {{"definition", to}});
        auto body = std::move(definitions->at(from));
        definitions->erase(from);
        (*definitions)[to] = std::move(body);
        // Every name moves with the body in this same op, so the document is
        // never left naming a definition that is not there.
        visitDefinitionReferences(candidate, from,
            [&](const std::string &, Json &node, const Json::json_pointer &field) {
                node[field] = to;
            });
        // And what earlier ops asked to be checked follows it too.
        for (auto &check : recorded) {
            if (check.scope == from) check.scope = to;
            if (check.definition == from) check.definition = to;
        }
    }
}

void applyOperation(Json &candidate, const Json &operation, const std::string &kind,
                    std::vector<DeferredCheck> &checks, std::vector<DeferredCheck> &recorded) {
    if (isDefinitionOp(kind)) {
        applyDefinitionOp(candidate, operation, kind, checks, recorded);
        return;
    }
    // The definition bodies are read fresh for every op: one op in the list can
    // change what the next one is allowed to do.
    static const Json no_definitions = Json::object();
    const auto definitionsOf = [&]() -> const Json & {
        const auto found = candidate.find("definitions");
        return found == candidate.end() ? no_definitions : *found;
    };
    {
        const auto scope = resolveEditScope(candidate, operation);
        auto &nodes = *scope.nodes;
        const auto &definitions = definitionsOf();
        if (kind == "add_node" || kind == "replace_node") {
            const auto &node = operation.at("node");
            object(node, "graph node edit requires a node object");
            const auto id = opNodeName(node.at("id"));
            if (id.find('/') != std::string::npos)
                throw GraphRegistryError("graph.definition_immutable@1",
                    {{"node", id}, {"reason", "instance_path"}});
            const auto op = node.value("op", std::string{});
            // The two structural operations belong to one side each: an
            // instance stands in the root document, a boundary port inside a
            // definition. Nesting one in the wrong body is refused by name.
            if (op == "graph.instance" && !scope.root)
                throw GraphRegistryError("graph.nested_instance@1",
                    {{"node", id}, {"scope", scope.name}});
            if (op == "graph.input" && scope.root)
                throw GraphRegistryError("graph.input_scope@1", {{"node", id}});
            if (kind == "replace_node") {
                // Restoring a node preserves all authored fields, including
                // absent parameters and extension fields. The full candidate
                // is validated below just like an ordinary graph edit.
                locateNode(scope, definitions, id) = node;
                pruneScopeLayout(scope);
                return;
            }
            if (std::any_of(nodes.begin(), nodes.end(), [&](const Json &existing) {
                    return existing.contains("id") && existing.at("id") == id;
                }))
                throw GraphRegistryError("graph.node_duplicate@1", {{"node", id}});
            if (const auto index = operation.find("index"); index != operation.end()) {
                if ((!index->is_number_unsigned() && !index->is_number_integer()) ||
                    (index->is_number_integer() && index->get<std::int64_t>() < 0) ||
                    index->get<std::uint64_t>() > nodes.size())
                    throw std::invalid_argument("add_node index must be within the node array");
                nodes.insert(nodes.begin() + index->get<Json::difference_type>(), node);
            } else {
                nodes.push_back(node);
            }
        } else if (kind == "remove_node") {
            const auto id = opNodeName(operation.at("node"));
            const auto removed_op = locateNode(scope, definitions, id).value("op", std::string{});
            // A boundary port an instance is still supplying is that instance's
            // port. It is named first because it is the reference the canvas
            // cannot show: the value lives outside the body being edited. The
            // instance side may be cleared later in the list, so the check is
            // recorded, and runs here only if this op fails anyway.
            if (!scope.root && removed_op == "graph.input")
                checks.push_back({DeferredCheck::Kind::input_unreferenced, {}, {},
                                  scope.name, id});
            // items() borrows its container, so the object has to outlive the
            // loop rather than be a temporary from value().
            const auto outputs =
                scope.outputs != nullptr && scope.outputs->is_object() ? *scope.outputs : Json::object();
            for (const auto &[port, endpoint] : outputs.items())
                if (endpoint.is_object() && endpoint.value("node", std::string{}) == id)
                    throw GraphRegistryError("graph.output_referenced@1",
                        {{"node", id}, {"output", port}});
            for (const auto &node : nodes) {
                if (!node.contains("id") || node.at("id") == id) continue;
                const auto inputs = node.value("inputs", Json::object());
                for (const auto &[port, endpoint] : inputs.items())
                    if (endpoint.is_object() && endpoint.value("node", std::string{}) == id)
                        throw GraphRegistryError("graph.output_referenced@1",
                            {{"node", id}, {"consumer", node.at("id")}, {"port", port}});
            }
            nodes.erase(std::find_if(nodes.begin(), nodes.end(), [&](const Json &node) {
                return node.contains("id") && node.at("id") == id;
            }));
            pruneScopeLayout(scope);
        } else if (kind == "set_input") {
            const auto id = opNodeName(operation.at("node"));
            const auto port = identifier(operation.at("port"));
            auto &node = locateNode(scope, definitions, id);
            const auto &source = operation.at("source");
            // The port may be declared by a later op in the list.
            if (node.value("op", std::string{}) == "graph.instance") {
                DeferredCheck check{DeferredCheck::Kind::instance_port,
                                    scope.root ? std::string{} : scope.name, id, {}, port};
                if (source.is_null()) {
                    const auto reference = node.value("definition", std::string{});
                    check.cleared_known_port =
                        declaresInput(candidate, reference, port) ||
                        std::any_of(recorded.begin(), recorded.end(), [&](const DeferredCheck &earlier) {
                            return earlier.kind == DeferredCheck::Kind::input_unreferenced &&
                                   earlier.definition == reference && earlier.port == port;
                        });
                }
                checks.push_back(std::move(check));
            }
            if (source.is_null()) {
                if (const auto inputs = node.find("inputs"); inputs != node.end()) {
                    inputs->erase(port);
                    if (inputs->empty()) node.erase("inputs");
                }
            } else {
                object(source, "set_input source must be an object or null");
                if (source.size() != 2 || !source.contains("node") || !source.contains("port"))
                    throw std::invalid_argument("set_input source requires exactly node and port");
                (void)identifier(source.at("node"));
                (void)identifier(source.at("port"));
                node["inputs"][port] = source;
            }
            pruneScopeLayout(scope);
        } else if (kind == "set_node_parameters") {
            const auto id = opNodeName(operation.at("node"));
            const auto &parameters = operation.at("parameters");
            object(parameters, "set_node_parameters requires a parameters object");
            auto &node = locateNode(scope, definitions, id);
            if (node.value("op", std::string{}) == "graph.instance")
                for (const auto &[name, value] : parameters.items()) {
                    (void)value;
                    checks.push_back({DeferredCheck::Kind::instance_port,
                                      scope.root ? std::string{} : scope.name, id, {}, name});
                }
            if (parameters.empty()) node.erase("parameters");
            else node["parameters"] = parameters;
        } else if (kind == "set_node_label") {
            // The authored, human-chosen name. It is not an identifier: nothing
            // refers to it, so clearing it is spelled as the empty string
            // rather than as a separate op.
            const auto id = opNodeName(operation.at("node"));
            const auto &label = operation.at("label");
            if (!label.is_string())
                throw std::invalid_argument("set_node_label requires a string label");
            auto &node = locateNode(scope, definitions, id);
            if (label.get_ref<const std::string &>().empty()) node.erase("label");
            else node["label"] = label;
        } else if (kind == "set_layout") {
            // Where the editor put things. The engine carries it and reads
            // none of it, so this op touches nothing an evaluation depends on:
            // it replaces one member beside `nodes` and leaves the rest of the
            // scope exactly as it was.
            const auto &layout = operation.at("layout");
            if (scope.container == nullptr)
                throw std::invalid_argument("graph document scope has no container");
            if (layout.is_null()) {
                scope.container->erase("layout");
            } else {
                requireLayout(scope, definitions, layout);
                (*scope.container)["layout"] = layout;
            }
        } else {
            throw std::invalid_argument("unknown graph document op: " + kind);
        }
    }
}
} // namespace

nlohmann::json editGraphDocument(const Json &document, const Json &ops) {
    if (!ops.is_array()) throw std::invalid_argument("graph document ops must be an array");
    auto candidate = document;
    if (!candidate.at("nodes").is_array())
        throw std::invalid_argument("graph nodes must be an array");
    if (const auto found = candidate.find("definitions"); found != candidate.end())
        object(*found, "graph definitions must be an object");
    // What the ops leave for the end: see DeferredCheck.
    std::vector<DeferredCheck> deferred;
    for (const auto &operation : ops) {
        object(operation, "graph document op must be an object");
        const auto kind = operation.value("op", std::string{});
        std::vector<DeferredCheck> checks;
        try {
            applyOperation(candidate, operation, kind, checks, deferred);
        } catch (...) {
            // An op refused for its own reasons reports the whole-document
            // check it recorded first, as it did when that check ran in place.
            for (const auto &check : checks) runDeferredCheck(candidate, check, true);
            throw;
        }
        deferred.insert(deferred.end(), checks.begin(), checks.end());
    }
    for (const auto &check : deferred) runDeferredCheck(candidate, check);
    // One expansion at the end: every op landed, or the document is refused
    // whole and the caller's document is untouched.
    (void)expandGraphDocument(candidate);
    return candidate;
}

nlohmann::json editGraphInstances(const Json &document, const Json &attachments, const Json &detachments) {
    if (!attachments.is_array() || !detachments.is_array())
        throw std::invalid_argument("graph attachments and detachments must be arrays");
    const auto &nodes = document.at("nodes");
    if (!nodes.is_array()) throw std::invalid_argument("graph nodes must be an array");
    auto ops = Json::array();
    std::set<std::string> removed;
    for (const auto &value : detachments) {
        const auto id = identifier(value);
        if (!removed.insert(id).second) throw std::invalid_argument("duplicate graph detachment: " + id);
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) { return node.at("id") == id; });
        if (found == nodes.end() || found->at("op") != "graph.instance" || !found->contains("owner"))
            throw std::invalid_argument("graph detachment requires an owned root instance: " + id);
        ops.push_back({{"op", "remove_node"}, {"node", id}});
    }
    for (const auto &instance : attachments) {
        if (!instance.is_object() || instance.value("op", std::string{}) != "graph.instance" || !instance.contains("owner"))
            throw std::invalid_argument("graph attachment requires an owned graph.instance");
        ops.push_back({{"op", "add_node"}, {"node", instance}});
    }
    return editGraphDocument(document, ops);
}

} // namespace Pelican
