#include "rendergraphcomposition.hpp"
#include "graphcomposition.hpp"
#include "shaderresourceport.hpp"

#include <stdexcept>
#include <set>

namespace Pelican {
namespace {
using Json = nlohmann::json;
LogicalType bufferType(const LogicalTypeRegistry &registry, const Json &declaration) {
    const auto element = declaration.at("element").get<std::string>();
    if (!shaderResourceBufferElementFromName(element))
        throw std::invalid_argument("unknown graph buffer element: " + element);
    return registry.canonicalize(parseSemanticTypeId("pelican.render.storage_buffer@1"),
                                 {{"element", EnumValueId{element}}});
}
void forbid(const Json &parameters, std::initializer_list<const char *> fields) {
    if (!parameters.is_object()) throw std::invalid_argument("render node parameters must be an object");
    for (const auto field : fields)
        if (parameters.contains(field))
            throw std::invalid_argument(std::string("render graph derives field from connections: ") + field);
}
}

Json lowerRenderGraphDocument(const Json &document) {
    const auto graph = expandGraphDocument(document);
    const auto registry = makeBuiltinLogicalTypeRegistry();
    validateGraphPorts(graph, registry, GraphOperationContractResolver{[&](const Json &node) {
        const auto op = node.at("op").get<std::string>();
        const auto &parameters = node.at("parameters");
        GraphOperationContract contract;
        if (op == "render.buffer") {
            contract.outputs.emplace("resource", bufferType(registry, parameters));
        } else if (op == "render.compute") {
            const auto &ports = parameters.at("ports");
            if (!ports.is_object()) throw std::invalid_argument("compute ports must be an object");
            for (const auto &[port, declaration] : ports.items()) {
                const auto type = bufferType(registry, declaration);
                contract.inputs.emplace(port, GraphInputContract{
                    exactLogicalTypePattern(registry, type)});
                if (declaration.at("access") != "read") contract.outputs.emplace(port, type);
            }
        } else {
            throw std::invalid_argument("unsupported render graph operation: " + op);
        }
        return contract;
    }});
    auto config = document.value("rendering", Json::object());
    // Existing global settings, presets and image passes can coexist during
    // migration, but buffer/compute declarations have one authoring owner.
    forbid(config, {"buffers", "compute_tasks"});
    config["buffers"] = Json::array();
    config["compute_tasks"] = Json::array();
    std::map<std::string, const Json *> nodes;
    for (const auto &node : graph.nodes) nodes.emplace(node.at("id").get<std::string>(), &node);
    std::set<std::string> resolving;
    std::function<std::string(const Json &)> resourceName = [&](const Json &endpoint) {
        const auto id = endpoint.at("node").get<std::string>();
        const auto &node = *nodes.at(id);
        if (node.at("op") == "render.buffer") return id;
        if (!resolving.insert(id).second)
            throw std::invalid_argument("cyclic compute resource connection: " + id);
        auto resource = resourceName(node.at("inputs").at(endpoint.at("port").get<std::string>()));
        resolving.erase(id);
        return resource;
    };
    for (const auto &node : graph.nodes) {
        auto parameters = node.at("parameters");
        const auto id = node.at("id").get<std::string>();
        if (node.at("op") == "render.buffer") {
            forbid(parameters, {"name"});
            parameters.erase("element");
            parameters["name"] = id;
            config["buffers"].push_back(std::move(parameters));
            continue;
        }
        forbid(parameters, {"name", "reads", "writes", "resource_ports"});
        auto ports = parameters.at("ports");
        parameters.erase("ports");
        parameters["name"] = id;
        parameters["reads"] = Json::array();
        parameters["writes"] = Json::array();
        parameters["resource_ports"] = Json::object();
        std::set<std::string> reads, writes;
        auto after = parameters.value("after", Json::array());
        if (!after.is_array()) throw std::invalid_argument("compute after must be an array");
        for (const auto &[port, declaration] : ports.items()) {
            for (const auto &[key, value] : declaration.items())
                if (key != "element" && key != "access")
                    throw std::invalid_argument("unknown compute port setting: " + key);
            const auto &endpoint = node.at("inputs").at(port);
            const auto producer = endpoint.at("node").get<std::string>();
            const auto resource = resourceName(endpoint);
            if (nodes.at(producer)->at("op") == "render.compute" &&
                std::find(after.begin(), after.end(), producer) == after.end())
                after.push_back(producer);
            const auto access = declaration.at("access").get<std::string>();
            if (access != "read" && access != "write" && access != "read_write")
                throw std::invalid_argument("unknown compute port access: " + access);
            if (access != "write") reads.insert(resource);
            if (access != "read") writes.insert(resource);
            parameters["resource_ports"][port] = {
                {"resource", resource}, {"kind", "buffer"},
                {"element", declaration.at("element")}, {"access", "storage"}};
        }
        parameters["reads"] = reads;
        parameters["writes"] = writes;
        parameters["after"] = std::move(after);
        config["compute_tasks"].push_back(std::move(parameters));
    }
    return config;
}
}
