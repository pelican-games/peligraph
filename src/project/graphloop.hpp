#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

// LP-1a: `graph.loop`, the one loop node of the common graph
// (docs/design_graph_loops.md, decisions 1-4). It is a structural node like
// `graph.instance`: the expander handles it, the catalog does not list it, and
// it may stand only in the root document (graph.loop_nested@1 inside a
// definition).
//
//   {"id": "<loop>", "op": "graph.loop",
//    "parameters": {
//      "over":     {"range": {"count": <integer>}},
//      "where":    <condition>,                       optional
//      "order":    {"by": "element", "direction": "ascending" | "descending"},
//      "limit":    <integer>,                         optional
//      "branches": [{"when": <condition>, "body": "<definition>" | null}, ...,
//                   {"body": "<definition>" | null}],  the last is the else
//      "tier":     "expand",                          optional
//      "after": [...], "before": [...],               optional, graph node ids
//      "insert": {"after" | "before": "<pass>"} | "canonical_anchor": "<anchor>"},
//    "inputs":  {"<body graph.input>": {"node", "port"}},
//    "outputs": ["<body output>", ...]}
//
// where a <condition> is {"element": n}, {"element_in": [n, ...]} or
// {"element_not_in": [n, ...]} with integer literals.
//
// The form is checked when a document is expanded (every edit, every load);
// the elements are resolved and the bodies expanded per element after the
// preset and the features are composed (resolveGraphDocumentAfterComposition).

// A `where` / `when` test on one element.
struct GraphLoopCondition {
    enum class Kind : std::uint8_t { always, element, element_in, element_not_in };
    Kind kind = Kind::always;
    std::vector<std::int64_t> values;
    bool matches(std::int64_t element) const;
};

struct GraphLoopBranch {
    GraphLoopCondition when;
    // Empty for `"body": null`: the element runs nothing.
    std::optional<std::string> body;
};

// The set a loop runs over. Only `range` is expanded today; `view_axis`
// (LP-Vb) and `draw_items` (LP-3a) are recognised and refused by name.
enum class GraphLoopSet : std::uint8_t { range, view_axis, draw_items, unknown };
enum class GraphLoopTier : std::uint8_t { expand, draw_loop };
std::string graphLoopTierName(GraphLoopTier tier);

struct GraphLoopPlacement {
    bool after = true;
    // A pass name (insert) or a canonical anchor (canonical_anchor).
    std::string anchor;
};

struct GraphLoopShape {
    std::string node;
    GraphLoopSet set = GraphLoopSet::range;
    // `over.range.count`. LP-1b: a count written as an expression is
    // evaluated at the resolution stage and handed to graphLoopElements.
    std::int64_t range_count = 0;
    // The count is wired to a value that changes per frame
    // (`"count": {"node", "port"}`): refused as per_frame_value.
    bool per_frame_count = false;
    GraphLoopCondition where;
    bool descending = false;
    std::optional<std::int64_t> limit;
    std::vector<GraphLoopBranch> branches;
    std::optional<GraphLoopTier> tier;
    std::vector<std::string> after;
    std::vector<std::string> before;
    std::optional<GraphLoopPlacement> placement;
};

// The bound on range.count that the form accepts. The iteration limits below
// are what the resolution enforces; this only keeps a malformed count from
// making the resolution walk a huge range.
inline constexpr std::int64_t graphLoopRangeCountBound = 65536;
// graph.loop_expansion_limit@1: iterations of one loop, and passes plus
// compute tasks of the whole graph once every loop is expanded.
inline constexpr std::size_t graphLoopIterationLimit = 64;
inline constexpr std::size_t graphLoopGraphNodeLimit = 128;

// Parse and check a graph.loop node's parameters. Shape errors are
// std::invalid_argument (invalid_graph_document); graph.loop_branch_else_missing@1,
// graph.loop_order_unsupported@1, graph.loop_feature_pending@1 and
// graph.insert_anchor_unknown@1 are GraphRegistryError. The tier is not
// decided here (classifyGraphLoopTier).
GraphLoopShape parseGraphLoopShape(const nlohmann::json &node);

// What a branch body holds, for the tier decision.
struct GraphLoopBodyTraits {
    bool draw = false;      // a draw.* node
    bool render = false;    // a render.* or value.* node
    bool nested = false;    // a graph.instance or graph.loop node
    bool graphics = false;  // a render.fullscreen / render.raster node
};

// Section 3: which stage runs the loop. Throws graph.loop_tier_unsupported@1
// {node, reason} or graph.loop_tier_mismatch@1 {node, tier, actual}. LP-1a
// answers `expand` for `range` over render/value bodies and refuses the rest;
// LP-3a adds draw_loop (draw_items over draw.* bodies) here, LP-Vb view_axis.
GraphLoopTier classifyGraphLoopTier(const GraphLoopShape &shape,
                                    const std::vector<GraphLoopBodyTraits> &bodies);

struct GraphLoopElement {
    std::int64_t element = 0;
    // Position after where, order and limit.
    std::size_t index = 0;
    // The branch the element takes (the first whose `when` holds).
    std::size_t branch = 0;
};

// where -> order -> limit over the elements 0..count-1, then each element's
// branch. More than graphLoopIterationLimit elements is
// graph.loop_expansion_limit@1 {node, count, limit}. `count` is the set's
// length: range.count today; LP-1b passes an evaluated count, LP-Vb an axis
// length.
std::vector<GraphLoopElement> graphLoopElements(const GraphLoopShape &shape,
                                                std::int64_t count);

} // namespace Pelican
