#include "graphcomposition.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>

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

// Every `graph.instance` in the document that stands on this definition and
// supplies this boundary port -- with a stored parameter or with an edge. A
// nested instance inside another definition counts the same as a root one.
void requireInputUnreferenced(const Json &candidate, const std::string &definition,
                              const std::string &input) {
    const auto check = [&](const Json &nodes) {
        for (const auto &node : nodes) {
            if (!node.is_object() || node.value("op", std::string{}) != "graph.instance" ||
                node.value("definition", std::string{}) != definition)
                continue;
            const auto supplies = [&](const char *key) {
                const auto found = node.find(key);
                return found != node.end() && found->is_object() && found->contains(input);
            };
            if (!supplies("parameters") && !supplies("inputs")) continue;
            throw GraphRegistryError("graph.input_referenced@1",
                {{"definition", definition}, {"input", input},
                 {"instance", node.value("id", std::string{})}});
        }
    };
    const auto nodes = candidate.find("nodes");
    if (nodes != candidate.end() && nodes->is_array()) check(*nodes);
    const auto definitions = candidate.find("definitions");
    if (definitions == candidate.end() || !definitions->is_object()) return;
    for (const auto &[name, body] : definitions->items()) {
        (void)name;
        const auto body_nodes = body.find("nodes");
        if (body_nodes != body.end() && body_nodes->is_array()) check(*body_nodes);
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

void requireInstancePort(const Json &node, const Json &definitions, const std::string &port) {
    if (node.value("op", std::string{}) != "graph.instance") return;
    const auto declared = declaredInstanceInputs(node, definitions);
    if (declared.empty() || declared.contains(port)) return;
    throw GraphRegistryError("graph.input_unknown@1",
        {{"node", node.at("id")}, {"definition", node.value("definition", std::string{})},
         {"port", port}});
}
} // namespace

nlohmann::json editGraphDocument(const Json &document, const Json &ops) {
    if (!ops.is_array()) throw std::invalid_argument("graph document ops must be an array");
    auto candidate = document;
    if (!candidate.at("nodes").is_array())
        throw std::invalid_argument("graph nodes must be an array");
    if (const auto found = candidate.find("definitions"); found != candidate.end())
        object(*found, "graph definitions must be an object");
    // The definition bodies are read fresh for every op: one op in the list can
    // change what the next one is allowed to do.
    static const Json no_definitions = Json::object();
    const auto definitionsOf = [&]() -> const Json & {
        const auto found = candidate.find("definitions");
        return found == candidate.end() ? no_definitions : *found;
    };
    for (const auto &operation : ops) {
        object(operation, "graph document op must be an object");
        const auto kind = operation.value("op", std::string{});
        const auto scope = resolveEditScope(candidate, operation);
        auto &nodes = *scope.nodes;
        const auto &definitions = definitionsOf();
        if (kind == "add_node") {
            const auto &node = operation.at("node");
            object(node, "add_node requires a node object");
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
            if (std::any_of(nodes.begin(), nodes.end(), [&](const Json &existing) {
                    return existing.contains("id") && existing.at("id") == id;
                }))
                throw GraphRegistryError("graph.node_duplicate@1", {{"node", id}});
            nodes.push_back(node);
        } else if (kind == "remove_node") {
            const auto id = opNodeName(operation.at("node"));
            const auto removed_op = locateNode(scope, definitions, id).value("op", std::string{});
            // A boundary port an instance is still supplying is that instance's
            // port. It is named first because it is the reference the canvas
            // cannot show: the value lives outside the body being edited.
            if (!scope.root && removed_op == "graph.input")
                requireInputUnreferenced(candidate, scope.name, id);
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
        } else if (kind == "set_input") {
            const auto id = opNodeName(operation.at("node"));
            const auto port = identifier(operation.at("port"));
            auto &node = locateNode(scope, definitions, id);
            requireInstancePort(node, definitions, port);
            const auto &source = operation.at("source");
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
        } else if (kind == "set_node_parameters") {
            const auto id = opNodeName(operation.at("node"));
            const auto &parameters = operation.at("parameters");
            object(parameters, "set_node_parameters requires a parameters object");
            auto &node = locateNode(scope, definitions, id);
            for (const auto &[name, value] : parameters.items()) {
                (void)value;
                requireInstancePort(node, definitions, name);
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
