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
// Only the root `nodes` array is editable in v0. A node inside a definition,
// or named across an instance path, is refused by name rather than reported as
// missing, because the two call for different fixes.
Json &locateRootNode(Json &nodes, const Json &definitions, const std::string &id) {
    if (id.find('/') != std::string::npos)
        throw GraphRegistryError("graph.definition_immutable@1",
            {{"node", id}, {"reason", "instance_path"}});
    const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const Json &node) {
        return node.contains("id") && node.at("id") == id;
    });
    if (found != nodes.end()) return *found;
    for (const auto &[definition, body] : definitions.items())
        for (const auto &node : body.value("nodes", Json::array()))
            if (node.value("id", std::string{}) == id)
                throw GraphRegistryError("graph.definition_immutable@1",
                    {{"node", id}, {"definition", definition}});
    throw GraphRegistryError("graph.node_missing@1", {{"node", id}});
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
    auto &nodes = candidate.at("nodes");
    if (!nodes.is_array()) throw std::invalid_argument("graph nodes must be an array");
    const auto definitions = candidate.value("definitions", Json::object());
    object(definitions, "graph definitions must be an object");
    for (const auto &operation : ops) {
        object(operation, "graph document op must be an object");
        const auto kind = operation.value("op", std::string{});
        if (kind == "add_node") {
            const auto &node = operation.at("node");
            object(node, "add_node requires a node object");
            const auto id = opNodeName(node.at("id"));
            if (id.find('/') != std::string::npos)
                throw GraphRegistryError("graph.definition_immutable@1",
                    {{"node", id}, {"reason", "instance_path"}});
            if (std::any_of(nodes.begin(), nodes.end(), [&](const Json &existing) {
                    return existing.contains("id") && existing.at("id") == id;
                }))
                throw GraphRegistryError("graph.node_duplicate@1", {{"node", id}});
            nodes.push_back(node);
        } else if (kind == "remove_node") {
            const auto id = opNodeName(operation.at("node"));
            (void)locateRootNode(nodes, definitions, id);
            // items() borrows its container, so the object has to outlive the
            // loop rather than be a temporary from value().
            const auto outputs = candidate.value("outputs", Json::object());
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
            auto &node = locateRootNode(nodes, definitions, id);
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
            auto &node = locateRootNode(nodes, definitions, id);
            for (const auto &[name, value] : parameters.items()) {
                (void)value;
                requireInstancePort(node, definitions, name);
            }
            if (parameters.empty()) node.erase("parameters");
            else node["parameters"] = parameters;
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
