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
            if (arguments.contains(parameter)) throw std::invalid_argument("subgraph input also bound as parameter: " + parameter);
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
    return std::move(expander.result);
}

std::map<std::string, LogicalType> validateGraphPorts(
    const ExpandedGraphDocument &graph, const LogicalTypeRegistry &registry,
    const GraphOperationContracts &operations) {
    return validateGraphPorts(graph, registry, GraphOperationContractResolver{
        [&](const Json &node) {
            const auto op = node.at("op").get<std::string>();
            const auto found = operations.find(op);
            if (found == operations.end())
                throw std::invalid_argument("unknown graph operation: " + node.at("id").get<std::string>() + " (" + op + ")");
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

} // namespace Pelican
