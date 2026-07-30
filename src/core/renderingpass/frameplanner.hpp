#pragma once

#include "renderingpass.hpp"
#include "../../project/logicalrendergraph.hpp"
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
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
    std::optional<ImageSubresourceRange> subresource;
    FrameGraphAttachmentAspect aspect =
        FrameGraphAttachmentAspect::color;
    FrameGraphAttachmentLoadOp load_op =
        FrameGraphAttachmentLoadOp::clear;
    FrameGraphAttachmentStoreOp store_op =
        FrameGraphAttachmentStoreOp::store;

    bool operator==(
        const FrameGraphAttachmentDefinition &) const = default;
};

// A shader-visible read can state how far from the current invocation it
// accesses an image. The resource list remains separate so the legacy frame
// scheduler does not depend on target-planning semantics; this typed contract
// is consumed by the logical shadow graph and physical target compiler.
struct FrameGraphReadFootprintDefinition {
    std::string resource;
    LogicalReadFootprint footprint{
        LogicalReadFootprintKind::arbitrary,
        std::nullopt,
    };

    bool operator==(
        const FrameGraphReadFootprintDefinition &) const =
        default;
};

struct FrameGraphResourceAccessDefinition {
    // History reads retain the authored @history suffix so a current and
    // previous-epoch view of the same resource can carry distinct ports.
    std::string resource;
    LogicalAccessIntent intent =
        LogicalAccessIntent::automatic;

    bool operator==(
        const FrameGraphResourceAccessDefinition &) const =
        default;
};

struct FrameGraphNodeDefinition {
    std::string name;
    FramePlanNodeKind kind = FramePlanNodeKind::render;
    size_t declaration_index = 0;
    std::vector<std::string> reads;
    std::vector<std::string> reads_history;
    std::vector<FrameGraphReadFootprintDefinition>
        read_footprints;
    // Resource reads whose concrete pass implementation can switch between
    // a sampled descriptor and an input attachment. This prevents a future
    // same-pixel read on shadow/velocity/feature-owned inputs from being
    // fused before its shader ABI exists.
    std::vector<std::string> local_read_shader_inputs;
    std::vector<FrameGraphResourceAccessDefinition>
        resource_accesses;
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
    std::optional<MaterialDrawTagFilter> material_filter;
    std::optional<std::string> material_variant;
    std::string view_family{
        mainRenderViewFamilyId};
    // Optional dialect selection carried into FrameExecutionPlan. Empty
    // preserves the legacy kind-based compatibility mapping.
    std::string semantic_dialect;
    std::string execution_implementation;
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
    std::optional<MaterialDrawTagFilter> material_filter;
    std::optional<std::string> material_variant;
    std::string view_family{
        mainRenderViewFamilyId};
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
void applyMaterialDrawFilterResolutionToFramePlanJson(
    nlohmann::json &plan_json,
    std::string_view pass_name,
    MaterialDrawTagFilterId filter_id,
    std::size_t resolved_draw_count,
    const std::vector<std::string> &unmatched_include,
    const std::vector<std::string> &unmatched_exclude);

} // namespace Pelican
