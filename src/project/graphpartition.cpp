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
}

GraphExecutionPartitions partitionGraphDocument(const Json &document) {
    return partitionGraphDocument(document, builtinGraphModuleRegistry());
}

GraphExecutionPartitions partitionGraphDocument(const Json &document,
                                                const GraphModuleRegistry &modules) {
    const auto graph = expandGraphDocument(document);
    const auto &registry = modules.typeRegistry();
    // The module that registered an operation also declares which domain runs
    // it, so this partition never needs to know an operation's name.
    std::map<std::string, GraphExecutionDomain> domains;
    const auto output_types = validateGraphPorts(graph, registry, GraphOperationContractResolver{[&](const Json &node) {
        auto contract = modules.resolveOperation(node);
        domains.insert_or_assign(node.at("id").get<std::string>(), contract.domain);
        return contract;
    }});
    GraphExecutionPartitions result;
    result.values.instance_owners = graph.instance_owners;
    result.rendering.instance_owners = graph.instance_owners;
    result.animation.instance_owners = graph.instance_owners;
    // LP-1a: loops expand into render declarations after composition.
    result.rendering.loops = graph.loops;
    const auto domain_partition = [&result](GraphExecutionDomain domain) -> ExpandedGraphDocument & {
        switch (domain) {
        case GraphExecutionDomain::values: return result.values;
        case GraphExecutionDomain::rendering: return result.rendering;
        case GraphExecutionDomain::animation: return result.animation;
        }
        throw std::invalid_argument("unknown graph execution domain");
    };
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        const auto op = node.at("op").get<std::string>();
        auto &partition = domain_partition(domains.at(id));
        // An animation node reads this frame's scalars through the values
        // partition. Publishing the edge as an output is the whole coupling:
        // the animation phase never writes a value back.
        if (domains.at(id) == GraphExecutionDomain::animation)
            for (const auto &[port, endpoint] : node.at("inputs").items()) {
                if (domains.at(endpoint.at("node").get<std::string>()) != GraphExecutionDomain::values ||
                    endpoint.at("port") != "value")
                    continue; // Only scalars cross; typed streams wait for their own pass.
                const auto output = graphAnimationScalarOutput(id, port);
                if (graph.outputs.contains(output))
                    throw std::invalid_argument("reserved graph output: " + output);
                result.values.outputs[output] = endpoint;
                result.values.output_scopes.emplace(output, graph.node_scopes.at(id));
            }
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
    // Publish every scalar instance input port as an output of the values
    // partition. Without this the effective value of a connected port is not
    // observable anywhere: the expansion inlines the outer producer into the
    // consumer and leaves no node to read.
    for (const auto &[key, input] : graph.instance_inputs) {
        const auto producer = input.endpoint.at("node").get<std::string>();
        const auto domain = domains.find(producer);
        if (domain == domains.end() || domain->second != GraphExecutionDomain::values ||
            input.endpoint.at("port") != "value")
            continue; // Only scalars; a typed stream waits for its own pass.
        const auto output = graphInstanceInputOutput(key);
        if (graph.outputs.contains(output))
            throw std::invalid_argument("reserved graph output: " + output);
        result.values.outputs[output] = input.endpoint;
        result.values.output_scopes.emplace(output, input.scopes);
        result.values.instance_inputs.emplace(key, input);
    }
    for (const auto &[port, endpoint] : graph.outputs.items()) {
        const auto type = output_types.at(port).constructor;
        const auto producer = domains.at(endpoint.at("node").get<std::string>());
        auto &partition = producer == GraphExecutionDomain::animation ? result.animation
            : type == LogicalTypeConstructor::image || type == LogicalTypeConstructor::buffer
            ? result.rendering : result.values;
        partition.outputs[port] = endpoint;
        partition.output_scopes.emplace(port, graph.output_scopes.at(port));
    }
    return result;
}

std::string graphAnimationScalarOutput(std::string_view node, std::string_view port) {
    return "$pose/" + std::string{node} + "/" + std::string{port};
}

std::string graphInstanceInputOutput(std::string_view input) {
    return "$value/" + std::string{input};
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
