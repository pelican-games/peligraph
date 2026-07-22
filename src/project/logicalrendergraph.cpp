#include "logicalrendergraph.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

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

std::string_view logicalAccessModeName(LogicalAccessMode access) {
    switch (access) {
    case LogicalAccessMode::read: return "read";
    case LogicalAccessMode::write: return "write";
    case LogicalAccessMode::read_write: return "read_write";
    }
    throw std::runtime_error("unknown logical access mode");
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
            const auto resource = resources.find(use.resource);
            if (resource == resources.end()) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' references unknown resource '" +
                                         use.resource + "'");
            }
            const auto type_match =
                matchLogicalType(types, resource->second->type, port.accepted_type);
            if (type_match.status == LogicalTypeMatchStatus::rejected) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' port '" + port.name +
                                         "' rejects resource '" + use.resource +
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
            if (!regions.insert(region).second) {
                throw std::runtime_error("logical graph node '" + node.name +
                                         "' has duplicate region tag '" + region + "'");
            }
        }
        for (const auto &dependency : node.after) add_edge(dependency, node.name);
        for (const auto &dependent : node.before) add_edge(node.name, dependent);
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

    for (const auto &decision : graph.decisions) {
        requireName(decision.code, "logical compile decision code");
        requireName(decision.subject, "logical compile decision subject");
    }
}

nlohmann::ordered_json compiledLogicalRenderGraphToJson(
    const CompiledLogicalRenderGraph &graph) {
    nlohmann::ordered_json result;
    result["schema"] = "pelican.logical_render_graph";
    result["version"] = 1;
    result["graph"] = graph.name;
    result["resources"] = nlohmann::ordered_json::array();
    for (const auto &resource : graph.resources) {
        result["resources"].push_back(nlohmann::ordered_json{
            {"name", resource.name},
            {"type", logicalTypeToJson(resource.type)},
            {"materialization",
             logicalMaterializationRequirementName(resource.materialization)}});
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
            encoded["uses"].push_back(nlohmann::ordered_json{
                {"port", use.port},
                {"resource", use.resource},
                {"access", logicalAccessModeName(use.access)},
                {"footprint", std::move(footprint)},
            });
        }
        result["nodes"].push_back(std::move(encoded));
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
