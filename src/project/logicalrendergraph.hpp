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

enum class LogicalAccessMode : std::uint8_t {
    read,
    write,
    read_write,
};

std::string_view logicalAccessModeName(LogicalAccessMode access);

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
};

struct LogicalResourceUse {
    std::string port;
    std::string resource;
    LogicalAccessMode access = LogicalAccessMode::read;
    LogicalReadFootprint footprint;
};

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

struct CompiledLogicalRenderGraph {
    std::string name;
    std::vector<LogicalResourceDesc> resources;
    std::vector<LogicalGraphNode> nodes;
    std::vector<LogicalCompileDecision> decisions;
};

void validateCompiledLogicalRenderGraph(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &graph);
nlohmann::ordered_json compiledLogicalRenderGraphToJson(
    const CompiledLogicalRenderGraph &graph);

} // namespace Pelican
