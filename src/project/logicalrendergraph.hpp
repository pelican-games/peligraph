#pragma once

#include "logicalrendertype.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::size_t maximumLogicalRegionTagBytes = 255;

enum class LogicalPortDirection : std::uint8_t {
    input,
    output,
    input_output,
};

std::string_view logicalPortDirectionName(LogicalPortDirection direction);

enum class LogicalPortRelationKind : std::uint8_t {
    same_extent,
    extent_scale,
    same_view_set,
    same_samples,
    per_view,
};

std::string_view logicalPortRelationKindName(LogicalPortRelationKind kind);

struct LogicalPortRelation {
    LogicalPortRelationKind kind = LogicalPortRelationKind::same_extent;
    std::string other_port;
    Rational scale_x{1, 1};
    Rational scale_y{1, 1};
};

struct LogicalPortContract {
    std::string name;
    LogicalPortDirection direction = LogicalPortDirection::input;
    LogicalTypePattern accepted_type;
    std::vector<LogicalPortRelation> relations;
};

enum class LogicalMaterializationRequirement : std::uint8_t {
    virtual_resource,
    preferred,
    required,
    external,
};

std::string_view logicalMaterializationRequirementName(
    LogicalMaterializationRequirement requirement);

struct LogicalResourceDesc {
    std::string name;
    LogicalType type;
    LogicalMaterializationRequirement materialization =
        LogicalMaterializationRequirement::virtual_resource;
};

struct LogicalValueId {
    std::string resource;
    std::uint32_t version = 0;

    bool operator==(const LogicalValueId &) const = default;
    bool operator<(const LogicalValueId &other) const {
        if (resource != other.resource) return resource < other.resource;
        return version < other.version;
    }
};

std::string logicalValueIdName(const LogicalValueId &value);

enum class LogicalValueImportKind : std::uint8_t {
    graph_input,
    previous_epoch,
    external,
    legacy_implicit,
};

std::string_view logicalValueImportKindName(LogicalValueImportKind kind);

struct LogicalValueImport {
    LogicalValueId value;
    LogicalValueImportKind kind = LogicalValueImportKind::graph_input;

    bool operator==(const LogicalValueImport &) const = default;
};

enum class LogicalAccessMode : std::uint8_t {
    read,
    write,
    read_write,
};

std::string_view logicalAccessModeName(LogicalAccessMode access);

enum class LogicalAccessIntent : std::uint8_t {
    automatic,
    sampled,
    attachment,
    storage,
    transfer,
    host,
};

std::string_view logicalAccessIntentName(LogicalAccessIntent intent);

enum class LogicalReadFootprintKind : std::uint8_t {
    none,
    same_pixel,
    neighborhood,
    arbitrary,
    temporal,
};

std::string_view logicalReadFootprintKindName(LogicalReadFootprintKind kind);

struct LogicalReadFootprint {
    LogicalReadFootprintKind kind = LogicalReadFootprintKind::none;
    std::optional<std::uint32_t> radius;

    bool operator==(const LogicalReadFootprint &) const = default;
};

struct LogicalResourceUse {
    std::string port;
    LogicalAccessMode access = LogicalAccessMode::read;
    LogicalReadFootprint footprint;
    LogicalAccessIntent intent = LogicalAccessIntent::automatic;
    std::optional<LogicalValueId> input_value;
    std::optional<LogicalValueId> output_value;
};

LogicalResourceUse makeLogicalReadUse(
    std::string port, LogicalValueId input,
    LogicalReadFootprint footprint,
    LogicalAccessIntent intent = LogicalAccessIntent::automatic);
LogicalResourceUse makeLogicalWriteUse(
    std::string port, LogicalValueId output,
    LogicalAccessIntent intent = LogicalAccessIntent::automatic);
LogicalResourceUse makeLogicalReadWriteUse(
    std::string port, LogicalValueId input, LogicalValueId output,
    LogicalReadFootprint footprint,
    LogicalAccessIntent intent = LogicalAccessIntent::automatic);

enum class LogicalGraphNodeKind : std::uint8_t {
    render,
    compute,
    anchor,
    snapshot_copy,
    output_transform,
};

std::string_view logicalGraphNodeKindName(LogicalGraphNodeKind kind);

struct LogicalGraphNode {
    std::string name;
    LogicalGraphNodeKind kind = LogicalGraphNodeKind::render;
    std::size_t declaration_index = 0;
    std::vector<LogicalPortContract> ports;
    std::vector<LogicalResourceUse> uses;
    std::vector<std::string> after;
    std::vector<std::string> before;
    std::vector<std::string> region_tags;
};

struct LogicalCompileDecision {
    std::string code;
    std::string subject;
    std::string detail;
};

struct LogicalGraphTransformSelection {
    std::string name;
    std::string provider;
    std::string implementation;
    std::string contract;
    std::uint64_t boundary_fingerprint = 0;
    std::uint64_t input_graph_fingerprint = 0;
    std::uint64_t output_graph_fingerprint = 0;
    std::uint64_t provider_owner = 0;
    std::uint64_t provider_identity = 0;
    std::uint32_t provider_generation = 0;
    std::uint32_t provider_version = 0;
    std::uint64_t provider_capability_bits = 0;
    std::uint32_t transform_index = 0;
    bool explicitly_selected = false;

    bool operator==(
        const LogicalGraphTransformSelection &) const = default;
};

struct RenderStrategySelection {
    std::string name;
    std::string provider;
    std::string implementation;
    std::string contract;
    std::string output_contract;
    std::string graph_variant;
    std::uint64_t facade_capability_bits = 0;
    std::uint64_t input_config_fingerprint = 0;
    std::uint64_t output_config_fingerprint = 0;
    std::uint64_t provider_owner = 0;
    std::uint64_t provider_identity = 0;
    std::uint32_t provider_generation = 0;
    std::uint32_t provider_version = 0;
    std::uint64_t provider_capability_bits = 0;
    bool explicitly_selected = false;

    bool operator==(
        const RenderStrategySelection &) const = default;
};

struct LogicalSubgraphReplacementSelection {
    std::string region;
    std::string provider;
    std::string implementation;
    std::string contract;
    std::uint64_t contract_fingerprint = 0;
    std::uint64_t provider_owner = 0;
    std::uint64_t provider_identity = 0;
    std::uint32_t provider_generation = 0;
    std::uint32_t provider_version = 0;
    std::uint64_t provider_capability_bits = 0;
    bool explicitly_selected = false;
    std::vector<std::string> source_nodes;
    std::vector<std::string> replacement_nodes;

    bool operator==(
        const LogicalSubgraphReplacementSelection &) const = default;
};

struct LogicalDataEdge {
    LogicalValueId value;
    std::string producer_node;
    std::string producer_port;
    std::string consumer_node;
    std::string consumer_port;

    bool operator==(const LogicalDataEdge &) const = default;
};

struct CompiledLogicalRenderGraph {
    std::string name;
    std::vector<LogicalResourceDesc> resources;
    std::vector<LogicalValueImport> imports;
    std::vector<LogicalGraphNode> nodes;
    std::vector<LogicalGraphTransformSelection>
        graph_transforms;
    std::vector<LogicalSubgraphReplacementSelection>
        subgraph_replacements;
    std::vector<LogicalCompileDecision> decisions;
    std::optional<RenderStrategySelection>
        render_strategy;
};

std::vector<LogicalDataEdge> deriveLogicalDataEdges(
    const CompiledLogicalRenderGraph &graph);
void validateCompiledLogicalRenderGraph(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &graph);
nlohmann::ordered_json compiledLogicalRenderGraphToJson(
    const CompiledLogicalRenderGraph &graph);

} // namespace Pelican
