#include "graphpartition.hpp"
#include "shaderresourceport.hpp"

#include <stdexcept>
#include <algorithm>
#include <limits>

namespace Pelican {
namespace {
using Json = nlohmann::json;
void validateAttributeSource(const Json &node, bool owned) {
    const auto id = node.at("id").get<std::string>();
    const auto &parameters = node.at("parameters");
    const auto &source = parameters.at("source");
    if (!source.is_object() || parameters.contains("count") || parameters.contains("size") ||
        parameters.contains("host_source") || !node.at("inputs").empty())
        throw std::invalid_argument("sourced attribute derives its data and count: " + id);
    const auto index = [&](const char *key) {
        const auto &value = source.at(key);
        if (!value.is_number_integer() ||
            (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
            value.get<std::uint64_t>() > (std::numeric_limits<std::uint32_t>::max)())
            throw std::invalid_argument("attribute source index must fit uint32: " + id);
    };
    if (!source.at("asset").is_string() || source.at("asset").get_ref<const std::string &>().empty())
        throw std::invalid_argument("attribute source requires a nonempty asset: " + id);
    if (source.contains("pose_node")) {
        if (source.size() != 2 || parameters.at("element") != "mat4" || parameters.at("domain") != "instance")
            throw std::invalid_argument("pose source requires asset/pose_node and an instance mat4: " + id);
        if (!owned) throw std::invalid_argument("pose attribute requires an owned graph instance: " + id);
        index("pose_node");
        return;
    }
    const bool indices = parameters.at("domain") == "index";
    const bool morph = source.contains("morph_target");
    const bool deformed = source.contains("deformed_node");
    if ((!indices && parameters.at("domain") != "vertex") ||
        source.size() != (indices ? 3 : ((morph || deformed) ? 5 : 4)) ||
        (deformed && (indices || morph || parameters.at("element") != "vec3" ||
            (source.at("semantic") != "POSITION" && source.at("semantic") != "NORMAL"))) ||
        (indices && (morph || source.contains("semantic"))))
        throw std::invalid_argument("attribute source requires asset, mesh, primitive and a vertex semantic: " + id);
    if (!indices && (!source.at("semantic").is_string() || source.at("semantic").get_ref<const std::string &>().empty()))
        throw std::invalid_argument("attribute source requires a nonempty semantic: " + id);
    if (deformed && !owned) throw std::invalid_argument("deformed attribute requires an owned graph instance: " + id);
    index("mesh");
    index("primitive");
    if (morph) index("morph_target");
    if (deformed) index("deformed_node");
}
LogicalType bufferType(const LogicalTypeRegistry &registry, const Json &declaration) {
    const auto element = declaration.at("element").get<std::string>();
    if (!shaderResourceBufferElementFromName(element))
        throw std::invalid_argument("unknown graph buffer element: " + element);
    if (declaration.contains("domain")) {
        if (declaration.at("domain") == "index" && element != "uint")
            throw std::invalid_argument("index attributes require uint elements");
        return registry.canonicalize(parseSemanticTypeId("pelican.render.attribute@1"),
            {{"element", EnumValueId{element}}, {"domain", EnumValueId{declaration.at("domain").get<std::string>()}}});
    }
    return registry.canonicalize(parseSemanticTypeId("pelican.render.storage_buffer@1"),
                                 {{"element", EnumValueId{element}}});
}
LogicalType resourceType(const LogicalTypeRegistry &registry, const Json &declaration) {
    const auto kind = declaration.value("kind", std::string{"buffer"});
    if (kind == "buffer") return bufferType(registry, declaration);
    if (kind != "image") throw std::invalid_argument("unknown graph resource kind: " + kind);
    if (declaration.contains("domain"))
        throw std::invalid_argument("attribute domains apply to buffer resources, not images");
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
    const auto event = registry.canonicalize(parseSemanticTypeId("pelican.event.playback_control@1"));
    const auto voice = registry.canonicalize(parseSemanticTypeId("pelican.audio.voice@1"));
    const GraphInputContract optional_voice{exactLogicalTypePattern(registry, voice), false};
    return {
        {"value.input", {{}, output}}, {"value.constant", {{}, output}},
        {"value.parameter", {{}, output}},
        {"value.morph_weight", {{}, output}},
        {"value.trigger_clock", {{{"time", required}, {"trigger", required},
            {"paused", optional}, {"reset", optional}}, output}},
        {"event.playback", {{}, {{"events", event}}}},
        {"value.event_clock", {{{"time", required},
            {"events", GraphInputContract{exactLogicalTypePattern(registry, event)}}}, output}},
        {"audio.playback", {{{"time", required},
            {"events", GraphInputContract{exactLogicalTypePattern(registry, event)}}}, {{"voice", voice}}}},
        {"value.add", {{{"a", required}, {"b", required}}, output}},
        {"value.multiply", {{{"a", required}, {"b", required}}, output}},
        {"value.curve", {{{"time", required}}, output}},
        {"value.playback", {{{"elapsed", required}, {"source_begin", optional},
            {"source_end", optional}, {"duration", optional}, {"speed", optional},
            {"delay", optional}, {"sample_interval", optional}}, output}},
        {"render.scalar", {{{"value", required}}, {}}},
        {"audio.volume", {{{"value", required}, {"voice", optional_voice}}, {}}},
        {"audio.pitch", {{{"value", required}, {"voice", optional_voice}}, {}}},
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
        if (op == "render.buffer" || op == "render.attribute") {
            if (op == "render.attribute" && !parameters.contains("domain"))
                throw std::invalid_argument("render.attribute requires an attribute domain");
            if (op == "render.buffer" && parameters.contains("domain"))
                throw std::invalid_argument("attribute domain requires render.attribute");
            contract.outputs.emplace("resource", bufferType(registry, parameters));
            contract.inputs.emplace("value", GraphInputContract{exactLogicalTypePattern(registry, scalar), false});
        } else if (op == "render.image") {
            auto declaration = parameters;
            declaration["kind"] = "image";
            contract.outputs.emplace("resource", resourceType(registry, declaration));
        } else if (op == "render.compute" || op == "render.fullscreen" || op == "render.raster") {
            const auto &ports = parameters.at("ports");
            if (!ports.is_object()) throw std::invalid_argument("render ports must be an object");
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
        if (op == "render.attribute" && lowered.at("parameters").contains("source")) {
            const bool owned = std::any_of(graph.node_scopes.at(id).begin(), graph.node_scopes.at(id).end(),
                [&](const auto &scope) { return graph.instance_owners.contains(scope); });
            validateAttributeSource(lowered, owned);
        } else if (op == "render.attribute") {
            auto &parameters = lowered.at("parameters");
            const auto &count = parameters.at("count");
            const auto stride = shaderResourceBufferElementStride(
                *shaderResourceBufferElementFromName(parameters.at("element").get<std::string>()));
            if (!count.is_number_integer() ||
                (count.is_number_integer() && !count.is_number_unsigned() && count.get<std::int64_t>() <= 0) ||
                count.get<std::uint64_t>() == 0 ||
                count.get<std::uint64_t>() > (std::numeric_limits<std::uint64_t>::max)() / stride)
                throw std::invalid_argument("attribute count must be positive and fit its buffer size: " + id);
            if (parameters.contains("size"))
                throw std::invalid_argument("attribute size is derived from count and element: " + id);
            parameters["size"] = count.get<std::uint64_t>() * stride;
        }
        if ((op == "render.buffer" || op == "render.attribute") && node.at("inputs").contains("value")) {
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
        const auto type = output_types.at(port).constructor;
        auto &partition = type == LogicalTypeConstructor::image || type == LogicalTypeConstructor::buffer
            ? result.rendering : result.values;
        partition.outputs[port] = endpoint;
        partition.output_scopes.emplace(port, graph.output_scopes.at(port));
    }
    return result;
}

Json resolveGraphAttributeSources(const Json &document, const GraphAttributeSourceResolver &resolve) {
    const auto graph = partitionGraphDocument(document).rendering;
    auto result = document;
    // Definitions may have several instances; each expanded resource receives
    // data, while its one authored declaration receives the derived count once.
    std::map<std::pair<std::string, std::string>, std::uint64_t> counts;
    for (const auto &node : graph.nodes) {
        const auto &parameters = node.at("parameters");
        if (node.at("op") != "render.attribute" || !parameters.contains("source")) continue;
        const auto id = node.at("id").get<std::string>();
        const auto count = resolve(id, parameters);
        const auto &origin = graph.origins.at(id);
        const auto [found, inserted] = counts.emplace(std::pair{origin.definition, origin.node}, count);
        if (!inserted) {
            if (found->second != count)
                throw std::invalid_argument("shared attribute source resolved inconsistent counts: " + id);
            continue;
        }
        auto &scope = origin.definition.empty() ? result : result.at("definitions").at(origin.definition);
        for (auto &authored : scope.at("nodes"))
            if (authored.at("id") == origin.node) {
                authored["parameters"].erase("source");
                authored["parameters"]["count"] = count;
                if (parameters.at("source").contains("pose_node"))
                    authored["parameters"]["host_source"] = "runtime_f32";
                break;
            }
    }
    (void)partitionGraphDocument(result); // Validate resolved count/stride overflow too.
    return result;
}
}
