#pragma once

#include "renderingpass.hpp"
#include "../../project/logicalrendergraph.hpp"
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

enum class FramePlanNodeKind {
    render,
    compute,
    anchor,
    snapshot_copy,
    output_transform,
};

enum class FrameGraphAttachmentAspect {
    color,
    depth,
};

enum class FrameGraphAttachmentLoadOp {
    load,
    clear,
    discard,
};

enum class FrameGraphAttachmentStoreOp {
    store,
    discard,
};

struct FrameGraphAttachmentDefinition {
    std::string resource;
    FrameGraphAttachmentAspect aspect =
        FrameGraphAttachmentAspect::color;
    FrameGraphAttachmentLoadOp load_op =
        FrameGraphAttachmentLoadOp::clear;
    FrameGraphAttachmentStoreOp store_op =
        FrameGraphAttachmentStoreOp::store;

    bool operator==(
        const FrameGraphAttachmentDefinition &) const = default;
};

struct FrameGraphNodeDefinition {
    std::string name;
    FramePlanNodeKind kind = FramePlanNodeKind::render;
    size_t declaration_index = 0;
    std::vector<std::string> reads;
    std::vector<std::string> reads_history;
    std::vector<std::string> writes;
    std::vector<std::string> after;
    std::vector<std::string> before;
    std::vector<std::string> region_tags;
    std::vector<FrameGraphAttachmentDefinition>
        attachments;
    std::string snapshot_after;
    std::size_t byte_size = 0;
    bool raster_geometry = false;
    RenderResolutionDomain resolution_domain =
        RenderResolutionDomain::unclassified;
};

struct FrameGraphDefinition {
    std::string name;
    std::vector<std::string> declared_resources;
    std::vector<std::string> history_resources;
    std::vector<FrameGraphNodeDefinition> nodes;
    std::vector<LogicalGraphTransformSelection>
        graph_transforms;
    std::vector<LogicalSubgraphReplacementSelection>
        subgraph_replacements;
    std::optional<RenderStrategySelection>
        render_strategy;
};

struct FramePlanNode {
    std::string name;
    FramePlanNodeKind kind = FramePlanNodeKind::render;
    size_t declaration_index = 0;
    size_t order = 0;
    size_t level = 0;
    std::vector<std::string> reads;
    std::vector<std::string> reads_history;
    std::vector<std::string> writes;
    std::string snapshot_after;
    std::size_t byte_size = 0;
};

struct FramePlanBarrier {
    std::string kind;
    std::string resource;
    std::string from;
    std::string to;
};

struct FramePlan {
    std::string name;
    std::vector<FramePlanNode> nodes;
    std::vector<std::vector<std::string>> levels;
    std::vector<FramePlanBarrier> barriers;
};

std::string framePlanNodeKindName(FramePlanNodeKind kind);

FrameGraphDefinition makeFrameGraphDefinition(const RenderingPassDefinition &definition);
FrameGraphDefinition parseFrameGraphDefinitionFromJson(const nlohmann::json &graph_json);
std::vector<FrameGraphDefinition> parseFrameGraphDefinitionsFromConfigJson(const nlohmann::json &config_json);

FramePlan planFrameGraph(const FrameGraphDefinition &definition);
std::vector<std::string> framePlanOrder(const FramePlan &plan);
nlohmann::json framePlanToJson(
    const FramePlan &plan,
    const CompiledRenderPipeline *render_pipeline = nullptr);

} // namespace Pelican
