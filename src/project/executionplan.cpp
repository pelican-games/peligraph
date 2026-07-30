#include "executionplan.hpp"

#include "stablefingerprint.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

void requireNonEmpty(
    std::string_view value,
    std::string_view subject) {
    if (value.empty()) {
        throw std::runtime_error(
            std::string{subject} +
            " must not be empty");
    }
}

void requireVersionedName(
    std::string_view value,
    std::string_view subject) {
    requireNonEmpty(value, subject);
    try {
        const auto parsed =
            parseSemanticTypeId(value);
        if (semanticTypeIdName(parsed) != value) {
            throw std::runtime_error(
                "name is not canonical");
        }
    } catch (const std::runtime_error &error) {
        throw std::runtime_error(
            std::string{subject} +
            " must use namespace.name@major: " +
            std::string{value} + " (" +
            error.what() + ")");
    }
}

void canonicalizeVersionedNames(
    std::vector<std::string> &values,
    std::string_view subject) {
    for (const auto &value : values) {
        requireVersionedName(value, subject);
    }
    std::sort(values.begin(), values.end());
    values.erase(
        std::unique(values.begin(), values.end()),
        values.end());
}

bool reads(LogicalAccessMode access) {
    return access == LogicalAccessMode::read ||
           access == LogicalAccessMode::read_write;
}

bool writes(LogicalAccessMode access) {
    return access == LogicalAccessMode::write ||
           access == LogicalAccessMode::read_write;
}

bool intentAccepts(
    LogicalAccessIntent intent,
    LogicalAccessMode access) {
    switch (intent) {
    case LogicalAccessIntent::automatic:
    case LogicalAccessIntent::attachment:
    case LogicalAccessIntent::storage:
    case LogicalAccessIntent::host:
        return true;
    case LogicalAccessIntent::sampled:
        return access == LogicalAccessMode::read;
    case LogicalAccessIntent::transfer:
        return access !=
               LogicalAccessMode::read_write;
    }
    return false;
}

void validateFootprint(
    const ExecutionResourceUse &use,
    std::string_view node) {
    (void)executionResourceEpochName(
        use.epoch);
    (void)logicalAccessModeName(
        use.access);
    (void)logicalAccessIntentName(
        use.intent);
    (void)logicalReadFootprintKindName(
        use.footprint.kind);
    const auto is_read = reads(use.access);
    if (is_read &&
        use.footprint.kind ==
            LogicalReadFootprintKind::none) {
        throw std::runtime_error(
            "Execution node '" +
            std::string{node} +
            "' read resource '" + use.resource +
            "' requires a footprint");
    }
    if (!is_read &&
        use.footprint.kind !=
            LogicalReadFootprintKind::none) {
        throw std::runtime_error(
            "Execution node '" +
            std::string{node} +
            "' write resource '" + use.resource +
            "' must not declare a read footprint");
    }
    if (use.footprint.kind ==
        LogicalReadFootprintKind::neighborhood) {
        if (use.footprint.radius &&
            *use.footprint.radius == 0) {
            throw std::runtime_error(
                "Execution node '" +
                std::string{node} +
                "' neighborhood radius must be positive");
        }
    } else if (use.footprint.radius) {
        throw std::runtime_error(
            "Execution node '" +
            std::string{node} +
            "' radius is only valid for neighborhood reads");
    }
    if (use.epoch ==
            ExecutionResourceEpoch::previous &&
        (writes(use.access) ||
         use.footprint.kind !=
             LogicalReadFootprintKind::temporal)) {
        throw std::runtime_error(
            "Execution node '" +
            std::string{node} +
            "' previous-epoch resource '" +
            use.resource +
            "' must be a temporal read");
    }
    if (!intentAccepts(use.intent, use.access)) {
        throw std::runtime_error(
            "Execution node '" +
            std::string{node} +
            "' resource '" + use.resource +
            "' has incompatible access and intent");
    }
}

auto resourceUseKey(
    const ExecutionResourceUse &use) {
    return std::tuple{
        use.resource,
        use.epoch,
        use.access,
        use.intent,
        use.footprint.kind,
        use.footprint.radius,
    };
}

auto effectKey(const ExecutionEffect &effect) {
    return std::tie(effect.id, effect.subject);
}

auto dependencyKey(
    const ExecutionDependency &dependency) {
    return std::tie(
        dependency.from,
        dependency.to,
        dependency.reason,
        dependency.resource);
}

void normalize(FrameExecutionPlan &plan) {
    for (auto &endpoint : plan.endpoints) {
        canonicalizeVersionedNames(
            endpoint.capabilities,
            "execution endpoint capability");
    }
    std::sort(
        plan.endpoints.begin(),
        plan.endpoints.end(),
        [](const auto &lhs, const auto &rhs) {
            return lhs.id < rhs.id;
        });

    for (auto &node : plan.nodes) {
        canonicalizeVersionedNames(
            node.required_capabilities,
            "execution node required capability");
        std::sort(
            node.resource_uses.begin(),
            node.resource_uses.end(),
            [](const auto &lhs, const auto &rhs) {
                return resourceUseKey(lhs) <
                       resourceUseKey(rhs);
            });
        node.resource_uses.erase(
            std::unique(
                node.resource_uses.begin(),
                node.resource_uses.end()),
            node.resource_uses.end());
        std::sort(
            node.effects.begin(),
            node.effects.end(),
            [](const auto &lhs, const auto &rhs) {
                return effectKey(lhs) <
                       effectKey(rhs);
            });
        node.effects.erase(
            std::unique(
                node.effects.begin(),
                node.effects.end()),
            node.effects.end());
        std::sort(
            node.bridge_ids.begin(),
            node.bridge_ids.end());
        node.bridge_ids.erase(
            std::unique(
                node.bridge_ids.begin(),
                node.bridge_ids.end()),
            node.bridge_ids.end());
    }
    std::sort(
        plan.nodes.begin(), plan.nodes.end(),
        [](const auto &lhs, const auto &rhs) {
            return std::tie(lhs.order, lhs.name) <
                   std::tie(rhs.order, rhs.name);
        });

    std::sort(
        plan.dependencies.begin(),
        plan.dependencies.end(),
        [](const auto &lhs, const auto &rhs) {
            return dependencyKey(lhs) <
                   dependencyKey(rhs);
        });
    plan.dependencies.erase(
        std::unique(
            plan.dependencies.begin(),
            plan.dependencies.end()),
        plan.dependencies.end());

    for (auto &bridge : plan.bridges) {
        canonicalizeVersionedNames(
            bridge.required_capabilities,
            "execution bridge required capability");
    }
    std::sort(
        plan.bridges.begin(), plan.bridges.end(),
        [](const auto &lhs, const auto &rhs) {
            return lhs.id < rhs.id;
        });
}

void validateStructure(
    const FrameExecutionPlan &plan) {
    if (plan.schema_version != 1) {
        throw std::runtime_error(
            "Frame execution plan has unsupported schema_version");
    }
    requireNonEmpty(plan.graph,
                    "frame execution graph");

    std::map<std::string, const ExecutionEndpoint *,
             std::less<>>
        endpoints;
    for (const auto &endpoint : plan.endpoints) {
        requireNonEmpty(
            endpoint.id,
            "execution endpoint id");
        requireNonEmpty(
            endpoint.backend,
            "execution endpoint backend");
        (void)executionEndpointClassName(
            endpoint.endpoint_class);
        if (!endpoints.emplace(
                 endpoint.id, &endpoint)
                 .second) {
            throw std::runtime_error(
                "Frame execution plan has duplicate endpoint: " +
                endpoint.id);
        }
    }

    std::map<std::string, const FrameExecutionNode *,
             std::less<>>
        nodes;
    for (std::size_t index = 0;
         index < plan.nodes.size(); ++index) {
        const auto &node = plan.nodes[index];
        requireNonEmpty(
            node.name,
            "execution node name");
        requireVersionedName(
            node.semantic_dialect,
            "execution node semantic dialect");
        requireVersionedName(
            node.selected_implementation,
            "execution node selected implementation");
        requireNonEmpty(
            node.selected_endpoint,
            "execution node selected endpoint");
        requireNonEmpty(
            node.view_family,
            "execution node view family");
        if (node.order != index) {
            throw std::runtime_error(
                "Frame execution node order is not contiguous: " +
                node.name);
        }
        if (!nodes.emplace(node.name, &node).second) {
            throw std::runtime_error(
                "Frame execution plan has duplicate node: " +
                node.name);
        }
        const auto endpoint =
            endpoints.find(node.selected_endpoint);
        if (endpoint == endpoints.end()) {
            throw std::runtime_error(
                "Execution node '" + node.name +
                "' selects unknown endpoint '" +
                node.selected_endpoint + "'");
        }
        for (const auto &capability :
             node.required_capabilities) {
            if (!std::binary_search(
                    endpoint->second->capabilities.begin(),
                    endpoint->second->capabilities.end(),
                    capability)) {
                throw std::runtime_error(
                    "Execution endpoint '" +
                    endpoint->first +
                    "' does not provide capability '" +
                    capability +
                    "' required by node '" +
                    node.name + "'");
            }
        }

        std::set<
            std::pair<std::string,
                      ExecutionResourceEpoch>>
            resources;
        for (const auto &use :
             node.resource_uses) {
            requireNonEmpty(
                use.resource,
                "execution resource name");
            if (!resources.emplace(
                     use.resource, use.epoch)
                     .second) {
                throw std::runtime_error(
                    "Execution node '" +
                    node.name +
                    "' has duplicate resource epoch use: " +
                    use.resource);
            }
            validateFootprint(use, node.name);
        }
        for (const auto &effect : node.effects) {
            requireVersionedName(
                effect.id,
                "execution effect id");
        }
    }
    if (!plan.nodes.empty() &&
        plan.endpoints.empty()) {
        throw std::runtime_error(
            "Frame execution plan with nodes requires an endpoint");
    }

    std::map<std::string,
             const ExecutionBridgeObligation *,
             std::less<>>
        bridges;
    for (const auto &bridge : plan.bridges) {
        requireNonEmpty(
            bridge.id,
            "execution bridge id");
        if (!bridges.emplace(
                 bridge.id, &bridge)
                 .second) {
            throw std::runtime_error(
                "Frame execution plan has duplicate bridge: " +
                bridge.id);
        }
        if (bridge.source_endpoint ==
            bridge.destination_endpoint) {
            throw std::runtime_error(
                "Execution bridge endpoints must differ: " +
                bridge.id);
        }
        if (!endpoints.contains(
                bridge.source_endpoint) ||
            !endpoints.contains(
                bridge.destination_endpoint)) {
            throw std::runtime_error(
                "Execution bridge references an unknown endpoint: " +
                bridge.id);
        }
    }
    for (const auto &node : plan.nodes) {
        for (const auto &bridge_id :
             node.bridge_ids) {
            if (!bridges.contains(bridge_id)) {
                throw std::runtime_error(
                    "Execution node '" + node.name +
                    "' references unknown bridge '" +
                    bridge_id + "'");
            }
        }
    }

    for (const auto &dependency :
         plan.dependencies) {
        requireVersionedName(
            dependency.reason,
            "execution dependency reason");
        const auto from =
            nodes.find(dependency.from);
        const auto to =
            nodes.find(dependency.to);
        if (from == nodes.end() ||
            to == nodes.end()) {
            throw std::runtime_error(
                "Execution dependency references an unknown node: " +
                dependency.from + " -> " +
                dependency.to);
        }
        if (from->second->order >=
            to->second->order) {
            throw std::runtime_error(
                "Execution dependency source does not precede target: " +
                dependency.from + " -> " +
                dependency.to);
        }
        if (from->second->selected_endpoint !=
            to->second->selected_endpoint) {
            const auto bridge_exists =
                std::any_of(
                    plan.bridges.begin(),
                    plan.bridges.end(),
                    [&](const auto &bridge) {
                        return bridge.source_endpoint ==
                                   from->second
                                       ->selected_endpoint &&
                               bridge.destination_endpoint ==
                                   to->second
                                       ->selected_endpoint;
                    });
            if (!bridge_exists) {
                throw std::runtime_error(
                    "Cross-endpoint execution dependency lacks a bridge: " +
                    dependency.from + " -> " +
                    dependency.to);
            }
        }
    }
}

void appendStringVector(
    StableFingerprint64 &fingerprint,
    const std::vector<std::string> &values) {
    fingerprint.appendUnsigned(values.size());
    for (const auto &value : values) {
        fingerprint.appendString(value);
    }
}

std::uint64_t computeFingerprint(
    const FrameExecutionPlan &plan) {
    StableFingerprint64 fingerprint;
    fingerprint.appendString(
        "pelican.frame_execution_plan");
    fingerprint.appendUnsigned(
        plan.schema_version);
    fingerprint.appendString(plan.graph);
    fingerprint.appendUnsigned(
        plan.endpoints.size());
    for (const auto &endpoint : plan.endpoints) {
        fingerprint.appendString(endpoint.id);
        fingerprint.appendUnsigned(
            static_cast<std::uint8_t>(
                endpoint.endpoint_class));
        fingerprint.appendString(
            endpoint.backend);
        appendStringVector(
            fingerprint,
            endpoint.capabilities);
    }
    fingerprint.appendUnsigned(plan.nodes.size());
    for (const auto &node : plan.nodes) {
        fingerprint.appendString(node.name);
        fingerprint.appendUnsigned(
            node.declaration_index);
        fingerprint.appendUnsigned(node.order);
        fingerprint.appendUnsigned(node.level);
        fingerprint.appendString(
            node.semantic_dialect);
        fingerprint.appendString(
            node.selected_implementation);
        fingerprint.appendString(
            node.selected_endpoint);
        appendStringVector(
            fingerprint,
            node.required_capabilities);
        fingerprint.appendUnsigned(
            node.resource_uses.size());
        for (const auto &use :
             node.resource_uses) {
            fingerprint.appendString(
                use.resource);
            fingerprint.appendUnsigned(
                static_cast<std::uint8_t>(
                    use.epoch));
            fingerprint.appendUnsigned(
                static_cast<std::uint8_t>(
                    use.access));
            fingerprint.appendUnsigned(
                static_cast<std::uint8_t>(
                    use.intent));
            fingerprint.appendUnsigned(
                static_cast<std::uint8_t>(
                    use.footprint.kind));
            fingerprint.appendUnsigned(
                use.footprint.radius.has_value());
            if (use.footprint.radius) {
                fingerprint.appendUnsigned(
                    *use.footprint.radius);
            }
        }
        fingerprint.appendUnsigned(
            node.effects.size());
        for (const auto &effect : node.effects) {
            fingerprint.appendString(effect.id);
            fingerprint.appendString(
                effect.subject);
        }
        appendStringVector(
            fingerprint, node.bridge_ids);
        fingerprint.appendString(
            node.view_family);
    }
    fingerprint.appendUnsigned(
        plan.dependencies.size());
    for (const auto &dependency :
         plan.dependencies) {
        fingerprint.appendString(dependency.from);
        fingerprint.appendString(dependency.to);
        fingerprint.appendString(dependency.reason);
        fingerprint.appendString(
            dependency.resource);
    }
    fingerprint.appendUnsigned(
        plan.bridges.size());
    for (const auto &bridge : plan.bridges) {
        fingerprint.appendString(bridge.id);
        fingerprint.appendString(
            bridge.source_endpoint);
        fingerprint.appendString(
            bridge.destination_endpoint);
        appendStringVector(
            fingerprint,
            bridge.required_capabilities);
    }
    return fingerprint.value();
}

nlohmann::ordered_json footprintToJson(
    const LogicalReadFootprint &footprint) {
    nlohmann::ordered_json result{
        {"kind",
         logicalReadFootprintKindName(
             footprint.kind)},
    };
    if (footprint.radius) {
        result["radius"] = *footprint.radius;
    }
    return result;
}

} // namespace

std::string_view executionEndpointClassName(
    ExecutionEndpointClass endpoint_class) {
    switch (endpoint_class) {
    case ExecutionEndpointClass::host:
        return "host";
    case ExecutionEndpointClass::device:
        return "device";
    case ExecutionEndpointClass::external:
        return "external";
    }
    throw std::runtime_error(
        "unknown execution endpoint class");
}

std::string_view executionResourceEpochName(
    ExecutionResourceEpoch epoch) {
    switch (epoch) {
    case ExecutionResourceEpoch::current:
        return "current";
    case ExecutionResourceEpoch::previous:
        return "previous";
    }
    throw std::runtime_error(
        "unknown execution resource epoch");
}

FrameExecutionPlan canonicalizeFrameExecutionPlan(
    FrameExecutionPlan plan) {
    plan.fingerprint = 0;
    normalize(plan);
    validateStructure(plan);
    plan.fingerprint =
        computeFingerprint(plan);
    return plan;
}

void validateFrameExecutionPlan(
    const FrameExecutionPlan &plan) {
    auto canonical =
        canonicalizeFrameExecutionPlan(plan);
    if (canonical.fingerprint !=
        plan.fingerprint) {
        throw std::runtime_error(
            "Frame execution plan fingerprint mismatch for graph: " +
            plan.graph);
    }
    if (canonical != plan) {
        throw std::runtime_error(
            "Frame execution plan is not canonical for graph: " +
            plan.graph);
    }
}

std::uint64_t frameExecutionPlanFingerprint(
    const FrameExecutionPlan &plan) {
    return canonicalizeFrameExecutionPlan(plan)
        .fingerprint;
}

nlohmann::ordered_json frameExecutionPlanToJson(
    const FrameExecutionPlan &plan) {
    validateFrameExecutionPlan(plan);
    nlohmann::ordered_json result{
        {"schema", "pelican.frame_execution_plan"},
        {"schema_version", plan.schema_version},
        {"graph", plan.graph},
        {"fingerprint",
         stableFingerprint64String(
             plan.fingerprint)},
        {"endpoints",
         nlohmann::ordered_json::array()},
        {"nodes",
         nlohmann::ordered_json::array()},
        {"dependencies",
         nlohmann::ordered_json::array()},
        {"bridges",
         nlohmann::ordered_json::array()},
    };
    for (const auto &endpoint : plan.endpoints) {
        result["endpoints"].push_back(
            nlohmann::ordered_json{
                {"id", endpoint.id},
                {"class",
                 executionEndpointClassName(
                     endpoint.endpoint_class)},
                {"backend", endpoint.backend},
                {"capabilities",
                 endpoint.capabilities},
            });
    }
    for (const auto &node : plan.nodes) {
        nlohmann::ordered_json encoded{
            {"name", node.name},
            {"declaration_index",
             node.declaration_index},
            {"order", node.order},
            {"level", node.level},
            {"semantic_dialect",
             node.semantic_dialect},
            {"selected_implementation",
             node.selected_implementation},
            {"selected_endpoint",
             node.selected_endpoint},
            {"required_capabilities",
             node.required_capabilities},
            {"resource_uses",
             nlohmann::ordered_json::array()},
            {"effects",
             nlohmann::ordered_json::array()},
            {"bridge_ids", node.bridge_ids},
            {"view_family", node.view_family},
        };
        for (const auto &use :
             node.resource_uses) {
            encoded["resource_uses"].push_back(
                nlohmann::ordered_json{
                    {"resource", use.resource},
                    {"epoch",
                     executionResourceEpochName(
                         use.epoch)},
                    {"access",
                     logicalAccessModeName(
                         use.access)},
                    {"intent",
                     logicalAccessIntentName(
                         use.intent)},
                    {"footprint",
                     footprintToJson(
                         use.footprint)},
                });
        }
        for (const auto &effect : node.effects) {
            nlohmann::ordered_json encoded_effect{
                {"id", effect.id},
            };
            if (!effect.subject.empty()) {
                encoded_effect["subject"] =
                    effect.subject;
            }
            encoded["effects"].push_back(
                std::move(encoded_effect));
        }
        result["nodes"].push_back(
            std::move(encoded));
    }
    for (const auto &dependency :
         plan.dependencies) {
        nlohmann::ordered_json encoded{
            {"from", dependency.from},
            {"to", dependency.to},
            {"reason", dependency.reason},
        };
        if (!dependency.resource.empty()) {
            encoded["resource"] =
                dependency.resource;
        }
        result["dependencies"].push_back(
            std::move(encoded));
    }
    for (const auto &bridge : plan.bridges) {
        result["bridges"].push_back(
            nlohmann::ordered_json{
                {"id", bridge.id},
                {"source_endpoint",
                 bridge.source_endpoint},
                {"destination_endpoint",
                 bridge.destination_endpoint},
                {"required_capabilities",
                 bridge.required_capabilities},
            });
    }
    return result;
}

} // namespace Pelican
