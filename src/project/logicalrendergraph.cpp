#include "logicalrendergraph.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Pelican {

namespace {

void requireName(std::string_view name, std::string_view subject) {
    if (name.empty()) {
        throw std::runtime_error(std::string{subject} + " must not be empty");
    }
}

std::uint64_t signedMagnitude(std::int64_t value) {
    if (value >= 0) return static_cast<std::uint64_t>(value);
    return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

void requireCanonicalPositiveScale(const Rational &scale,
                                   std::string_view subject) {
    if (scale.numerator <= 0 || scale.denominator == 0) {
        throw std::runtime_error(std::string{subject} +
                                 " must be a positive rational");
    }
    if (std::gcd(signedMagnitude(scale.numerator), scale.denominator) != 1) {
        throw std::runtime_error(std::string{subject} + " must be canonical");
    }
}

const LogicalPortContract &requirePort(
    const std::map<std::string, const LogicalPortContract *, std::less<>> &ports,
    std::string_view name, std::string_view node_name) {
    const auto found = ports.find(name);
    if (found == ports.end()) {
        throw std::runtime_error("logical graph node '" + std::string{node_name} +
                                 "' references unknown port '" + std::string{name} + "'");
    }
    return *found->second;
}

nlohmann::ordered_json rationalToJson(const Rational &value) {
    return nlohmann::ordered_json{{"numerator", value.numerator},
                                  {"denominator", value.denominator}};
}

nlohmann::ordered_json logicalValueIdToJson(const LogicalValueId &value) {
    return nlohmann::ordered_json{{"resource", value.resource},
                                  {"version", value.version}};
}

bool accessIntentAccepts(LogicalAccessIntent intent, LogicalAccessMode access) {
    switch (intent) {
    case LogicalAccessIntent::automatic:
    case LogicalAccessIntent::attachment:
    case LogicalAccessIntent::storage:
    case LogicalAccessIntent::host:
        return true;
    case LogicalAccessIntent::sampled:
        return access == LogicalAccessMode::read;
    case LogicalAccessIntent::transfer:
        return access != LogicalAccessMode::read_write;
    }
    return false;
}

} // namespace

std::string_view logicalPortDirectionName(LogicalPortDirection direction) {
    switch (direction) {
    case LogicalPortDirection::input: return "input";
    case LogicalPortDirection::output: return "output";
    case LogicalPortDirection::input_output: return "input_output";
    }
    throw std::runtime_error("unknown logical port direction");
}

std::string_view logicalPortRelationKindName(LogicalPortRelationKind kind) {
    switch (kind) {
    case LogicalPortRelationKind::same_extent: return "same_extent";
    case LogicalPortRelationKind::extent_scale: return "extent_scale";
    case LogicalPortRelationKind::same_view_set: return "same_view_set";
    case LogicalPortRelationKind::same_samples: return "same_samples";
    case LogicalPortRelationKind::per_view: return "per_view";
    }
    throw std::runtime_error("unknown logical port relation kind");
}

std::string_view logicalMaterializationRequirementName(
    LogicalMaterializationRequirement requirement) {
    switch (requirement) {
    case LogicalMaterializationRequirement::virtual_resource: return "virtual";
    case LogicalMaterializationRequirement::preferred: return "preferred";
    case LogicalMaterializationRequirement::required: return "required";
    case LogicalMaterializationRequirement::external: return "external";
    }
    throw std::runtime_error("unknown logical materialization requirement");
}

std::string logicalValueIdName(const LogicalValueId &value) {
    return value.resource + "#" + std::to_string(value.version);
}

std::string_view logicalValueImportKindName(LogicalValueImportKind kind) {
    switch (kind) {
    case LogicalValueImportKind::graph_input: return "graph_input";
    case LogicalValueImportKind::previous_epoch: return "previous_epoch";
    case LogicalValueImportKind::external: return "external";
    case LogicalValueImportKind::legacy_implicit: return "legacy_implicit";
    }
    throw std::runtime_error("unknown logical value import kind");
}

std::string_view logicalAccessModeName(LogicalAccessMode access) {
    switch (access) {
    case LogicalAccessMode::read: return "read";
    case LogicalAccessMode::write: return "write";
    case LogicalAccessMode::read_write: return "read_write";
    }
    throw std::runtime_error("unknown logical access mode");
}

std::string_view logicalAccessIntentName(LogicalAccessIntent intent) {
    switch (intent) {
    case LogicalAccessIntent::automatic: return "automatic";
    case LogicalAccessIntent::sampled: return "sampled";
    case LogicalAccessIntent::attachment: return "attachment";
    case LogicalAccessIntent::storage: return "storage";
    case LogicalAccessIntent::transfer: return "transfer";
    case LogicalAccessIntent::host: return "host";
    }
    throw std::runtime_error("unknown logical access intent");
}

std::string_view logicalReadFootprintKindName(LogicalReadFootprintKind kind) {
    switch (kind) {
    case LogicalReadFootprintKind::none: return "none";
    case LogicalReadFootprintKind::same_pixel: return "same_pixel";
    case LogicalReadFootprintKind::neighborhood: return "neighborhood";
    case LogicalReadFootprintKind::arbitrary: return "arbitrary";
    case LogicalReadFootprintKind::temporal: return "temporal";
    }
    throw std::runtime_error("unknown logical read footprint kind");
}

LogicalResourceUse makeLogicalReadUse(std::string port, LogicalValueId input,
                                      LogicalReadFootprint footprint,
                                      LogicalAccessIntent intent) {
    LogicalResourceUse result;
    result.port = std::move(port);
    result.access = LogicalAccessMode::read;
    result.footprint = footprint;
    result.intent = intent;
    result.input_value = std::move(input);
    return result;
}

LogicalResourceUse makeLogicalWriteUse(std::string port, LogicalValueId output,
                                       LogicalAccessIntent intent) {
    LogicalResourceUse result;
    result.port = std::move(port);
    result.access = LogicalAccessMode::write;
    result.intent = intent;
    result.output_value = std::move(output);
    return result;
}

LogicalResourceUse makeLogicalReadWriteUse(
    std::string port, LogicalValueId input, LogicalValueId output,
    LogicalReadFootprint footprint, LogicalAccessIntent intent) {
    LogicalResourceUse result;
    result.port = std::move(port);
    result.access = LogicalAccessMode::read_write;
    result.footprint = footprint;
    result.intent = intent;
    result.input_value = std::move(input);
    result.output_value = std::move(output);
    return result;
}

std::string_view logicalGraphNodeKindName(LogicalGraphNodeKind kind) {
    switch (kind) {
    case LogicalGraphNodeKind::render: return "render";
    case LogicalGraphNodeKind::compute: return "compute";
    case LogicalGraphNodeKind::anchor: return "anchor";
    case LogicalGraphNodeKind::snapshot_copy: return "snapshot_copy";
    case LogicalGraphNodeKind::output_transform: return "output_transform";
    }
    throw std::runtime_error("unknown logical graph node kind");
}

std::vector<LogicalDataEdge> deriveLogicalDataEdges(
    const CompiledLogicalRenderGraph &graph) {
    using Producer = std::pair<std::string, std::string>;
    std::map<LogicalValueId, Producer> producers;
    std::set<LogicalValueId> imports;

    for (const auto &imported : graph.imports) {
        requireName(imported.value.resource, "logical imported value resource");
        if (imported.value.version != 0) {
            throw std::runtime_error("logical imported value must use version zero: " +
                                     logicalValueIdName(imported.value));
        }
        if (!imports.insert(imported.value).second) {
            throw std::runtime_error("duplicate logical value import: " +
                                     logicalValueIdName(imported.value));
        }
    }

    for (const auto &node : graph.nodes) {
        for (const auto &use : node.uses) {
            if (!use.output_value) continue;
            requireName(use.output_value->resource, "logical output value resource");
            if (use.output_value->version == 0) {
                throw std::runtime_error("logical node output must not produce version zero: " +
                                         logicalValueIdName(*use.output_value));
            }
            const auto [found, inserted] = producers.emplace(
                *use.output_value, Producer{node.name, use.port});
            if (!inserted) {
                throw std::runtime_error(
                    "logical value has multiple producers: " +
                    logicalValueIdName(*use.output_value) + " ('" +
                    found->second.first + "." + found->second.second + "' and '" +
                    node.name + "." + use.port + "')");
            }
        }
    }
    for (const auto &imported : imports) {
        if (producers.contains(imported)) {
            throw std::runtime_error("logical value is both imported and produced: " +
                                     logicalValueIdName(imported));
        }
    }

    std::vector<LogicalDataEdge> result;
    for (const auto &node : graph.nodes) {
        for (const auto &use : node.uses) {
            if (!use.input_value) continue;
            requireName(use.input_value->resource, "logical input value resource");
            if (const auto producer = producers.find(*use.input_value);
                producer != producers.end()) {
                if (producer->second.first == node.name) {
                    throw std::runtime_error(
                        "logical node consumes its own output value: " +
                        logicalValueIdName(*use.input_value));
                }
                result.push_back(LogicalDataEdge{
                    *use.input_value, producer->second.first,
                    producer->second.second, node.name, use.port});
            } else if (!imports.contains(*use.input_value)) {
                throw std::runtime_error("logical input value has no producer or import: " +
                                         logicalValueIdName(*use.input_value) +
                                         " consumed by '" + node.name + "." +
                                         use.port + "'");
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return std::tie(left.value, left.producer_node, left.producer_port,
                        left.consumer_node, left.consumer_port) <
               std::tie(right.value, right.producer_node, right.producer_port,
                        right.consumer_node, right.consumer_port);
    });
    return result;
}

void validateCompiledLogicalRenderGraph(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &graph) {
    requireName(graph.name, "logical graph name");
    std::map<std::string, const LogicalResourceDesc *, std::less<>> resources;
    for (const auto &resource : graph.resources) {
        requireName(resource.name, "logical resource name");
        types.requireCanonical(resource.type);
        if (!resources.emplace(resource.name, &resource).second) {
            throw std::runtime_error("duplicate logical resource: " + resource.name);
        }
    }
    for (const auto &imported : graph.imports) {
        if (!resources.contains(imported.value.resource)) {
            throw std::runtime_error("logical value import references unknown resource: " +
                                     logicalValueIdName(imported.value));
        }
    }

    std::map<std::string, const LogicalGraphNode *, std::less<>> nodes;
    for (const auto &node : graph.nodes) {
        requireName(node.name, "logical graph node name");
        if (!nodes.emplace(node.name, &node).second) {
            throw std::runtime_error("duplicate logical graph node: " + node.name);
        }
    }

    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> edges;
    std::map<std::string, std::size_t, std::less<>> indegree;
    for (const auto &[name, unused] : nodes) {
        (void)unused;
        edges.emplace(name, std::set<std::string, std::less<>>{});
        indegree.emplace(name, 0);
    }
    const auto add_edge = [&](std::string_view from, std::string_view to) {
        if (from == to) {
            throw std::runtime_error("logical graph node has a self dependency: " +
                                     std::string{from});
        }
        if (!nodes.contains(from)) {
            throw std::runtime_error("logical graph dependency references unknown node: " +
                                     std::string{from});
        }
        if (!nodes.contains(to)) {
            throw std::runtime_error("logical graph dependency references unknown node: " +
                                     std::string{to});
        }
        if (edges.at(std::string{from}).insert(std::string{to}).second) {
            ++indegree.at(std::string{to});
        }
    };

    for (const auto &node : graph.nodes) {
        std::map<std::string, const LogicalPortContract *, std::less<>> ports;
        for (const auto &port : node.ports) {
            requireName(port.name, "logical port name");
            if (!port.accepted_type.semantic) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' port '" + port.name +
                                         "' requires a nominal semantic type");
            }
            if (!ports.emplace(port.name, &port).second) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' has duplicate port '" + port.name + "'");
            }
        }
        std::set<std::string, std::less<>> used_ports;
        for (const auto &use : node.uses) {
            const auto &port = requirePort(ports, use.port, node.name);
            if (!used_ports.insert(use.port).second) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' uses port more than once: " + use.port);
            }
            const auto has_input = use.input_value.has_value();
            const auto has_output = use.output_value.has_value();
            const auto value_shape_matches =
                (use.access == LogicalAccessMode::read && has_input && !has_output) ||
                (use.access == LogicalAccessMode::write && !has_input && has_output) ||
                (use.access == LogicalAccessMode::read_write && has_input && has_output);
            if (!value_shape_matches) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' port '" + use.port +
                                         "' has invalid value bindings for access " +
                                         std::string{logicalAccessModeName(use.access)});
            }
            if (has_input && has_output &&
                use.input_value->resource != use.output_value->resource) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' read-write port '" + use.port +
                                         "' must keep one resource family");
            }
            const auto &value = has_input ? *use.input_value : *use.output_value;
            const auto resource = resources.find(value.resource);
            if (resource == resources.end()) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' references unknown resource '" +
                                         value.resource + "'");
            }
            const auto type_match =
                matchLogicalType(types, resource->second->type, port.accepted_type);
            if (type_match.status == LogicalTypeMatchStatus::rejected) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' port '" + port.name +
                                         "' rejects resource '" + value.resource +
                                         "': " + type_match.reason_code + " (" +
                                         type_match.detail + ")");
            }
            const auto direction_matches =
                (port.direction == LogicalPortDirection::input &&
                 use.access == LogicalAccessMode::read) ||
                (port.direction == LogicalPortDirection::output &&
                 use.access == LogicalAccessMode::write) ||
                (port.direction == LogicalPortDirection::input_output &&
                 use.access == LogicalAccessMode::read_write);
            if (!direction_matches) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' port '" + port.name +
                                         "' direction/access mismatch");
            }
            if (!accessIntentAccepts(use.intent, use.access)) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' port '" + port.name + "' intent " +
                                         std::string{logicalAccessIntentName(use.intent)} +
                                         " rejects access " +
                                         std::string{logicalAccessModeName(use.access)});
            }
            const auto reads = use.access == LogicalAccessMode::read ||
                               use.access == LogicalAccessMode::read_write;
            if (reads && use.footprint.kind == LogicalReadFootprintKind::none) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' read port '" + port.name +
                                         "' requires a footprint");
            }
            if (!reads && use.footprint.kind != LogicalReadFootprintKind::none) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' write port '" + port.name +
                                         "' must not declare a read footprint");
            }
            if (use.footprint.kind == LogicalReadFootprintKind::neighborhood) {
                if (use.footprint.radius && *use.footprint.radius == 0) {
                    throw std::runtime_error("logical graph node '" + node.name +
                                             "' neighborhood radius must be positive");
                }
            } else if (use.footprint.radius) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' radius is only valid for neighborhood reads");
            }
        }
        if (used_ports.size() != ports.size()) {
            for (const auto &[port_name, unused] : ports) {
                (void)unused;
                if (!used_ports.contains(port_name)) {
                    throw std::runtime_error("logical graph node '" + node.name +
                                             "' has unbound port '" + port_name + "'");
                }
            }
        }

        for (const auto &port : node.ports) {
            std::set<std::pair<LogicalPortRelationKind, std::string>> seen_relations;
            for (const auto &relation : port.relations) {
                if (!seen_relations.emplace(relation.kind, relation.other_port).second) {
                    throw std::runtime_error("logical graph node '" + node.name +
                                             "' port '" + port.name +
                                             "' has duplicate relation");
                }
                if (relation.kind == LogicalPortRelationKind::per_view) {
                    if (!relation.other_port.empty()) {
                        throw std::runtime_error("per_view relation must not name another port");
                    }
                } else {
                    requirePort(ports, relation.other_port, node.name);
                    if (relation.other_port == port.name) {
                        throw std::runtime_error("logical port relation must reference another port: " +
                                                 port.name);
                    }
                }
                if (relation.kind == LogicalPortRelationKind::extent_scale) {
                    requireCanonicalPositiveScale(relation.scale_x,
                                                  "logical extent scale x");
                    requireCanonicalPositiveScale(relation.scale_y,
                                                  "logical extent scale y");
                } else if (relation.scale_x != Rational{1, 1} ||
                           relation.scale_y != Rational{1, 1}) {
                    throw std::runtime_error("logical port scale is only valid for extent_scale");
                }
            }
        }

        std::set<std::string, std::less<>> regions;
        for (const auto &region : node.region_tags) {
            requireName(region, "logical graph region tag");
            if (region.size() >
                maximumLogicalRegionTagBytes) {
                throw std::runtime_error(
                    "logical graph node '" + node.name +
                    "' has an overlong region tag");
            }
            if (!regions.insert(region).second) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' has duplicate region tag '" + region + "'");
            }
        }
        for (const auto &dependency : node.after) add_edge(dependency, node.name);
        for (const auto &dependent : node.before) add_edge(node.name, dependent);
    }

    for (const auto &edge : deriveLogicalDataEdges(graph)) {
        add_edge(edge.producer_node, edge.consumer_node);
    }

    std::set<std::string, std::less<>> ready;
    for (const auto &[name, degree] : indegree) {
        if (degree == 0) ready.insert(name);
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto current = *ready.begin();
        ready.erase(ready.begin());
        ++visited;
        for (const auto &dependent : edges.at(current)) {
            auto &degree = indegree.at(dependent);
            if (--degree == 0) ready.insert(dependent);
        }
    }
    if (visited != nodes.size()) {
        throw std::runtime_error("logical graph contains a dependency cycle: " +
                                 graph.name);
    }

    if (graph.render_strategy) {
        const auto &selection =
            *graph.render_strategy;
        requireName(
            selection.name,
            "render strategy name");
        requireName(
            selection.provider,
            "render strategy provider");
        requireName(
            selection.implementation,
            "render strategy implementation");
        requireName(
            selection.contract,
            "render strategy contract");
        requireName(
            selection.output_contract,
            "render strategy output contract");
        if (selection.graph_variant != "flat" &&
            selection.graph_variant != "preview" &&
            selection.graph_variant != "xr") {
            throw std::runtime_error(
                "logical graph render strategy has an "
                "invalid graph variant: " +
                selection.graph_variant);
        }
        if (selection.facade_capability_bits == 0 ||
            selection.input_config_fingerprint == 0 ||
            selection.output_config_fingerprint == 0 ||
            selection.provider_identity == 0 ||
            selection.provider_generation == 0 ||
            selection.provider_version == 0 ||
            selection.provider_capability_bits == 0) {
            throw std::runtime_error(
                "logical graph render strategy has "
                "incomplete provider or config provenance: " +
                selection.name);
        }
    }

    std::set<std::string, std::less<>>
        transform_names;
    std::set<std::uint32_t>
        transform_indices;
    for (const auto &selection :
         graph.graph_transforms) {
        requireName(
            selection.name,
            "logical graph transform name");
        requireName(
            selection.provider,
            "logical graph transform provider");
        requireName(
            selection.implementation,
            "logical graph transform implementation");
        requireName(
            selection.contract,
            "logical graph transform contract");
        if (!transform_names
                 .insert(selection.name)
                 .second) {
            throw std::runtime_error(
                "logical graph has duplicate transform "
                "selection: " +
                selection.name);
        }
        if (!transform_indices
                 .insert(selection.transform_index)
                 .second ||
            selection.transform_index >=
                graph.graph_transforms.size()) {
            throw std::runtime_error(
                "logical graph transform has an invalid or "
                "duplicate chain index: " +
                selection.name);
        }
        if (selection.boundary_fingerprint == 0 ||
            selection.input_graph_fingerprint == 0 ||
            selection.output_graph_fingerprint == 0 ||
            selection.provider_identity == 0 ||
            selection.provider_generation == 0 ||
            selection.provider_version == 0 ||
            selection.provider_capability_bits == 0) {
            throw std::runtime_error(
                "logical graph transform has incomplete "
                "provider or graph provenance: " +
                selection.name);
        }
    }

    std::set<std::string, std::less<>>
        replacement_regions;
    for (const auto &selection :
         graph.subgraph_replacements) {
        requireName(
            selection.region,
            "logical subgraph replacement region");
        if (selection.region.size() >
            maximumLogicalRegionTagBytes) {
            throw std::runtime_error(
                "logical subgraph replacement region is "
                "too long: " +
                selection.region);
        }
        requireName(
            selection.provider,
            "logical subgraph replacement provider");
        requireName(
            selection.implementation,
            "logical subgraph replacement implementation");
        requireName(
            selection.contract,
            "logical subgraph replacement contract");
        if (!replacement_regions
                 .insert(selection.region)
                 .second) {
            throw std::runtime_error(
                "logical graph has duplicate subgraph replacement "
                "selection for region: " +
                selection.region);
        }
        if (selection.provider_identity == 0 ||
            selection.provider_generation == 0 ||
            selection.provider_version == 0 ||
            selection.provider_capability_bits == 0) {
            throw std::runtime_error(
                "logical subgraph replacement has incomplete provider "
                "provenance: " +
                selection.region);
        }
        if (selection.source_nodes.empty() ||
            selection.replacement_nodes.empty()) {
            throw std::runtime_error(
                "logical subgraph replacement requires source and "
                "replacement node provenance: " +
                selection.region);
        }
        std::set<std::string, std::less<>>
            source_names;
        for (const auto &source :
             selection.source_nodes) {
            requireName(
                source,
                "logical subgraph source node");
            if (!source_names.insert(source).second) {
                throw std::runtime_error(
                    "logical subgraph replacement has duplicate source "
                    "node: " +
                    source);
            }
        }
        std::set<std::string, std::less<>>
            replacement_names;
        for (const auto &replacement :
             selection.replacement_nodes) {
            requireName(
                replacement,
                "logical subgraph replacement node");
            if (!replacement_names
                     .insert(replacement)
                     .second) {
                throw std::runtime_error(
                    "logical subgraph replacement has duplicate "
                    "replacement node: " +
                    replacement);
            }
            const auto found = nodes.find(replacement);
            if (found == nodes.end() ||
                std::find(
                    found->second->region_tags.begin(),
                    found->second->region_tags.end(),
                    selection.region) ==
                    found->second->region_tags.end()) {
                throw std::runtime_error(
                    "logical subgraph replacement node does not belong "
                    "to selected region: " +
                    replacement);
            }
        }
    }

    for (const auto &decision : graph.decisions) {
        requireName(decision.code, "logical compile decision code");
        requireName(decision.subject, "logical compile decision subject");
    }
}

nlohmann::ordered_json compiledLogicalRenderGraphToJson(
    const CompiledLogicalRenderGraph &graph) {
    nlohmann::ordered_json result;
    result["schema"] = "pelican.logical_render_graph";
    result["version"] = 2;
    result["graph"] = graph.name;
    result["resources"] = nlohmann::ordered_json::array();
    for (const auto &resource : graph.resources) {
        result["resources"].push_back(nlohmann::ordered_json{
            {"name", resource.name},
            {"type", logicalTypeToJson(resource.type)},
            {"materialization",
             logicalMaterializationRequirementName(resource.materialization)}});
    }
    result["imports"] = nlohmann::ordered_json::array();
    auto imports = graph.imports;
    std::sort(imports.begin(), imports.end(), [](const auto &left, const auto &right) {
        if (left.value != right.value) return left.value < right.value;
        return logicalValueImportKindName(left.kind) <
               logicalValueImportKindName(right.kind);
    });
    for (const auto &imported : imports) {
        result["imports"].push_back(nlohmann::ordered_json{
            {"value", logicalValueIdToJson(imported.value)},
            {"kind", logicalValueImportKindName(imported.kind)}});
    }
    result["nodes"] = nlohmann::ordered_json::array();
    for (const auto &node : graph.nodes) {
        nlohmann::ordered_json encoded{
            {"name", node.name},
            {"kind", logicalGraphNodeKindName(node.kind)},
            {"declaration_index", node.declaration_index},
            {"ports", nlohmann::ordered_json::array()},
            {"uses", nlohmann::ordered_json::array()},
            {"after", node.after},
            {"before", node.before},
            {"regions", node.region_tags},
        };
        for (const auto &port : node.ports) {
            nlohmann::ordered_json encoded_port{
                {"name", port.name},
                {"direction", logicalPortDirectionName(port.direction)},
                {"accepted_type", logicalTypePatternToJson(port.accepted_type)},
                {"relations", nlohmann::ordered_json::array()},
            };
            for (const auto &relation : port.relations) {
                encoded_port["relations"].push_back(nlohmann::ordered_json{
                    {"kind", logicalPortRelationKindName(relation.kind)},
                    {"other_port", relation.other_port},
                    {"scale_x", rationalToJson(relation.scale_x)},
                    {"scale_y", rationalToJson(relation.scale_y)},
                });
            }
            encoded["ports"].push_back(std::move(encoded_port));
        }
        for (const auto &use : node.uses) {
            nlohmann::ordered_json footprint{
                {"kind", logicalReadFootprintKindName(use.footprint.kind)}};
            if (use.footprint.radius) footprint["radius"] = *use.footprint.radius;
            nlohmann::ordered_json encoded_use{
                {"port", use.port},
                {"access", logicalAccessModeName(use.access)},
                {"intent", logicalAccessIntentName(use.intent)},
                {"footprint", std::move(footprint)},
            };
            if (use.input_value) {
                encoded_use["input_value"] =
                    logicalValueIdToJson(*use.input_value);
            }
            if (use.output_value) {
                encoded_use["output_value"] =
                    logicalValueIdToJson(*use.output_value);
            }
            encoded["uses"].push_back(std::move(encoded_use));
        }
        result["nodes"].push_back(std::move(encoded));
    }
    result["data_edges"] = nlohmann::ordered_json::array();
    for (const auto &edge : deriveLogicalDataEdges(graph)) {
        result["data_edges"].push_back(nlohmann::ordered_json{
            {"value", logicalValueIdToJson(edge.value)},
            {"producer", nlohmann::ordered_json{{"node", edge.producer_node},
                                                 {"port", edge.producer_port}}},
            {"consumer", nlohmann::ordered_json{{"node", edge.consumer_node},
                                                 {"port", edge.consumer_port}}},
        });
    }
    if (graph.render_strategy) {
        const auto &selection =
            *graph.render_strategy;
        result["render_strategy"] =
            nlohmann::ordered_json{
                {"name", selection.name},
                {"provider", selection.provider},
                {"implementation",
                 selection.implementation},
                {"contract", selection.contract},
                {"output_contract",
                 selection.output_contract},
                {"graph_variant",
                 selection.graph_variant},
                {"facade_capability_bits",
                 selection.facade_capability_bits},
                {"input_config_fingerprint",
                 selection.input_config_fingerprint},
                {"output_config_fingerprint",
                 selection.output_config_fingerprint},
                {"provider_owner",
                 selection.provider_owner},
                {"provider_identity",
                 selection.provider_identity},
                {"provider_generation",
                 selection.provider_generation},
                {"provider_version",
                 selection.provider_version},
                {"provider_capability_bits",
                 selection.provider_capability_bits},
                {"explicitly_selected",
                 selection.explicitly_selected},
            };
    }
    result["graph_transforms"] =
        nlohmann::ordered_json::array();
    for (const auto &selection :
         graph.graph_transforms) {
        result["graph_transforms"].push_back(
            nlohmann::ordered_json{
                {"name", selection.name},
                {"provider", selection.provider},
                {"implementation",
                 selection.implementation},
                {"contract", selection.contract},
                {"boundary_fingerprint",
                 selection.boundary_fingerprint},
                {"input_graph_fingerprint",
                 selection.input_graph_fingerprint},
                {"output_graph_fingerprint",
                 selection.output_graph_fingerprint},
                {"provider_owner",
                 selection.provider_owner},
                {"provider_identity",
                 selection.provider_identity},
                {"provider_generation",
                 selection.provider_generation},
                {"provider_version",
                 selection.provider_version},
                {"provider_capability_bits",
                 selection.provider_capability_bits},
                {"transform_index",
                 selection.transform_index},
                {"explicitly_selected",
                 selection.explicitly_selected},
            });
    }
    result["subgraph_replacements"] =
        nlohmann::ordered_json::array();
    for (const auto &selection :
         graph.subgraph_replacements) {
        result["subgraph_replacements"].push_back(
            nlohmann::ordered_json{
                {"region", selection.region},
                {"provider", selection.provider},
                {"implementation",
                 selection.implementation},
                {"contract", selection.contract},
                {"contract_fingerprint",
                 selection.contract_fingerprint},
                {"provider_owner",
                 selection.provider_owner},
                {"provider_identity",
                 selection.provider_identity},
                {"provider_generation",
                 selection.provider_generation},
                {"provider_version",
                 selection.provider_version},
                {"provider_capability_bits",
                 selection.provider_capability_bits},
                {"explicitly_selected",
                 selection.explicitly_selected},
                {"source_nodes",
                 selection.source_nodes},
                {"replacement_nodes",
                 selection.replacement_nodes},
            });
    }
    result["decisions"] = nlohmann::ordered_json::array();
    for (const auto &decision : graph.decisions) {
        result["decisions"].push_back(
            nlohmann::ordered_json{{"code", decision.code},
                                   {"subject", decision.subject},
                                   {"detail", decision.detail}});
    }
    return result;
}

} // namespace Pelican
