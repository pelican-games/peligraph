#include "graphcomposition.hpp"

#include <algorithm>
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
               const std::string &definition, const Json &arguments) {
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
        for (const auto &input : inputs)
            if (!arguments.contains(input)) throw std::invalid_argument("unconnected subgraph input: " + input);
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
            Json bound = Json::object();
            const auto authored_inputs = node.value("inputs", Json::object());
            object(authored_inputs, "instance inputs must be an object");
            for (const auto &[port, endpoint] : authored_inputs.items()) bound[port] = resolve(endpoint);
            if (!active_definitions.insert(ref).second) throw std::invalid_argument("recursive graph definition: " + ref);
            auto outputs = scope(definitions.at(ref), path + id + "/", ref, bound);
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
                return arguments.at(id);
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
            if (node->at("op") == "graph.input") continue;
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
        for (const auto &[port, endpoint] : outputs.items()) exposed[identifier(port)] = resolve(endpoint);
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
    expander.result.outputs = expander.scope(document, "", "", Json::object());
    return std::move(expander.result);
}

} // namespace Pelican
