#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
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
//      "over":     {"range": {"count": <integer> | {"$expr": ...}}},
//      "where":    <condition>,                       optional
//      "order":    {"by": "element", "direction": "ascending" | "descending"},
//      "limit":    <integer>,                         optional
//      "branches": [{"when": <condition>, "body": "<definition>" | null}, ...,
//                   {"body": "<definition>" | null}],  the last is the else
//      "carry":    [{"from": "<body output>", "to": "<body graph.input>"}, ...],
//      "tier":     "expand",                          optional
//      "after": [...], "before": [...],               optional, graph node ids
//      "insert": {"after" | "before": "<pass>"} | "canonical_anchor": "<anchor>"},
//    "inputs":  {"<body graph.input>": {"node", "port"[, "element": {"$expr": ...}]}},
//    "outputs": ["<body output>", "<body output>[]", ...]}
//
// where a <condition> is {"element": v}, {"element_in": [v, ...]} or
// {"element_not_in": [v, ...]} and each v is an integer literal or, LP-1b, an
// integer expression (graphloopexpression.hpp): in `where` it may read
// `element`, in `when` also `index` and `count`; the count may read neither.
//
// LP-1b: `carry` hands a body output to the next element's body input: the
// first element reads the loop's input edge, every later one the previous
// element's output (an element with no body passes the value on). `p[]` is
// the output of every element; another node reads one of them with an edge
// that names the element, `{"node": "<loop>", "port": "p[]", "element":
// {"$expr": ...}}`, whose `element` variable is the receiving loop's element
// (a receiver that is no loop reads a constant element).
//
// The form is checked when a document is expanded (every edit, every load);
// the elements are resolved and the bodies expanded per element after the
// preset and the features are composed (resolveGraphDocumentAfterComposition).

// A `where` / `when` test on one element.
struct GraphLoopCondition {
    enum class Kind : std::uint8_t { always, element, element_in, element_not_in };
    Kind kind = Kind::always;
    // Each an integer literal or an expression object (LP-1b).
    std::vector<nlohmann::json> values;
    // The document pointer of each value, for graph.loop_expression_invalid@1.
    std::vector<std::string> pointers;
};

// LP-3a: a draw_loop's `where` and a branch's `when` look at one material,
// never at an element number. `where` takes only `tags`; `when` any of these,
// every key it writes must hold (AND):
//   "material":    the matching key (<document>#<name>, <asset>#material[i],
//                  or the registered name; materialdrawloop.hpp)
//   "tags":        {"include": [...], "exclude": [...], "require_all": [...]}
//                  (include: any of them; exclude: none of them; require_all:
//                  every one of them)
//   "contract":    the material's shader contract (gbuffer_v1,
//                  forward_scene_color_v1)
//   "skinned":     true | false
//   "has_variant": true | false (any variant), or a variant name
struct GraphLoopTagFilter {
    // Each sorted and unique.
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::vector<std::string> require_all;
    bool empty() const noexcept { return include.empty() && exclude.empty() && require_all.empty(); }
    bool operator==(const GraphLoopTagFilter &) const = default;
};

struct GraphLoopDrawCondition {
    std::optional<std::string> material;
    std::optional<GraphLoopTagFilter> tags;
    std::optional<std::string> contract;
    std::optional<bool> skinned;
    // true / false: has any variant / has none; a string: has that variant.
    std::optional<bool> has_variant;
    std::optional<std::string> has_variant_named;
    bool operator==(const GraphLoopDrawCondition &) const = default;
};

struct GraphLoopBranch {
    GraphLoopCondition when;
    // LP-3a: the `when` of a draw_loop branch (`when` above stays `always`).
    GraphLoopDrawCondition draw_when;
    // Empty for `"body": null`: the element runs nothing.
    std::optional<std::string> body;
};

// The set a loop runs over. `range` is expanded; `draw_items` (LP-3a) runs
// inside one material pass; `view_axis` (LP-Vb) is recognised and refused by
// name.
enum class GraphLoopSet : std::uint8_t { range, view_axis, draw_items, unknown };
enum class GraphLoopTier : std::uint8_t { expand, draw_loop };
std::string graphLoopTierName(GraphLoopTier tier);

struct GraphLoopPlacement {
    bool after = true;
    // A pass name (insert) or a canonical anchor (canonical_anchor).
    std::string anchor;
};

// LP-1b: one `carry` entry: the body output `from` of an element is the
// body input `to` of the next.
struct GraphLoopCarry {
    std::string from;
    std::string to;
};

struct GraphLoopShape {
    std::string node;
    // The loop node's JSON pointer in the authoring document (`/nodes/<i>`);
    // expression refusals point below it.
    std::string pointer;
    GraphLoopSet set = GraphLoopSet::range;
    // `over.range.count`, when it is a literal.
    std::int64_t range_count = 0;
    // LP-1b: `over.range.count` written as an expression, evaluated at the
    // resolution stage (graphLoopCount) with the composed targets' constants.
    std::optional<nlohmann::json> range_count_expression;
    // The count is wired to a value that changes per frame
    // (`"count": {"node", "port"}`): refused as per_frame_value.
    bool per_frame_count = false;
    GraphLoopCondition where;
    bool descending = false;
    std::optional<std::int64_t> limit;
    std::vector<GraphLoopBranch> branches;
    std::vector<GraphLoopCarry> carry;
    // LP-3a, `over: {"draw_items": {"contract": "<material pass contract>"}}`:
    // the contract's name, its `where` (tags only) and whether `order` was
    // written ({"by": "phase_default"} is the only order it takes).
    std::string draw_contract;
    std::optional<GraphLoopTagFilter> draw_where;
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
// graph.loop_order_unsupported@1, graph.loop_expression_invalid@1 and
// graph.insert_anchor_unknown@1 are GraphRegistryError. The tier is not
// decided here (classifyGraphLoopTier). `pointer` is the node's JSON pointer
// in the authoring document. graph.loop_feature_pending@1 (LP-1a) is no longer
// thrown: LP-1b runs all four features it stood for; the code stays reserved.
GraphLoopShape parseGraphLoopShape(const nlohmann::json &node, const std::string &pointer = {});

// What a branch body holds, for the tier decision.
struct GraphLoopBodyTraits {
    bool draw = false;      // a draw.* node
    bool render = false;    // a render.* or value.* node
    bool nested = false;    // a graph.instance or graph.loop node
    bool graphics = false;  // a render.fullscreen / render.raster node
    bool resource = false;  // LP-3a: a render.image / render.buffer / render.attribute node
};

// Section 3: which stage runs the loop. Throws graph.loop_tier_unsupported@1
// {node, reason} or graph.loop_tier_mismatch@1 {node, tier, actual}. LP-1a
// answers `expand` for `range` over render/value bodies and refuses the rest;
// LP-3a answers `draw_loop` for `draw_items` over draw.* bodies (reasons
// resource_in_draw_body, mixed_body, carry_in_draw_loop, limit_in_draw_loop);
// LP-Vb adds view_axis.
GraphLoopTier classifyGraphLoopTier(const GraphLoopShape &shape,
                                    const std::vector<GraphLoopBodyTraits> &bodies);

struct GraphLoopElement {
    std::int64_t element = 0;
    // Position after where, order and limit.
    std::size_t index = 0;
    // The branch the element takes (the first whose `when` holds).
    std::size_t branch = 0;
};

// Answers mip_levels_of / layers_of (graphloopexpression.hpp); empty where
// no composed config is at hand, which makes every constant not_static.
using GraphLoopConstants = std::function<std::int64_t(std::string_view function, const std::string &target)>;

// LP-1b: the set's length: range.count, evaluated when it is an expression.
// A refusal is graph.loop_expression_invalid@1 {node, pointer, reason}.
std::int64_t graphLoopCount(const GraphLoopShape &shape, const GraphLoopConstants &constants);

// where -> order -> limit over the elements 0..count-1, then each element's
// branch (its `when` sees element, index and count). More than
// graphLoopIterationLimit elements is graph.loop_expansion_limit@1 {node,
// count, limit}. `count` is the set's length (graphLoopCount; LP-Vb an axis
// length).
std::vector<GraphLoopElement> graphLoopElements(const GraphLoopShape &shape, std::int64_t count,
                                                const GraphLoopConstants &constants = {});

} // namespace Pelican
