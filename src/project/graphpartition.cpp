#include "graphpartition.hpp"
#include "shaderresourceport.hpp"

#include <stdexcept>

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
LogicalType resourceType(const LogicalTypeRegistry &registry, const Json &declaration) {
    const auto kind = declaration.value("kind", std::string{"buffer"});
    if (kind == "buffer") return bufferType(registry, declaration);
    if (kind != "image") throw std::invalid_argument("unknown graph resource kind: " + kind);
    const auto &type = declaration.at("type");
    std::vector<LogicalTypeArgumentInput> arguments;
    for (const auto &[name, value] : type.at("arguments").items())
        arguments.push_back({name, EnumValueId{value.get<std::string>()}});
    const auto result = registry.canonicalize(parseSemanticTypeId(type.at("semantic").get<std::string>()),
                                               std::move(arguments));
    if (result.constructor != LogicalTypeConstructor::image)
        throw std::invalid_argument("graph image requires an image logical type");
    return result;
}
}

GraphOperationContracts valueGraphOperationContracts(const LogicalTypeRegistry &registry) {
    const auto scalar = registry.canonicalize(parseSemanticTypeId("pelican.value.scalar@1"));
    const GraphInputContract required{exactLogicalTypePattern(registry, scalar)};
    const GraphInputContract optional{required.accepted_type, false};
    const std::map<std::string, LogicalType> output{{"value", scalar}};
    return {
        {"value.input", {{}, output}}, {"value.constant", {{}, output}},
        {"value.parameter", {{}, output}},
        {"value.trigger_clock", {{{"time", required}, {"trigger", required},
            {"paused", optional}, {"reset", optional}}, output}},
        {"value.add", {{{"a", required}, {"b", required}}, output}},
        {"value.multiply", {{{"a", required}, {"b", required}}, output}},
        {"value.curve", {{{"time", required}}, output}},
        {"value.playback", {{{"elapsed", required}, {"source_begin", optional},
            {"source_end", optional}, {"duration", optional}, {"speed", optional},
            {"delay", optional}, {"sample_interval", optional}}, output}},
        {"render.scalar", {{{"value", required}}, {}}},
        {"audio.volume", {{{"value", required}}, {}}},
        {"audio.pitch", {{{"value", required}}, {}}},
    };
}

GraphExecutionPartitions partitionGraphDocument(const Json &document) {
    const auto graph = expandGraphDocument(document);
    const auto registry = makeBuiltinLogicalTypeRegistry();
    const auto value_operations = valueGraphOperationContracts(registry);
    const auto scalar = registry.canonicalize(parseSemanticTypeId("pelican.value.scalar@1"));
    const auto output_types = validateGraphPorts(graph, registry, GraphOperationContractResolver{[&](const Json &node) {
        const auto op = node.at("op").get<std::string>();
        if (const auto found = value_operations.find(op); found != value_operations.end()) return found->second;
        const auto &parameters = node.at("parameters");
        GraphOperationContract contract;
        if (op == "render.buffer") {
            contract.outputs.emplace("resource", bufferType(registry, parameters));
            contract.inputs.emplace("value", GraphInputContract{exactLogicalTypePattern(registry, scalar), false});
        } else if (op == "render.image") {
            auto declaration = parameters;
            declaration["kind"] = "image";
            contract.outputs.emplace("resource", resourceType(registry, declaration));
        } else if (op == "render.compute") {
            const auto &ports = parameters.at("ports");
            if (!ports.is_object()) throw std::invalid_argument("compute ports must be an object");
            for (const auto &[port, declaration] : ports.items()) {
                const auto type = resourceType(registry, declaration);
                contract.inputs.emplace(port, GraphInputContract{exactLogicalTypePattern(registry, type)});
                const auto access = declaration.at("access").get<std::string>();
                if (access != "read" && access != "write" && access != "read_write")
                    throw std::invalid_argument("unknown compute port access: " + access);
                if (access != "read") contract.outputs.emplace(port, type);
            }
        } else throw std::invalid_argument("unknown graph operation: " + op);
        return contract;
    }});
    GraphExecutionPartitions result;
    result.values.instance_owners = graph.instance_owners;
    result.rendering.instance_owners = graph.instance_owners;
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        const auto op = node.at("op").get<std::string>();
        auto &partition = value_operations.contains(op) ? result.values : result.rendering;
        auto lowered = node;
        if (op == "render.buffer" && node.at("inputs").contains("value")) {
            auto &parameters = lowered.at("parameters");
            if (parameters.at("element") != "float" ||
                (parameters.contains("size") && parameters.at("size") != 4) ||
                (parameters.contains("host_source") && parameters.at("host_source") != "runtime_f32"))
                throw std::invalid_argument("scalar-connected buffer requires one runtime float: " + id);
            parameters["size"] = 4;
            parameters["host_source"] = "runtime_f32";
            lowered["inputs"] = Json::object();
            const auto sink_id = "$upload/" + id;
            if (graph.origins.contains(sink_id)) throw std::invalid_argument("reserved graph node: " + sink_id);
            result.values.nodes.push_back({{"id", sink_id}, {"op", "render.scalar"},
                {"parameters", {{"target", id}}},
                {"inputs", {{"value", node.at("inputs").at("value")}}}, {"outputs", Json::array()}});
            result.values.origins.emplace(sink_id, graph.origins.at(id));
            result.values.node_scopes.emplace(sink_id, graph.node_scopes.at(id));
        }
        partition.nodes.push_back(std::move(lowered));
        partition.origins.emplace(id, graph.origins.at(id));
        partition.node_scopes.emplace(id, graph.node_scopes.at(id));
    }
    for (const auto &[port, endpoint] : graph.outputs.items()) {
        auto &partition = output_types.at(port) == scalar ? result.values : result.rendering;
        partition.outputs[port] = endpoint;
        partition.output_scopes.emplace(port, graph.output_scopes.at(port));
    }
    return result;
}
}
