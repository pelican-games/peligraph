#pragma once

#include "logicalrendergraph.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// Endpoint class is intentionally coarse. Queue families, Vulkan queue
// capabilities, and backend-native scheduling remain open capability data.
enum class ExecutionEndpointClass : std::uint8_t {
    host,
    device,
    external,
};

std::string_view executionEndpointClassName(
    ExecutionEndpointClass endpoint_class);

enum class ExecutionResourceEpoch : std::uint8_t {
    current,
    previous,
};

std::string_view executionResourceEpochName(
    ExecutionResourceEpoch epoch);

struct ExecutionEndpoint {
    std::string id;
    ExecutionEndpointClass endpoint_class =
        ExecutionEndpointClass::device;
    // Open backend identity. "vulkan" is used today; a future Metal or host
    // endpoint does not require extending a central backend enum.
    std::string backend;
    std::vector<std::string> capabilities;

    bool operator==(const ExecutionEndpoint &) const = default;
};

struct ExecutionResourceUse {
    std::string resource;
    ExecutionResourceEpoch epoch =
        ExecutionResourceEpoch::current;
    LogicalAccessMode access = LogicalAccessMode::read;
    LogicalAccessIntent intent =
        LogicalAccessIntent::automatic;
    LogicalReadFootprint footprint;

    bool operator==(const ExecutionResourceUse &) const = default;
};

struct ExecutionEffect {
    std::string id;
    std::string subject;

    bool operator==(const ExecutionEffect &) const = default;
};

// This is a backend-independent execution-dialect node, not a universal
// backend payload. Native Vulkan/Metal commands live in sibling physical
// packages and are linked to this plan through endpoint and implementation
// identities.
struct FrameExecutionNode {
    std::string name;
    std::size_t declaration_index = 0;
    std::size_t order = 0;
    std::size_t level = 0;
    std::string semantic_dialect;
    std::string selected_implementation;
    std::string selected_endpoint;
    std::vector<std::string> required_capabilities;
    std::vector<ExecutionResourceUse> resource_uses;
    std::vector<ExecutionEffect> effects;
    std::vector<std::string> bridge_ids;
    std::string view_family;

    bool operator==(const FrameExecutionNode &) const = default;
};

struct ExecutionDependency {
    std::string from;
    std::string to;
    std::string reason;
    // Empty for an ordering-only dependency.
    std::string resource;

    bool operator==(const ExecutionDependency &) const = default;
};

struct ExecutionBridgeObligation {
    std::string id;
    std::string source_endpoint;
    std::string destination_endpoint;
    std::vector<std::string> required_capabilities;

    bool operator==(
        const ExecutionBridgeObligation &) const = default;
};

struct FrameExecutionPlan {
    std::uint32_t schema_version = 1;
    std::string graph;
    std::vector<ExecutionEndpoint> endpoints;
    std::vector<FrameExecutionNode> nodes;
    std::vector<ExecutionDependency> dependencies;
    std::vector<ExecutionBridgeObligation> bridges;
    std::uint64_t fingerprint = 0;

    bool operator==(const FrameExecutionPlan &) const = default;
};

// Canonicalization sorts open ID sets and structural tables before stamping
// the stable provenance fingerprint. It also validates endpoint/dependency
// closure, but deliberately permits disconnected node components.
FrameExecutionPlan canonicalizeFrameExecutionPlan(
    FrameExecutionPlan plan);
void validateFrameExecutionPlan(
    const FrameExecutionPlan &plan);
std::uint64_t frameExecutionPlanFingerprint(
    const FrameExecutionPlan &plan);
nlohmann::ordered_json frameExecutionPlanToJson(
    const FrameExecutionPlan &plan);

} // namespace Pelican
