#include "graphcomposition.hpp"
#include "featurecompose.hpp"
#include "graphloopexpression.hpp"
#include "renderpipeline.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace Pelican {
namespace {
using Json = nlohmann::json;
std::string identifier(const Json &value) {
    if (!value.is_string()) throw std::invalid_argument("graph identifier must be a string");
    const auto result = value.get<std::string>();
    if (result.empty() || result.find('/') != std::string::npos)
        throw std::invalid_argument("graph identifier must be nonempty and contain no slash");
    return result;
}
void object(const Json &value, const char *description) {
    if (!value.is_object()) throw std::invalid_argument(description);
}
// CT-1: a field the authoring document must hold, refused by what holds it
// ("graph definition gain requires nodes") rather than by the JSON library's
// "key not found".
const Json &member(const Json &holder, const char *key, const std::string &description) {
    const auto found = holder.is_object() ? holder.find(key) : holder.end();
    if (found == holder.end()) throw std::invalid_argument(description + " requires " + key);
    return *found;
}
std::string scopeDescription(const std::string &definition) {
    return definition.empty() ? std::string{"the graph document"} : "graph definition " + definition;
}

// --- graph.loop (LP-1a). The form, the tier and the elements.

using Pointer = Json::json_pointer;

// LP-1b: an expression in a place of the loop's own parameters. `scope` is
// what it may read; a refusal names the loop and the document pointer.
void checkLoopExpression(const std::string &node, const Json &value, GraphLoopExpressionKind kind,
                         const GraphLoopExpressionScope &scope, const std::string &pointer) {
    try {
        checkGraphLoopExpression(value, kind, scope);
    } catch (const GraphLoopExpressionError &error) {
        throw graphLoopExpressionInvalid(node, pointer, error.reason());
    }
}

std::int64_t loopInteger(const std::string &node, const Json &value, const char *what, const std::string &pointer) {
    // Only the places decision 5 lists take an expression.
    if (isGraphLoopExpression(value))
        throw graphLoopExpressionInvalid(node, pointer, graphLoopExpressionFieldNotAllowed);
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string{"graph.loop "} + what + " must be an integer literal: " + node);
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        throw std::invalid_argument(std::string{"graph.loop "} + what + " is out of range: " + node);
    return value.get<std::int64_t>();
}

// A `where` / `when` value: an integer literal, or an integer expression that
// reads what `scope` allows.
Json loopConditionValue(const std::string &node, const Json &value, const char *what,
                        const GraphLoopExpressionScope &scope, const std::string &pointer) {
    if (isGraphLoopExpression(value)) {
        checkLoopExpression(node, value, GraphLoopExpressionKind::integer, scope, pointer);
        return value;
    }
    return loopInteger(node, value, what, pointer);
}

GraphLoopCondition loopCondition(const std::string &node, const Json &value, const char *what,
                                 const GraphLoopExpressionScope &scope, const Pointer &pointer) {
    if (!value.is_object() || value.size() != 1)
        throw std::invalid_argument(std::string{"graph.loop "} + what +
            " must be {\"element\": n}, {\"element_in\": [...]} or {\"element_not_in\": [...]}: " + node);
    GraphLoopCondition condition;
    // Not *items().begin(): that dereference returns a reference into the
    // temporary proxy, which is gone at the end of the statement.
    const auto first = value.begin();
    const auto &key = first.key();
    const auto &operand = first.value();
    if (key == "element") {
        condition.kind = GraphLoopCondition::Kind::element;
        const auto at = (pointer / key).to_string();
        condition.values.push_back(loopConditionValue(node, operand, what, scope, at));
        condition.pointers.push_back(at);
        return condition;
    }
    if (key != "element_in" && key != "element_not_in")
        throw std::invalid_argument(std::string{"graph.loop "} + what + " does not know the test " + key + ": " + node);
    condition.kind = key == "element_in" ? GraphLoopCondition::Kind::element_in
                                         : GraphLoopCondition::Kind::element_not_in;
    if (isGraphLoopExpression(operand))
        throw graphLoopExpressionInvalid(node, (pointer / key).to_string(), graphLoopExpressionFieldNotAllowed);
    if (!operand.is_array())
        throw std::invalid_argument(std::string{"graph.loop "} + what + " " + key + " must be an array: " + node);
    for (std::size_t index = 0; index < operand.size(); ++index) {
        const auto at = (pointer / key / index).to_string();
        condition.values.push_back(loopConditionValue(node, operand.at(index), what, scope, at));
        condition.pointers.push_back(at);
    }
    return condition;
}

// One value of a condition, for one element.
std::int64_t loopConditionOperand(const std::string &node, const GraphLoopCondition &condition, std::size_t index,
                                  const GraphLoopExpressionValues &values, const GraphLoopConstants &constants) {
    const auto &value = condition.values.at(index);
    if (!isGraphLoopExpression(value)) return value.get<std::int64_t>();
    try {
        return evaluateGraphLoopExpression(value, GraphLoopExpressionKind::integer, values, constants).get<std::int64_t>();
    } catch (const GraphLoopExpressionError &error) {
        throw graphLoopExpressionInvalid(node, condition.pointers.at(index), error.reason());
    }
}

bool loopConditionMatches(const std::string &node, const GraphLoopCondition &condition,
                          const GraphLoopExpressionValues &values, const GraphLoopConstants &constants) {
    const auto element = *values.element;
    const auto any = [&] {
        for (std::size_t index = 0; index < condition.values.size(); ++index)
            if (loopConditionOperand(node, condition, index, values, constants) == element) return true;
        return false;
    };
    switch (condition.kind) {
    case GraphLoopCondition::Kind::always: return true;
    case GraphLoopCondition::Kind::element:
    case GraphLoopCondition::Kind::element_in: return any();
    case GraphLoopCondition::Kind::element_not_in: return !any();
    }
    return false;
}

// LP-3a: the tag test of a draw_loop's `where` / `when`.
GraphLoopTagFilter loopTagFilter(const std::string &node, const Json &value, const char *what) {
    if (!value.is_object())
        throw std::invalid_argument(std::string{"graph.loop "} + what +
                                    " tags must be {\"include\", \"exclude\", \"require_all\"}: " + node);
    GraphLoopTagFilter filter;
    for (const auto &[key, list] : value.items()) {
        std::vector<std::string> *target = key == "include" ? &filter.include
            : key == "exclude" ? &filter.exclude : key == "require_all" ? &filter.require_all : nullptr;
        if (target == nullptr)
            throw std::invalid_argument(std::string{"graph.loop "} + what + " tags do not take " + key + ": " + node);
        if (!list.is_array())
            throw std::invalid_argument(std::string{"graph.loop "} + what + " tags " + key + " must be an array: " + node);
        for (const auto &tag : list) {
            if (!tag.is_string() || tag.get_ref<const std::string &>().empty())
                throw std::invalid_argument(std::string{"graph.loop "} + what + " tags must be nonempty strings: " + node);
            target->push_back(tag.get<std::string>());
        }
        std::sort(target->begin(), target->end());
        target->erase(std::unique(target->begin(), target->end()), target->end());
    }
    return filter;
}

// LP-3a: a draw_loop's `where` takes the tags alone (the same words as a
// material pass's material_filter, and require_all); a branch's `when` any of
// the material's own attributes.
GraphLoopDrawCondition loopDrawCondition(const std::string &node, const Json &value, const char *what, bool where) {
    if (!value.is_object())
        throw std::invalid_argument(std::string{"graph.loop "} + what + " of draw_items must be an object: " + node);
    GraphLoopDrawCondition condition;
    for (const auto &[key, operand] : value.items()) {
        if (isGraphLoopExpression(operand))
            throw std::invalid_argument(std::string{"graph.loop "} + what +
                                        " of draw_items compares literal values: " + node);
        if (key == "tags") {
            condition.tags = loopTagFilter(node, operand, what);
            continue;
        }
        if (where)
            throw std::invalid_argument(std::string{"graph.loop where of draw_items takes tags only: "} + node);
        if (key == "material" || key == "contract") {
            if (!operand.is_string() || operand.get_ref<const std::string &>().empty())
                throw std::invalid_argument(std::string{"graph.loop when "} + key + " must be a nonempty string: " + node);
            (key == "material" ? condition.material : condition.contract) = operand.get<std::string>();
        } else if (key == "skinned") {
            if (!operand.is_boolean())
                throw std::invalid_argument(std::string{"graph.loop when skinned must be a boolean: "} + node);
            condition.skinned = operand.get<bool>();
        } else if (key == "has_variant") {
            if (operand.is_boolean()) condition.has_variant = operand.get<bool>();
            else if (operand.is_string() && !operand.get_ref<const std::string &>().empty())
                condition.has_variant_named = operand.get<std::string>();
            else
                throw std::invalid_argument(std::string{"graph.loop when has_variant is a boolean or a variant name: "} +
                                            node);
        } else {
            throw std::invalid_argument(std::string{"graph.loop when of draw_items does not know "} + key + ": " + node);
        }
    }
    if (condition.contract && *condition.contract != "legacy_gbuffer_v1" && *condition.contract != "gbuffer_v1" &&
        *condition.contract != "forward_scene_color_v1")
        throw std::invalid_argument("graph.loop when contract is a material shader contract: " + node);
    return condition;
}

std::vector<std::string> loopNames(const std::string &node, const Json &parameters, const char *key) {
    std::vector<std::string> names;
    const auto found = parameters.find(key);
    if (found == parameters.end()) return names;
    if (!found->is_array())
        throw std::invalid_argument(std::string{"graph.loop "} + key + " must be an array: " + node);
    for (const auto &name : *found) {
        if (!name.is_string() || name.get_ref<const std::string &>().empty())
            throw std::invalid_argument(std::string{"graph.loop "} + key + " names must be nonempty strings: " + node);
        names.push_back(name.get<std::string>());
    }
    return names;
}
} // namespace

std::string graphLoopTierName(GraphLoopTier tier) {
    return tier == GraphLoopTier::expand ? "expand" : "draw_loop";
}

GraphLoopShape parseGraphLoopShape(const Json &node, const std::string &pointer) {
    GraphLoopShape shape;
    shape.node = identifier(node.at("id"));
    shape.pointer = pointer;
    const auto &id = shape.node;
    const auto parameters_pointer = Pointer{pointer} / "parameters";
    const auto found = node.find("parameters");
    if (found == node.end() || !found->is_object())
        throw std::invalid_argument("graph.loop requires a parameters object: " + id);
    const auto &parameters = *found;
    for (const auto &[key, value] : parameters.items()) {
        (void)value;
        static const std::set<std::string> known{"over", "where", "order", "limit", "branches", "carry",
                                                 "tier", "after", "before", "insert", "canonical_anchor"};
        if (!known.contains(key)) throw std::invalid_argument("graph.loop does not take parameter " + key + ": " + id);
    }
    // over: the set.
    const auto over = parameters.find("over");
    if (over == parameters.end() || !over->is_object() || over->size() != 1)
        throw std::invalid_argument("graph.loop over must name exactly one set: " + id);
    const auto first = over->begin();
    const auto &set = first.key();
    const auto &declaration = first.value();
    if (set == "range") {
        shape.set = GraphLoopSet::range;
        if (!declaration.is_object() || declaration.size() != 1 || !declaration.contains("count"))
            throw std::invalid_argument("graph.loop range takes exactly {\"count\": n}: " + id);
        const auto &count = declaration.at("count");
        const auto count_pointer = (parameters_pointer / "over" / "range" / "count").to_string();
        if (count.is_object() && count.contains("node") && count.contains("port") && !isGraphLoopExpression(count)) {
            // A count read from the graph's values changes per frame: the
            // expand stage cannot build it when the graph is recompiled.
            shape.per_frame_count = true;
        } else if (isGraphLoopExpression(count)) {
            // LP-1b: composition-time constants only; the elements do not
            // exist yet, so neither does any variable.
            checkLoopExpression(id, count, GraphLoopExpressionKind::integer, {}, count_pointer);
            shape.range_count_expression = count;
        } else {
            shape.range_count = loopInteger(id, count, "range count", count_pointer);
            if (shape.range_count < 0 || shape.range_count > graphLoopRangeCountBound)
                throw std::invalid_argument("graph.loop range count must be 0.." +
                                            std::to_string(graphLoopRangeCountBound) + ": " + id);
        }
    } else if (set == "view_axis") {
        // LP-Vb: one axis of a view family; its length is known when the
        // graph is resolved (the family's provider).
        shape.set = GraphLoopSet::view_axis;
        const auto name = [&](const char *key) {
            const auto found = declaration.is_object() ? declaration.find(key) : declaration.end();
            if (found == declaration.end() || !found->is_string() || found->get_ref<const std::string &>().empty())
                return std::string{};
            return found->get<std::string>();
        };
        shape.view_family = name("family");
        shape.view_axis = name("axis");
        if (!declaration.is_object() || declaration.size() != 2 || shape.view_family.empty() || shape.view_axis.empty())
            throw std::invalid_argument(
                "graph.loop view_axis takes exactly {\"family\": \"<view family>\", \"axis\": \"<axis>\"}: " + id);
    } else if (set == "draw_items") {
        // LP-3a: the draws of one material pass, one element per material.
        shape.set = GraphLoopSet::draw_items;
        if (!declaration.is_object() || declaration.size() != 1 || !declaration.contains("contract") ||
            !declaration.at("contract").is_string())
            throw std::invalid_argument("graph.loop draw_items takes exactly {\"contract\": \"<material_contract>\"}: " + id);
        shape.draw_contract = declaration.at("contract").get<std::string>();
        const auto contract = materialPassContractFromName(shape.draw_contract);
        if (!contract)
            throw std::invalid_argument("graph.loop draw_items names an unknown material_contract '" +
                                        shape.draw_contract + "': " + id);
        // A legacy pass draws with no route and no phase; its queue is not
        // the one a draw loop walks.
        if (*contract == MaterialPassContract::legacy_gbuffer_v1)
            throw GraphRegistryError("graph.loop_contract_unsupported@1", {{"node", id}, {"contract", shape.draw_contract}});
    } else {
        shape.set = GraphLoopSet::unknown;
    }
    const bool draw_items = shape.set == GraphLoopSet::draw_items;
    // `where` runs before the order: it sees the element and nothing the order
    // or the limit decides. A draw_loop's element is a material: its `where`
    // tests the material's tags.
    if (const auto where = parameters.find("where"); where != parameters.end()) {
        if (draw_items) {
            auto condition = loopDrawCondition(id, *where, "where", true);
            if (condition.tags) shape.draw_where = std::move(*condition.tags);
        } else {
            shape.where = loopCondition(id, *where, "where", {true, false, false}, parameters_pointer / "where");
        }
    }
    if (const auto order = parameters.find("order"); order != parameters.end()) {
        const auto refused = [&] {
            return GraphRegistryError("graph.loop_order_unsupported@1", {{"node", id}, {"order", *order}});
        };
        // Decision 4: a draw_loop keeps the phase's one order (v0).
        if (draw_items) {
            if (!order->is_object() || order->size() != 1 || order->value("by", Json{}) != "phase_default")
                throw refused();
        } else if (!order->is_object() || !order->contains("by") || order->at("by") != "element") throw refused();
        for (const auto &[key, value] : order->items()) {
            (void)value;
            if (key != "by" && key != "direction") throw refused();
        }
        // Json("ascending"), not Json{"ascending"}: braces make a one-element
        // array, which refused every order that left the direction out.
        const auto direction = order->value("direction", Json("ascending"));
        if (direction != "ascending" && direction != "descending") throw refused();
        shape.descending = direction == "descending";
    }
    if (const auto limit = parameters.find("limit"); limit != parameters.end()) {
        shape.limit = loopInteger(id, *limit, "limit", (parameters_pointer / "limit").to_string());
        if (*shape.limit < 0) throw std::invalid_argument("graph.loop limit must not be negative: " + id);
    }
    // branches: tested top to bottom; the last is the else.
    const auto branches = parameters.find("branches");
    if (branches == parameters.end() || !branches->is_array() || branches->empty())
        throw std::invalid_argument("graph.loop requires a nonempty branches array: " + id);
    for (std::size_t index = 0; index < branches->size(); ++index) {
        const auto &entry = branches->at(index);
        if (!entry.is_object() || !entry.contains("body"))
            throw std::invalid_argument("graph.loop branch requires a body (a definition name or null): " + id);
        for (const auto &[key, value] : entry.items()) {
            (void)value;
            if (key != "when" && key != "body")
                throw std::invalid_argument("graph.loop branch does not take " + key + ": " + id);
        }
        GraphLoopBranch branch;
        if (const auto when = entry.find("when"); when != entry.end() && draw_items)
            branch.draw_when = loopDrawCondition(id, *when, "when", false);
        else if (when != entry.end())
            branch.when = loopCondition(id, *when, "when", {true, true, true},
                                        parameters_pointer / "branches" / index / "when");
        else if (index + 1 != branches->size())
            throw std::invalid_argument("graph.loop: only the last branch may omit when: " + id);
        if (!entry.at("body").is_null()) branch.body = identifier(entry.at("body"));
        shape.branches.push_back(std::move(branch));
    }
    if (branches->back().contains("when"))
        throw GraphRegistryError("graph.loop_branch_else_missing@1", {{"node", id}});
    if (const auto carry = parameters.find("carry"); carry != parameters.end()) {
        if (!carry->is_array()) throw std::invalid_argument("graph.loop carry must be an array: " + id);
        std::set<std::string> targets;
        for (const auto &entry : *carry) {
            const auto name = [&](const char *key) {
                const auto found = entry.is_object() ? entry.find(key) : entry.end();
                if (!entry.is_object() || entry.size() != 2 || found == entry.end() || !found->is_string() ||
                    found->get_ref<const std::string &>().empty())
                    throw std::invalid_argument("graph.loop carry entries are {\"from\": <output>, \"to\": <input>}: " + id);
                return found->get<std::string>();
            };
            GraphLoopCarry pair{name("from"), name("to")};
            if (!targets.insert(pair.to).second)
                throw std::invalid_argument("graph.loop carries into " + pair.to + " twice: " + id);
            shape.carry.push_back(std::move(pair));
        }
    }
    if (const auto tier = parameters.find("tier"); tier != parameters.end()) {
        if (*tier == "expand") shape.tier = GraphLoopTier::expand;
        else if (*tier == "draw_loop") shape.tier = GraphLoopTier::draw_loop;
        else throw std::invalid_argument("graph.loop tier must be \"expand\" or \"draw_loop\": " + id);
    }
    shape.after = loopNames(id, parameters, "after");
    shape.before = loopNames(id, parameters, "before");
    // Placement: the same two forms a graph pass takes (LP-0b), for the
    // loop's whole expansion.
    if (parameters.contains("insert") && parameters.contains("canonical_anchor"))
        throw std::invalid_argument("graph.loop insert and canonical_anchor exclude each other: " + id);
    if (const auto insert = parameters.find("insert"); insert != parameters.end()) {
        const auto side = insert->is_object() && insert->size() == 1 ? insert->begin() : insert->end();
        if (side == insert->end() || (side.key() != "after" && side.key() != "before") ||
            !side->is_string() || side->get_ref<const std::string &>().empty())
            throw std::invalid_argument("graph.loop insert must be {\"after\" | \"before\": \"<pass>\"}: " + id);
        shape.placement = GraphLoopPlacement{side.key() == "after", side->get<std::string>()};
    }
    if (const auto anchor = parameters.find("canonical_anchor"); anchor != parameters.end()) {
        if (!anchor->is_string()) throw std::invalid_argument("graph.loop canonical_anchor must be a string: " + id);
        if (!isCanonicalRenderPassAnchor(anchor->get<std::string>()))
            throw GraphRegistryError("graph.insert_anchor_unknown@1", {{"node", id}, {"anchor", *anchor}});
        shape.placement = GraphLoopPlacement{true, anchor->get<std::string>()};
    }
    return shape;
}

GraphLoopTier classifyGraphLoopTier(const GraphLoopShape &shape,
                                    const std::vector<GraphLoopBodyTraits> &bodies) {
    const auto unsupported = [&](const char *reason) {
        return GraphRegistryError("graph.loop_tier_unsupported@1", {{"node", shape.node}, {"reason", reason}});
    };
    bool draw = false;
    bool render = false;
    bool resource = false;
    for (const auto &body : bodies) {
        if (body.nested) throw unsupported("nested");
        draw = draw || body.draw;
        render = render || body.render;
        resource = resource || body.resource;
    }
    // A draw body runs inside one material pass (draw_loop); a render body is
    // copied per element (expand). Mixed, the loop has no single place to run.
    // LP-3a: `draw_items` over draw.* bodies is draw_loop.
    auto actual = GraphLoopTier::expand;
    if (shape.set == GraphLoopSet::draw_items) {
        // One material pass records the draws: a body cannot make a resource
        // in it, and has nowhere to put a pass or a value.
        if (resource) throw unsupported("resource_in_draw_body");
        if (render) throw unsupported("mixed_body");
        // The elements are the queue's ranges, drawn in the phase's order
        // every frame: no element hands a value to the next, and the frame
        // decides how many there are.
        if (!shape.carry.empty()) throw unsupported("carry_in_draw_loop");
        if (shape.limit) throw unsupported("limit_in_draw_loop");
        actual = GraphLoopTier::draw_loop;
    } else {
        if (draw) throw unsupported("mixed_body");
        // LP-Vb: a view axis is expanded like a range; an axis known only per
        // frame is refused when the graph is resolved and its length is asked.
        if (shape.set != GraphLoopSet::range && shape.set != GraphLoopSet::view_axis)
            throw unsupported("set_unsupported");
        if (shape.per_frame_count) throw unsupported("per_frame_value");
    }
    if (shape.tier && *shape.tier != actual)
        throw GraphRegistryError("graph.loop_tier_mismatch@1",
            {{"node", shape.node}, {"tier", graphLoopTierName(*shape.tier)}, {"actual", graphLoopTierName(actual)}});
    return actual;
}

std::int64_t graphLoopCount(const GraphLoopShape &shape, const GraphLoopConstants &constants) {
    if (!shape.range_count_expression) return shape.range_count;
    std::int64_t count = 0;
    const auto pointer = (Pointer{shape.pointer} / "parameters" / "over" / "range" / "count").to_string();
    try {
        count = evaluateGraphLoopExpression(*shape.range_count_expression, GraphLoopExpressionKind::integer, {},
                                            constants).get<std::int64_t>();
    } catch (const GraphLoopExpressionError &error) {
        throw graphLoopExpressionInvalid(shape.node, pointer, error.reason());
    }
    // A range of negative length is empty. One past the bound would only make
    // the resolution walk it before the iteration limit refuses it.
    if (count > graphLoopRangeCountBound)
        throw graphLoopExpressionInvalid(shape.node, pointer, graphLoopExpressionOverflow);
    return (std::max)(count, std::int64_t{0});
}

std::vector<GraphLoopElement> graphLoopElements(const GraphLoopShape &shape, std::int64_t count,
                                                const GraphLoopConstants &constants) {
    std::vector<GraphLoopElement> elements;
    for (std::int64_t step = 0; step < count; ++step) {
        const auto element = shape.descending ? count - 1 - step : step;
        if (!loopConditionMatches(shape.node, shape.where, {element, std::nullopt, std::nullopt}, constants)) continue;
        if (shape.limit && static_cast<std::int64_t>(elements.size()) >= *shape.limit) break;
        elements.push_back({element, elements.size(), 0});
    }
    if (elements.size() > graphLoopIterationLimit)
        throw GraphRegistryError("graph.loop_expansion_limit@1",
            {{"node", shape.node}, {"count", elements.size()}, {"limit", graphLoopIterationLimit}});
    // A branch's `when` sees where the element landed and how many there are.
    const auto total = static_cast<std::int64_t>(elements.size());
    for (auto &entry : elements) {
        const GraphLoopExpressionValues values{entry.element, static_cast<std::int64_t>(entry.index), total};
        const auto branch = std::find_if(shape.branches.begin(), shape.branches.end(),
            [&](const GraphLoopBranch &candidate) {
                return loopConditionMatches(shape.node, candidate.when, values, constants);
            });
        entry.branch = static_cast<std::size_t>(branch - shape.branches.begin());
    }
    return elements;
}

GraphRegistryError graphLoopExpressionInvalid(const std::string &node, const std::string &pointer,
                                              std::string_view reason) {
    return GraphRegistryError("graph.loop_expression_invalid@1",
                              {{"node", node}, {"pointer", pointer}, {"reason", std::string{reason}}});
}

namespace {
// The template a scope is being expanded as, when it is a graph.loop branch
// body (LP-1a).
struct LoopScope {
    std::string node;
    std::size_t branch = 0;
};

// What a branch body holds: its boundary (the graph.input ids and the output
// ports, which every branch must share) and what the tier decision reads.
struct LoopBodyInterface {
    std::set<std::string> inputs;
    std::set<std::string> outputs;
    GraphLoopBodyTraits traits;
};

LoopBodyInterface loopBodyInterface(const Json &definition) {
    object(definition, "graph definition must be an object");
    LoopBodyInterface result;
    const auto &nodes = member(definition, "nodes", "a graph.loop branch body");
    if (!nodes.is_array()) throw std::invalid_argument("graph nodes must be an array");
    for (const auto &node : nodes) {
        const auto op = node.value("op", std::string{});
        if (op == "graph.input") result.inputs.insert(node.value("id", std::string{}));
        else if (op == "graph.instance" || op == "graph.loop") result.traits.nested = true;
        else if (op.starts_with("draw.")) result.traits.draw = true;
        else if (op == "render.image" || op == "render.buffer" || op == "render.attribute")
            result.traits.render = result.traits.resource = true;
        else if (op.starts_with("render.") || op.starts_with("value.")) result.traits.render = true;
        if (op == "render.fullscreen" || op == "render.raster" || op == "render.material_pass")
            result.traits.graphics = true;
        // The loop's expansion lands as one run where the loop says; a body
        // node has no place of its own to take.
        if (const auto parameters = node.find("parameters"); parameters != node.end() && parameters->is_object() &&
            (parameters->contains("insert") || parameters->contains("canonical_anchor")))
            throw std::invalid_argument("a graph.loop body node takes its place from the loop's insert: " +
                                        node.value("id", std::string{}));
        // LP-0e: a shadow depth pass and a snapshot copy are passes too.
        if (op == "render.shadow_depth" || op == "render.snapshot_copy") result.traits.graphics = true;
    }
    const auto &outputs = member(definition, "outputs", "a graph.loop branch body");
    object(outputs, "graph outputs must be an object");
    for (const auto &[port, endpoint] : outputs.items()) {
        (void)endpoint;
        result.outputs.insert(port);
    }
    return result;
}

// The first port one side has and the other lacks, in name order.
std::optional<std::string> firstDifference(const std::set<std::string> &lhs, const std::set<std::string> &rhs) {
    std::vector<std::string> difference;
    std::set_symmetric_difference(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), std::back_inserter(difference));
    if (difference.empty()) return std::nullopt;
    return difference.front();
}

// Inside a definition, a render node's `after` / `before` naming a node of the
// same body means that body's node, so it takes the scope's path like the
// node's id does (LP-1a; the same hole for graph.instance bodies).
void scopeLocalOrder(Json &node, const std::map<std::string, const Json *> &local, const std::string &path) {
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object()) return;
    for (const auto *key : {"after", "before"}) {
        const auto order = parameters->find(key);
        if (order == parameters->end() || !order->is_array()) continue;
        for (auto &name : *order) {
            if (!name.is_string()) continue;
            const auto found = local.find(name.get<std::string>());
            if (found == local.end()) continue;
            const auto op = found->second->value("op", std::string{});
            if (op == "graph.input" || op == "graph.instance" || op == "graph.loop") continue;
            name = path + name.get<std::string>();
        }
    }
    // LP-0e: so does a snapshot copy's snapshot_after.
    if (const auto snapshot = parameters->find("snapshot_after"); snapshot != parameters->end() && snapshot->is_string())
        if (const auto found = local.find(snapshot->get<std::string>()); found != local.end()) {
            const auto op = found->second->value("op", std::string{});
            if (op != "graph.input" && op != "graph.instance" && op != "graph.loop")
                *snapshot = path + snapshot->get<std::string>();
        }
}

// LP-1b, decision 5: the places of a body node that take an expression, and
// of which kind. `path` is the member path from the node (`parameters/...`).
std::optional<GraphLoopExpressionKind> bodyExpressionPlace(const std::string &op, const std::vector<std::string> &path) {
    if (!op.starts_with("render.") || path.size() < 2 || path[0] != "parameters") return std::nullopt;
    if (path.size() == 2 && path[1] == "extent_scale") return GraphLoopExpressionKind::real;
    // RS-15: a body image's fixed size, layer count and mip count. The
    // evaluated integers are checked where the render target is read, as an
    // authored number is.
    if (op == "render.image" && path.size() == 2 &&
        (path[1] == "width" || path[1] == "height" || path[1] == "layers" || path[1] == "mip_levels"))
        return GraphLoopExpressionKind::integer;
    if (path.size() == 3 && path[1] == "view_select") return GraphLoopExpressionKind::integer;
    if (path.size() == 4 && path[1] == "shader" && path[2] == "defines") return GraphLoopExpressionKind::format;
    if (path.size() == 5 && path[1] == "ports" && path[3] == "subresource" && (path[4] == "mip" || path[4] == "layer"))
        return GraphLoopExpressionKind::integer;
    return std::nullopt;
}

// Every expression in a node's parameters is checked where it stands: in a
// loop body at a listed place it may read element, index and count;
// anywhere else -- outside a loop body, or at a place the list does not name
// -- it is field_not_allowed. A checked expression keeps its document pointer
// as `$pointer` in the expansion, so a refusal when it is evaluated per
// element points at what the author wrote.
void checkNodeExpressions(Json &node, const std::string &id, const Pointer &node_pointer, bool in_loop_body) {
    const auto parameters = node.find("parameters");
    if (parameters == node.end()) return;
    const auto op = node.value("op", std::string{});
    std::vector<std::string> path{"parameters"};
    std::function<void(Json &)> walk = [&](Json &value) {
        if (isGraphLoopExpression(value)) {
            auto at = node_pointer;
            for (const auto &token : path) at /= token;
            const auto pointer = at.to_string();
            const auto kind = in_loop_body ? bodyExpressionPlace(op, path) : std::nullopt;
            if (!kind) throw graphLoopExpressionInvalid(id, pointer, graphLoopExpressionFieldNotAllowed);
            try {
                checkGraphLoopExpression(value, *kind, {true, true, true});
            } catch (const GraphLoopExpressionError &error) {
                throw graphLoopExpressionInvalid(id, pointer, error.reason());
            }
            value["$pointer"] = pointer;
            return;
        }
        if (value.is_object()) {
            for (auto member = value.begin(); member != value.end(); ++member) {
                path.push_back(member.key());
                walk(member.value());
                path.pop_back();
            }
        } else if (value.is_array()) {
            for (std::size_t index = 0; index < value.size(); ++index) {
                path.push_back(std::to_string(index));
                walk(value.at(index));
                path.pop_back();
            }
        }
    };
    walk(*parameters);
}

class Expander {
    const Json &definitions;
    std::set<std::string> active_definitions;
  public:
    ExpandedGraphDocument result;
    explicit Expander(const Json &definitions) : definitions(definitions) {}

    Json scope(const Json &document, const std::string &path,
               const std::string &definition, const Json &arguments, const Json &parameters,
               const LoopScope *loop = nullptr) {
        const auto origin = [&](const std::string &id) {
            GraphNodeOrigin entry{path, definition, id};
            if (loop != nullptr) {
                entry.loop = loop->node;
                entry.loop_branch = loop->branch;
            }
            return entry;
        };
        const auto &nodes = member(document, "nodes", scopeDescription(definition));
        if (!nodes.is_array()) throw std::invalid_argument("graph nodes must be an array");
        std::map<std::string, const Json *> local;
        std::set<std::string> inputs;
        // Where each node stands in the authoring document (LP-1b: the
        // pointers of expression refusals).
        std::map<std::string, Pointer> pointers;
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const auto &node = nodes.at(index);
            const auto id = identifier(member(node, "id", "a graph node in " + scopeDescription(definition)));
            if (!local.emplace(id, &node).second) throw std::invalid_argument("duplicate graph node: " + id);
            if (!member(node, "op", "graph node " + id).is_string())
                throw std::invalid_argument("graph node op must be a string: " + id);
            if (node.at("op") == "graph.input") inputs.insert(id);
            pointers.emplace(id, definition.empty() ? Pointer{"/nodes"} / index
                                                    : Pointer{"/definitions"} / definition / "nodes" / index);
        }
        for (const auto &[input, endpoint] : arguments.items()) {
            (void)endpoint;
            if (!inputs.contains(input)) throw std::invalid_argument("unknown subgraph input: " + input);
        }
        object(parameters, "instance parameters must be an object");
        for (const auto &[parameter, value] : parameters.items()) {
            if (!inputs.contains(parameter)) throw std::invalid_argument("unknown subgraph parameter: " + parameter);
            // edge > parameters > default. A parameter under an edge is not a
            // contradiction: it is the value the port returns to when the edge
            // is removed, so it is still checked, just not used.
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("subgraph parameter must be a finite scalar: " + parameter);
        }
        Json defaults = Json::object();
        for (const auto &input : inputs) {
            if (arguments.contains(input)) continue;
            const auto &node = *local.at(input);
            if (!parameters.contains(input) && !node.contains("default"))
                throw std::invalid_argument("unconnected subgraph input: " + input);
            const auto &value = parameters.contains(input) ? parameters.at(input) : node.at("default");
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("subgraph parameter must be a finite scalar: " + input);
            defaults[input] = value;
        }
        // Record the supply of every instance input port while both halves are
        // still in hand. The root document's own parameters are not an
        // instance's, so they are left out, and so are a loop body
        // template's: its ports are the loop's, which Studio reads there.
        if (!path.empty() && loop == nullptr) {
            const auto scope = path.substr(0, path.size() - 1);
            for (const auto &input : inputs) {
                GraphInstanceInput record{scope, input};
                if (arguments.contains(input)) {
                    record.source = GraphInstanceInputSource::connected;
                    record.endpoint = arguments.at(input);
                } else {
                    record.source = parameters.contains(input) ? GraphInstanceInputSource::parameter
                                                              : GraphInstanceInputSource::defaulted;
                    record.endpoint = Json{{"node", path + input}, {"port", "value"}};
                    record.parameter = path + input;
                }
                result.instance_inputs.emplace(path + input, std::move(record));
            }
        }
        std::map<std::string, Json> instances;
        std::set<std::string> active_instances;
        // `each`: the edge names an element, so it reads a loop's `p[]`.
        std::function<Json(const Json &, bool each)> resolveAny;
        const auto resolve = [&](const Json &endpoint) { return resolveAny(endpoint, false); };
        // LP-1b: an edge into `input` of `receiver` that may name an element
        // of a loop's `p[]` output. Its `element` variable is the receiver's
        // element when the receiver is a loop; otherwise the edge reads a
        // constant element.
        const auto resolveEdge = [&](const Json &endpoint, const std::string &receiver, const std::string &input,
                                     bool receiver_loop) -> Json {
            if (!endpoint.is_object() || !endpoint.contains("element")) return resolve(endpoint);
            auto plain = endpoint;
            plain.erase("element");
            auto resolved = resolveAny(plain, true);
            auto element = endpoint.at("element");
            const auto pointer = (pointers.at(receiver) / "inputs" / input / "element").to_string();
            if (isGraphLoopExpression(element)) {
                const GraphLoopExpressionScope scope{receiver_loop, receiver_loop, receiver_loop};
                try {
                    checkGraphLoopExpression(element, GraphLoopExpressionKind::integer, scope);
                } catch (const GraphLoopExpressionError &error) {
                    throw graphLoopExpressionInvalid(receiver, pointer, error.reason());
                }
                element["$pointer"] = pointer;
            } else if (!element.is_number_integer()) {
                throw std::invalid_argument("an edge's element is an integer or {\"$expr\": ...}: " + receiver + "." + input);
            }
            auto &marker = resolved[graphLoopOutputMarker];
            marker["element"] = std::move(element);
            marker["receiver"] = receiver;
            marker["input"] = input;
            marker["receiver_loop"] = receiver_loop;
            return resolved;
        };
        std::function<const Json &(const std::string &)> instantiate;
        instantiate = [&](const std::string &id) -> const Json & {
            if (instances.contains(id)) return instances.at(id);
            if (!active_instances.insert(id).second) throw std::invalid_argument("cyclic subgraph connection: " + id);
            const auto &node = *local.at(id);
            const auto ref = identifier(member(node, "definition", "graph.instance " + id));
            if (!definitions.contains(ref)) throw std::invalid_argument("unknown graph definition: " + ref);
            // LP-3a: draw.* nodes say how a draw_loop draws one material; a
            // definition that holds one is a loop body and nothing else.
            if (const auto &body = definitions.at(ref); body.is_object() && body.contains("nodes") &&
                body.at("nodes").is_array())
                for (const auto &inner : body.at("nodes"))
                    if (inner.is_object() && inner.value("op", std::string{}).starts_with("draw."))
                        throw GraphRegistryError("graph.definition_role@1", {{"definition", ref}, {"node", id}});
            if (node.contains("owner")) {
                const auto &owner = node.at("owner");
                object(owner, "graph owner must be an object");
                if (owner.size() != 2 || !owner.contains("scene") || !owner.contains("object") ||
                    !owner.at("scene").is_string() || owner.at("scene").get_ref<const std::string &>().empty() ||
                    !owner.at("object").is_number_integer() || owner.at("object") <= 0)
                    throw std::invalid_argument("graph owner requires scene and positive authoring object id");
                result.instance_owners.emplace(path + id, GraphInstanceOwner{
                    owner.at("scene").get<std::string>(), owner.at("object").get<std::uint64_t>()});
            }
            Json bound = Json::object();
            const auto authored_inputs = node.value("inputs", Json::object());
            object(authored_inputs, "instance inputs must be an object");
            for (const auto &[port, endpoint] : authored_inputs.items())
                bound[port] = resolveEdge(endpoint, id, port, false);
            if (!active_definitions.insert(ref).second) throw std::invalid_argument("recursive graph definition: " + ref);
            auto outputs = scope(definitions.at(ref), path + id + "/", ref, bound,
                                 node.value("parameters", Json::object()));
            active_definitions.erase(ref);
            active_instances.erase(id);
            return instances.emplace(id, std::move(outputs)).first->second;
        };
        // LP-1a: a graph.loop. Its form is checked here, on every edit and
        // every load; each branch body is expanded once as a template under
        // `<loop>/$<branch>/`, so its ports and types are checked like any
        // other node's. The copies per element are made after composition.
        std::map<std::string, Json> loop_outputs;
        std::set<std::string> active_loops;
        std::function<const Json &(const std::string &)> expandLoop;
        expandLoop = [&](const std::string &id) -> const Json & {
            if (loop_outputs.contains(id)) return loop_outputs.at(id);
            if (!active_loops.insert(id).second) throw std::invalid_argument("cyclic subgraph connection: " + id);
            const auto &node = *local.at(id);
            auto shape = parseGraphLoopShape(node, pointers.at(id).to_string());
            // Every input reaches every element with the same value, but for
            // two (LP-1b): an edge with `element` reads one element of another
            // loop's `p[]` per element, and a carried input reads the loop's
            // edge in the first element only.
            std::set<std::string> carried;
            for (const auto &pair : shape.carry) carried.insert(pair.to);
            Json bound = Json::object();
            const auto authored_inputs = node.value("inputs", Json::object());
            object(authored_inputs, "graph.loop inputs must be an object");
            for (const auto &[port, endpoint] : authored_inputs.items()) {
                object(endpoint, "graph.loop input must be an endpoint object");
                if (endpoint.contains("element") && carried.contains(port))
                    throw std::invalid_argument("a carried input takes one first value, not one per element: " +
                                                id + "." + port);
                bound[port] = resolveEdge(endpoint, id, port, true);
            }
            std::vector<GraphLoopBodyTraits> traits;
            std::optional<std::size_t> reference;
            LoopBodyInterface boundary;
            for (std::size_t branch = 0; branch < shape.branches.size(); ++branch) {
                const auto &body = shape.branches[branch].body;
                if (!body) continue;
                if (!definitions.contains(*body)) throw std::invalid_argument("unknown graph definition: " + *body);
                auto interface = loopBodyInterface(definitions.at(*body));
                traits.push_back(interface.traits);
                if (!reference) {
                    reference = branch;
                    boundary = std::move(interface);
                    continue;
                }
                auto port = firstDifference(boundary.inputs, interface.inputs);
                if (!port) port = firstDifference(boundary.outputs, interface.outputs);
                if (port)
                    throw GraphRegistryError("graph.loop_branch_interface@1",
                        {{"node", id}, {"branch", branch}, {"definition", *body}, {"port", *port}});
            }
            const auto tier = classifyGraphLoopTier(shape, traits);
            // LP-3a: a draw_loop runs inside the material pass its `draws`
            // reach; the pass takes the place.
            if (tier == GraphLoopTier::draw_loop &&
                (shape.placement || !shape.after.empty() || !shape.before.empty()))
                throw std::invalid_argument("a draw_loop runs inside its render.material_pass; place the pass, "
                                            "not the loop: " + id);
            // A compute task is declared after every pass; a loop of tasks
            // has no place among the passes to take (as for a graph node).
            if (shape.placement && reference &&
                std::none_of(traits.begin(), traits.end(), [](const auto &body) { return body.graphics; }))
                throw GraphRegistryError("graph.insert_on_compute@1", {{"node", id}});
            // LP-1b: a carry names a body output and a body input, and the
            // input's first value is the loop's edge. The types are compared
            // when the ports are (validateGraphPorts).
            for (const auto &pair : shape.carry) {
                if (reference && !boundary.outputs.contains(pair.from))
                    throw std::invalid_argument("graph.loop carries from " + pair.from +
                                                ", which its body does not output: " + id);
                if (reference && !boundary.inputs.contains(pair.to))
                    throw std::invalid_argument("graph.loop carries into " + pair.to +
                                                ", which its body does not take: " + id);
                if (!bound.contains(pair.to))
                    throw std::invalid_argument("graph.loop carries into " + pair.to +
                                                ", which has no input edge for its first element: " + id);
                bound[pair.to][graphLoopCarryMarker] = Json{{"loop", id}, {"input", pair.to}};
            }
            GraphLoopExpansion expansion{shape, {}, bound};
            for (std::size_t branch = 0; branch < shape.branches.size(); ++branch) {
                const auto &body = shape.branches[branch].body;
                if (!body) {
                    expansion.branches.emplace_back(std::nullopt);
                    continue;
                }
                const auto prefix = id + "/$" + std::to_string(branch) + "/";
                const LoopScope template_scope{id, branch};
                auto outputs = scope(definitions.at(*body), prefix, *body, bound, Json::object(), &template_scope);
                expansion.branches.emplace_back(GraphLoopBodyTemplate{*body, prefix, std::move(outputs)});
            }
            // The outputs the loop declares are what other nodes may read:
            // `p`, the value of the last element that has a body, and (LP-1b)
            // `p[]`, each element's, read one element at a time.
            const auto declared = node.value("outputs", Json::array());
            if (!declared.is_array()) throw std::invalid_argument("graph.loop outputs must be an array: " + id);
            Json exposed = Json::object();
            for (const auto &entry : declared) {
                if (!entry.is_string()) throw std::invalid_argument("graph.loop outputs must be port names: " + id);
                const auto port = entry.get<std::string>();
                const bool each = port.ends_with("[]");
                const auto base = each ? port.substr(0, port.size() - 2) : port;
                if (!reference || !boundary.outputs.contains(base))
                    throw std::invalid_argument("unknown graph loop output: " + id + "." + port);
                auto endpoint = expansion.branches.at(*reference)->outputs.at(base);
                // An output that passes a carried input through is the loop's
                // value to its readers, not a carry of theirs.
                endpoint.erase(graphLoopCarryMarker);
                Json marker{{"loop", id}, {"port", base}};
                if (each) marker["element"] = nullptr;
                endpoint[graphLoopOutputMarker] = std::move(marker);
                exposed[port] = std::move(endpoint);
            }
            result.loops.insert_or_assign(id, std::move(expansion));
            active_loops.erase(id);
            return loop_outputs.emplace(id, std::move(exposed)).first->second;
        };
        resolveAny = [&](const Json &endpoint, bool each) -> Json {
            if (endpoint.is_object() && endpoint.contains("element"))
                throw std::invalid_argument("only an edge into a node names an element");
            const auto id = identifier(member(endpoint, "node", "a graph edge"));
            const auto port = identifier(member(endpoint, "port", "a graph edge"));
            const auto found = local.find(id);
            if (found == local.end()) throw std::invalid_argument("unknown graph source: " + id);
            const auto &node = *found->second;
            // A loop's `p[]` is read one element at a time, and an element is
            // named only of a `p[]`.
            if (each != (node.at("op") == "graph.loop" && port.ends_with("[]")))
                throw std::invalid_argument(each
                    ? "an edge names an element only of a graph.loop's p[] output: " + id + "." + port
                    : "a graph.loop's p[] output is read one element at a time; the edge names it: " + id + "." + port);
            if (node.at("op") == "graph.input") {
                if (port != "value") throw std::invalid_argument("graph input output port must be value");
                return arguments.contains(id) ? arguments.at(id)
                    : Json{{"node", path + id}, {"port", "value"}};
            }
            if (node.at("op") == "graph.instance") {
                const auto &outputs = instantiate(id);
                if (!outputs.contains(port)) throw std::invalid_argument("unknown subgraph output: " + port);
                return outputs.at(port);
            }
            if (node.at("op") == "graph.loop") {
                const auto &outputs = expandLoop(id);
                if (!outputs.contains(port))
                    throw std::invalid_argument("unknown graph loop output: " + id + "." + port);
                return outputs.at(port);
            }
            static const Json no_ports = Json::array();
            const auto declared = node.find("outputs");
            const auto &ports = declared != node.end() ? *declared : no_ports;
            if (!ports.is_array() || std::find(ports.begin(), ports.end(), port) == ports.end())
                throw std::invalid_argument("unknown graph output port: " + id + "." + port);
            return Json{{"node", path + id}, {"port", port}};
        };
        for (const auto &[id, node] : local) {
            if (node->at("op") == "graph.input") {
                if (defaults.contains(id)) {
                    result.nodes.push_back({{"id", path + id}, {"op", "value.parameter"},
                        {"parameters", {{"name", path + id}, {"default", defaults.at(id)}}},
                        {"inputs", Json::object()}, {"outputs", {"value"}}});
                    result.origins.emplace(path + id, origin(id));
                }
                continue;
            }
            if (node->at("op") == "graph.instance") { (void)instantiate(id); continue; }
            if (node->at("op") == "graph.loop") {
                if (!path.empty())
                    throw GraphRegistryError("graph.loop_nested@1", {{"node", id}, {"definition", definition}});
                (void)expandLoop(id);
                continue;
            }
            if (loop == nullptr && node->value("op", std::string{}).starts_with("draw."))
                throw std::invalid_argument("a draw.* node stands in a graph.loop body over draw_items: " + id);
            auto expanded = *node;
            expanded["id"] = path + id;
            if (!path.empty()) scopeLocalOrder(expanded, local, path);
            checkNodeExpressions(expanded, id, pointers.at(id), loop != nullptr);
            const auto authored_inputs = node->value("inputs", Json::object());
            object(authored_inputs, "node inputs must be an object");
            expanded["inputs"] = Json::object();
            for (const auto &[port, endpoint] : authored_inputs.items())
                expanded["inputs"][port] = resolveEdge(endpoint, id, port, false);
            result.nodes.push_back(std::move(expanded));
            result.origins.emplace(path + id, origin(id));
        }
        const auto &outputs = member(document, "outputs", scopeDescription(definition));
        object(outputs, "graph outputs must be an object");
        Json exposed = Json::object();
        for (const auto &[port, endpoint] : outputs.items()) {
            auto resolved = resolve(endpoint);
            // Even an input-to-output alias is unavailable when its instance
            // is inactive. Keep that boundary until dependency compilation.
            // A loop body template is no instance: nothing deactivates it.
            if (!path.empty() && loop == nullptr) resolved["$scopes"].push_back(path.substr(0, path.size() - 1));
            exposed[identifier(port)] = std::move(resolved);
        }
        return exposed;
    }
};
}

namespace {
// LP-1a: a graph.loop stands in the root document only. Every definition is
// looked at, not only the ones an instance or a loop reaches, so the refusal
// does not depend on whether the body is in use.
void requireNoNestedLoop(const Json &definitions) {
    for (const auto &[name, body] : definitions.items()) {
        if (!body.is_object()) continue;
        const auto nodes = body.find("nodes");
        if (nodes == body.end() || !nodes->is_array()) continue;
        for (const auto &node : *nodes)
            if (node.is_object() && node.value("op", std::string{}) == "graph.loop")
                throw GraphRegistryError("graph.loop_nested@1",
                    {{"node", node.value("id", std::string{})}, {"definition", name}});
    }
}

// LP-1a: the names a loop's expansion takes (`<loop>/<element>/<node>`) are
// derived and change with the elements, so the authored document may not
// name them. Ordering against a loop is written with the loop's own id.
void requireLoopExpansionUnreferenced(const Json &document, const std::map<std::string, GraphLoopExpansion> &loops) {
    if (loops.empty()) return;
    using Pointer = Json::json_pointer;
    const auto underLoop = [&](const Json &name) {
        if (!name.is_string()) return false;
        const auto &text = name.get_ref<const std::string &>();
        return std::any_of(loops.begin(), loops.end(), [&](const auto &entry) {
            return text.size() > entry.first.size() + 1 && text.starts_with(entry.first) &&
                   text[entry.first.size()] == '/';
        });
    };
    const auto refuse = [](const Json &name, const Pointer &pointer) {
        throw GraphRegistryError("graph.loop_expansion_referenced@1",
            {{"name", name}, {"pointer", pointer.to_string()}});
    };
    const auto orderLists = [&](const Json &holder, const Pointer &at) {
        if (!holder.is_object()) return;
        for (const auto *key : {"after", "before"}) {
            const auto found = holder.find(key);
            if (found == holder.end() || !found->is_array()) continue;
            for (std::size_t index = 0; index < found->size(); ++index)
                if (underLoop(found->at(index))) refuse(found->at(index), at / key / index);
        }
        if (const auto insert = holder.find("insert"); insert != holder.end() && insert->is_object())
            for (const auto &[side, anchor] : insert->items())
                if (underLoop(anchor)) refuse(anchor, at / "insert" / side);
    };
    const auto nodeLists = [&](const Json &holder, const Pointer &at) {
        const auto nodes = holder.find("nodes");
        if (nodes == holder.end() || !nodes->is_array()) return;
        for (std::size_t index = 0; index < nodes->size(); ++index) {
            const auto &node = nodes->at(index);
            if (!node.is_object()) continue;
            if (const auto parameters = node.find("parameters"); parameters != node.end())
                orderLists(*parameters, at / "nodes" / index / "parameters");
        }
    };
    nodeLists(document, Pointer{});
    if (const auto definitions = document.find("definitions");
        definitions != document.end() && definitions->is_object())
        for (const auto &[name, body] : definitions->items())
            if (body.is_object()) nodeLists(body, Pointer{"/definitions"} / name);
    const auto rendering = document.find("rendering");
    if (rendering == document.end() || !rendering->is_object()) return;
    const Pointer section{"/rendering"};
    if (const auto families = rendering->find("rendering_passes");
        families != rendering->end() && families->is_array())
        for (std::size_t family = 0; family < families->size(); ++family) {
            const auto &passes = families->at(family).value("passes", Json::array());
            for (std::size_t pass = 0; pass < passes.size(); ++pass)
                orderLists(passes.at(pass), section / "rendering_passes" / family / "passes" / pass);
        }
    if (const auto orders = rendering->find("frame_node_order");
        orders != rendering->end() && orders->is_object())
        for (const auto &[graph, names] : orders->items())
            if (names.is_array())
                for (std::size_t index = 0; index < names.size(); ++index)
                    if (underLoop(names.at(index)))
                        refuse(names.at(index), section / "frame_node_order" / graph / index);
    if (const auto planning = rendering->find("target_planning"); planning != rendering->end() && planning->is_object())
        if (const auto graphs = planning->find("graphs"); graphs != planning->end() && graphs->is_object())
            for (const auto &[graph, entry] : graphs->items()) {
                if (!entry.is_object()) continue;
                const auto nodes = entry.find("nodes");
                if (nodes == entry.end() || !nodes->is_object()) continue;
                for (const auto &[name, value] : nodes->items()) {
                    (void)value;
                    if (underLoop(Json(name)))
                        refuse(name, section / "target_planning" / "graphs" / graph / "nodes" / name);
                }
            }
}
} // namespace

namespace {
// LP-3a: a draw_loop's `draws` is how one material pass draws, so exactly one
// render.material_pass reads it, on its `draws` input, and the pass draws the
// contract the loop runs over. A `where` and the pass's own material_filter
// may not say two things.
void requireDrawLoopsConnected(const ExpandedGraphDocument &graph) {
    for (const auto &[id, expansion] : graph.loops) {
        if (expansion.shape.set != GraphLoopSet::draw_items) continue;
        std::vector<const Json *> passes;
        std::size_t readers = 0;
        for (const auto &node : graph.nodes) {
            const auto inputs = node.find("inputs");
            if (inputs == node.end() || !inputs->is_object()) continue;
            for (const auto &[port, endpoint] : inputs->items()) {
                const auto marker = endpoint.find(graphLoopOutputMarker);
                if (marker == endpoint.end() || marker->value("loop", std::string{}) != id) continue;
                ++readers;
                if (node.value("op", std::string{}) == "render.material_pass" && port == "draws" &&
                    marker->value("port", std::string{}) == "draws")
                    passes.push_back(&node);
            }
        }
        if (readers != 1 || passes.size() != 1)
            throw GraphRegistryError("graph.loop_draws_unconnected@1", {{"node", id}, {"count", readers}});
        const auto &pass = *passes.front();
        const auto &parameters = pass.value("parameters", Json::object());
        const auto contract = parameters.value("material_contract", std::string{});
        if (contract != expansion.shape.draw_contract)
            throw std::invalid_argument("graph.loop " + id + " runs over draw_items of " +
                                        expansion.shape.draw_contract + " but " +
                                        pass.at("id").get<std::string>() + " draws " +
                                        (contract.empty() ? std::string{"no material_contract"} : contract));
        if (expansion.shape.draw_where && parameters.contains("material_filter")) {
            const auto &filter = parameters.at("material_filter");
            const auto list = [&](const char *key) {
                std::vector<std::string> tags;
                if (filter.is_object() && filter.contains(key) && filter.at(key).is_array())
                    for (const auto &tag : filter.at(key))
                        if (tag.is_string()) tags.push_back(tag.get<std::string>());
                std::sort(tags.begin(), tags.end());
                tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
                return tags;
            };
            if (list("include") != expansion.shape.draw_where->include ||
                list("exclude") != expansion.shape.draw_where->exclude)
                throw std::invalid_argument("graph.loop " + id + " where and " + pass.at("id").get<std::string>() +
                                            " material_filter select different materials");
        }
    }
}
} // namespace

ExpandedGraphDocument expandGraphDocument(const Json &document) {
    if (document.at("schema") != "pelican.graph" || document.at("version") != 1)
        throw std::invalid_argument("unsupported graph schema/version");
    const auto definitions = document.value("definitions", Json::object());
    object(definitions, "graph definitions must be an object");
    requireNoNestedLoop(definitions);
    Expander expander{definitions};
    expander.result.outputs = expander.scope(document, "", "", Json::object(),
                                            document.value("parameters", Json::object()));
    auto &result = expander.result;
    requireLoopExpansionUnreferenced(document, result.loops);
    requireDrawLoopsConnected(result);
    const auto addAncestors = [&](std::set<std::string> &scopes, std::string_view path) {
        for (const auto &[scope, owner] : result.instance_owners) {
            if (path == scope || (path.starts_with(scope) && path.size() > scope.size() && path[scope.size()] == '/'))
                scopes.insert(scope);
        }
    };
    const auto takeScopes = [&](Json &endpoint, std::set<std::string> &scopes) {
        if (const auto found = endpoint.find("$scopes"); found != endpoint.end()) {
            for (const auto &scope : *found) addAncestors(scopes, scope.get_ref<const std::string &>());
            endpoint.erase(found);
        }
    };
    std::map<std::string, std::vector<std::string>> consumers;
    for (auto &node : result.nodes) {
        const auto id = node.at("id").get<std::string>();
        auto &scopes = result.node_scopes[id];
        addAncestors(scopes, id);
        for (auto &[port, endpoint] : node.at("inputs").items()) {
            takeScopes(endpoint, scopes);
            consumers[endpoint.at("node").get<std::string>()].push_back(id);
        }
    }
    // Propagate once per newly discovered node/scope pair. Declaration order
    // and duplicate edges do not affect the result.
    std::vector<std::pair<std::string, std::string>> pending;
    for (const auto &[id, scopes] : result.node_scopes)
        for (const auto &scope : scopes) pending.emplace_back(id, scope);
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto [id, scope] = pending[i];
        for (const auto &consumer : consumers[id])
            if (result.node_scopes.at(consumer).insert(scope).second)
                pending.emplace_back(consumer, scope);
    }
    for (auto &[port, endpoint] : result.outputs.items()) {
        auto &scopes = result.output_scopes[port];
        takeScopes(endpoint, scopes);
        const auto &source = result.node_scopes.at(endpoint.at("node").get<std::string>());
        scopes.insert(source.begin(), source.end());
    }
    // A port's value is unavailable when its own instance is, not only when its
    // producer is; an outer producer carries none of the instance's scopes.
    for (auto &[key, input] : result.instance_inputs) {
        (void)key;
        takeScopes(input.endpoint, input.scopes);
        addAncestors(input.scopes, input.scope);
        const auto &source = result.node_scopes.at(input.endpoint.at("node").get<std::string>());
        input.scopes.insert(source.begin(), source.end());
    }
    return std::move(expander.result);
}

GraphRegistryError::GraphRegistryError(std::string code, nlohmann::json detail)
    : std::runtime_error(code + " " + detail.dump()), code_(std::move(code)),
      detail_(std::move(detail)) {}

std::string_view graphExecutionDomainName(GraphExecutionDomain domain) {
    switch (domain) {
    case GraphExecutionDomain::values: return "values";
    case GraphExecutionDomain::rendering: return "rendering";
    case GraphExecutionDomain::animation: return "animation";
    }
    throw std::invalid_argument("unknown graph execution domain");
}

std::string_view graphOperationEvaluationName(GraphOperationEvaluation evaluation) {
    switch (evaluation) {
    case GraphOperationEvaluation::direct: return "direct";
    case GraphOperationEvaluation::replay: return "replay";
    case GraphOperationEvaluation::baked: return "baked";
    case GraphOperationEvaluation::recorded: return "recorded";
    }
    throw std::invalid_argument("unknown graph operation evaluation");
}

std::string_view graphParameterChangeName(GraphParameterChange change) {
    switch (change) {
    case GraphParameterChange::value: return "value";
    case GraphParameterChange::recompile: return "recompile";
    case GraphParameterChange::rebuild: return "rebuild";
    }
    throw std::invalid_argument("unknown graph parameter change");
}

std::map<std::string, LogicalType> validateGraphPorts(
    const ExpandedGraphDocument &graph, const LogicalTypeRegistry &registry,
    const GraphOperationContracts &operations) {
    return validateGraphPorts(graph, registry, GraphOperationContractResolver{
        [&](const Json &node) {
            const auto op = node.at("op").get<std::string>();
            const auto found = operations.find(op);
            if (found == operations.end())
                throw GraphRegistryError("graph.operation_unregistered@1",
                    {{"node", node.at("id").get<std::string>()}, {"op", op}});
            return found->second;
        }});
}

std::map<std::string, LogicalType> validateGraphPorts(
    const ExpandedGraphDocument &graph, const LogicalTypeRegistry &registry,
    const GraphOperationContractResolver &resolve) {
    std::map<std::string, GraphOperationContract> nodes;
    std::map<std::string, const Json *> documents;
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        const auto [found, inserted] = nodes.emplace(id, resolve(node));
        if (!inserted)
            throw std::invalid_argument("duplicate expanded graph node: " + id);
        documents.emplace(id, &node);
        std::set<std::string> ports;
        std::size_t typed = 0;
        const auto &outputs = node.at("outputs");
        if (!outputs.is_array()) throw std::invalid_argument("graph outputs must be an array: " + id);
        for (const auto &port : outputs) {
            const auto name = port.get<std::string>();
            // RS-4: a forwarded output may be listed; it need not be.
            const bool own = found->second.outputs.contains(name);
            if ((!own && !found->second.forwarded_outputs.contains(name)) || !ports.insert(name).second)
                throw std::invalid_argument("invalid graph output: " + id + "." + name);
            if (own) ++typed;
        }
        if (typed != found->second.outputs.size())
            throw std::invalid_argument("missing graph output: " + id);
        for (const auto &[port, type] : found->second.outputs) registry.requireCanonical(type);
    }
    // RS-4: a forwarded output is the type of the edge into its input, however
    // many forwarded outputs the edge passes through first.
    std::set<std::string> forwarding;
    std::function<const LogicalType &(const Json &)> sourceType = [&](const Json &endpoint) -> const LogicalType & {
        const auto id = endpoint.at("node").get<std::string>();
        const auto port = endpoint.at("port").get<std::string>();
        const auto node = nodes.find(id);
        if (node == nodes.end() ||
            (!node->second.outputs.contains(port) && !node->second.forwarded_outputs.contains(port)))
            throw std::invalid_argument("unknown graph source: " + id + "." + port);
        if (node->second.outputs.contains(port)) return node->second.outputs.at(port);
        const auto &input = node->second.forwarded_outputs.at(port);
        const auto &inputs = documents.at(id)->at("inputs");
        if (!inputs.is_object() || !inputs.contains(input))
            throw std::invalid_argument("unconnected graph input: " + id + "." + input);
        const auto key = id + "." + port;
        if (!forwarding.insert(key).second)
            throw std::invalid_argument("cyclic graph output forwarding: " + key);
        const auto &type = sourceType(inputs.at(input));
        forwarding.erase(key);
        return type;
    };
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        const auto &contract = nodes.at(id);
        const auto &inputs = node.at("inputs");
        object(inputs, "graph inputs must be an object");
        for (const auto &[port, endpoint] : inputs.items()) {
            const auto input = contract.inputs.find(port);
            if (input == contract.inputs.end())
                throw std::invalid_argument("unknown graph input: " + id + "." + port);
            const auto match = matchLogicalType(registry, sourceType(endpoint), input->second.accepted_type);
            if (match.status != LogicalTypeMatchStatus::exact)
                throw std::invalid_argument("graph type mismatch at " + id + "." + port +
                    " from " + endpoint.at("node").get<std::string>() + "." +
                    endpoint.at("port").get<std::string>() + ": " + match.reason_code);
        }
        for (const auto &[port, input] : contract.inputs)
            if (input.required && !inputs.contains(port))
                throw std::invalid_argument("unconnected graph input: " + id + "." + port);
    }
    // LP-1b: a carried input is read as the loop's edge in the first element
    // and as `from` in every later one, so the two are the same type. A
    // partition of the document (rendering, values) holds only its own
    // nodes; a carry whose ends are elsewhere is compared there.
    for (const auto &[loop, expansion] : graph.loops)
        for (const auto &pair : expansion.shape.carry) {
            const auto first = expansion.inputs.find(pair.to);
            if (first == expansion.inputs.end() || !nodes.contains(first->at("node").get<std::string>())) continue;
            for (const auto &branch : expansion.branches) {
                if (!branch || !branch->outputs.contains(pair.from)) continue;
                const auto &from = branch->outputs.at(pair.from);
                if (!nodes.contains(from.at("node").get<std::string>())) continue;
                if (!(sourceType(from) == sourceType(*first)))
                    throw GraphRegistryError("graph.loop_carry_type_mismatch@1",
                        {{"node", loop}, {"from", pair.from}, {"to", pair.to}});
            }
        }
    std::map<std::string, LogicalType> outputs;
    for (const auto &[port, endpoint] : graph.outputs.items()) outputs.emplace(port, sourceType(endpoint));
    return outputs;
}

std::string_view graphInstanceInputSourceName(GraphInstanceInputSource source) {
    switch (source) {
    case GraphInstanceInputSource::connected: return "connected";
    case GraphInstanceInputSource::parameter: return "parameter";
    case GraphInstanceInputSource::defaulted: return "default";
    }
    throw std::invalid_argument("unknown graph instance input source");
}

namespace {
// The scope token that names the root document. "root" and an absent `scope`
// are the same thing; a definition that is itself called "root" is therefore
// unaddressable, which is the price of a default that reads.
constexpr auto kRootScope = "root";

// Which body an op addresses: the root `nodes` array, or one definition's. The
// outputs map travels with it, because "this node is still the scope's output"
// is a property of the scope, not of the document.
struct EditScope {
    bool root = true;
    std::string name;
    Json *nodes = nullptr;
    const Json *outputs = nullptr;
    // The object the scope's own members live on: the document for the root,
    // the definition body otherwise. `set_layout` is the one op that writes
    // beside `nodes` rather than into it.
    Json *container = nullptr;
};

// A node named across an instance path belongs to no single body, so it is
// refused by name rather than reported as missing: the two call for different
// fixes. Inside the root scope a node that lives in a definition is named as
// exactly that, so the caller learns which scope to address instead.
Json &locateNode(const EditScope &scope, const Json &definitions, const std::string &id) {
    if (id.find('/') != std::string::npos)
        throw GraphRegistryError("graph.definition_immutable@1",
            {{"node", id}, {"reason", "instance_path"}});
    const auto found = std::find_if(scope.nodes->begin(), scope.nodes->end(), [&](const Json &node) {
        return node.contains("id") && node.at("id") == id;
    });
    if (found != scope.nodes->end()) return *found;
    if (scope.root) {
        for (const auto &[definition, body] : definitions.items())
            for (const auto &node : body.value("nodes", Json::array()))
                if (node.value("id", std::string{}) == id)
                    throw GraphRegistryError("graph.definition_immutable@1",
                        {{"node", id}, {"definition", definition}});
        throw GraphRegistryError("graph.node_missing@1", {{"node", id}});
    }
    throw GraphRegistryError("graph.node_missing@1",
        {{"node", id}, {"scope", scope.name}});
}

// Resolve one op's `scope` against the candidate. The definition body has to be
// looked up per op: an earlier op in the same list may have changed it.
EditScope resolveEditScope(Json &candidate, const Json &operation) {
    std::string name = kRootScope;
    if (const auto found = operation.find("scope"); found != operation.end() && !found->is_null()) {
        if (!found->is_string())
            throw std::invalid_argument("graph document op scope must be a string");
        name = found->get<std::string>();
    }
    if (name.empty() || name == kRootScope) {
        EditScope scope;
        scope.nodes = &candidate.at("nodes");
        if (!scope.nodes->is_array()) throw std::invalid_argument("graph nodes must be an array");
        const auto outputs = candidate.find("outputs");
        if (outputs != candidate.end()) scope.outputs = &*outputs;
        scope.container = &candidate;
        return scope;
    }
    const auto definitions = candidate.find("definitions");
    if (definitions == candidate.end() || !definitions->is_object() ||
        !definitions->contains(name))
        throw std::invalid_argument("unknown graph definition scope: " + name);
    auto &body = definitions->at(name);
    object(body, "graph definition must be an object");
    EditScope scope;
    scope.root = false;
    scope.name = name;
    scope.nodes = &body.at("nodes");
    if (!scope.nodes->is_array())
        throw std::invalid_argument("graph nodes must be an array: " + name);
    const auto outputs = body.find("outputs");
    if (outputs != body.end()) scope.outputs = &*outputs;
    scope.container = &body;
    return scope;
}

// --- Who names a definition. LP-0c.
//
// One walk answers it for every consumer: remove_definition refuses a
// definition something still names, rename_definition rewrites every name in
// the same op, and the boundary-port check (requireInputUnreferenced) finds the
// instances that supply a port. A new kind of referrer is added here once and
// all three see it. The root document is walked first, then the definition
// bodies in the document's key order; a nested instance inside a definition
// counts the same as a root one.
//
// `Document` is `Json` or `const Json`, so the same walk both reads (the
// checks) and writes (rename). `visit(scope, node, field)` gets the scope that
// holds the referring node -- the empty string for the root document, else the
// definition's name -- the node, and where in the node the name is written.
template <typename Document, typename Visit>
void visitDefinitionReferences(Document &document, const std::string &definition,
                               Visit &&visit) {
    const auto walk = [&](auto &nodes, const std::string &scope) {
        if (!nodes.is_array()) return;
        for (auto &node : nodes) {
            if (!node.is_object()) continue;
            if (node.value("op", std::string{}) == "graph.instance") {
                const auto found = node.find("definition");
                if (found != node.end() && found->is_string() && *found == definition)
                    visit(scope, node, Json::json_pointer{"/definition"});
            }
            // LP-1a: every `parameters.branches[i].body` of a graph.loop that
            // names this definition, written at `/parameters/branches/<i>/body`.
            if (node.value("op", std::string{}) == "graph.loop") {
                const auto parameters = node.find("parameters");
                if (parameters == node.end() || !parameters->is_object()) continue;
                const auto branches = parameters->find("branches");
                if (branches == parameters->end() || !branches->is_array()) continue;
                for (std::size_t index = 0; index < branches->size(); ++index) {
                    const auto &branch = branches->at(index);
                    if (!branch.is_object()) continue;
                    const auto body = branch.find("body");
                    if (body != branch.end() && body->is_string() && *body == definition)
                        visit(scope, node, Json::json_pointer{"/parameters/branches"} / index / "body");
                }
            }
        }
    };
    if (const auto nodes = document.find("nodes"); nodes != document.end())
        walk(*nodes, std::string{});
    const auto definitions = document.find("definitions");
    if (definitions == document.end() || !definitions->is_object()) return;
    for (auto body = definitions->begin(); body != definitions->end(); ++body) {
        if (!body->is_object()) continue;
        if (const auto nodes = body->find("nodes"); nodes != body->end())
            walk(*nodes, body.key());
    }
}

struct DefinitionReference {
    // Empty for the root document, else the definition holding the referrer.
    std::string scope;
    std::string node;
    Json::json_pointer field;
    // Into the document the list was taken from; valid while it is unchanged.
    const Json *referrer = nullptr;
};

std::vector<DefinitionReference> definitionReferences(const Json &document,
                                                      const std::string &definition) {
    std::vector<DefinitionReference> references;
    visitDefinitionReferences(document, definition,
        [&](const std::string &scope, const Json &node, const Json::json_pointer &field) {
            references.push_back({scope, node.value("id", std::string{}), field, &node});
        });
    return references;
}

// Every reference to this definition that supplies this boundary port -- with
// a stored parameter or with an edge. A `graph.instance` supplies ports by the
// definition's input names from its `parameters` and `inputs`; a `graph.loop`
// (LP-1a) from its `inputs` only (its parameters are the loop's own; LP-1b
// adds `carry`), and it supplies every branch body alike.
void requireInputUnreferenced(const Json &candidate, const std::string &definition,
                              const std::string &input) {
    for (const auto &reference : definitionReferences(candidate, definition)) {
        const auto &node = *reference.referrer;
        const auto op = node.value("op", std::string{});
        if (op != "graph.instance" && op != "graph.loop") continue;
        const auto supplies = [&](const char *key) {
            const auto found = node.find(key);
            return found != node.end() && found->is_object() && found->contains(input);
        };
        if ((op == "graph.loop" || !supplies("parameters")) && !supplies("inputs")) continue;
        throw GraphRegistryError("graph.input_referenced@1",
            {{"definition", definition}, {"input", input}, {"instance", reference.node}});
    }
}

std::string opNodeName(const Json &value) {
    if (!value.is_string() || value.get_ref<const std::string &>().empty())
        throw std::invalid_argument("graph document op requires a nonempty node id");
    return value.get<std::string>();
}

// --- set_layout. Nothing here is read by the engine; what is checked is that
// every name the placement uses is one the addressed scope really holds. A
// layout that points at a node or an edge that is not there would come back to
// an editor as a drawing of a graph that does not exist, and the editor cannot
// tell that apart from its own bug.

GraphRegistryError layoutInvalid(const char *reason, Json detail) {
    detail["reason"] = reason;
    return GraphRegistryError("graph.layout_invalid@1", std::move(detail));
}

// The node one layout entry names, found under exactly the rules every other op
// is bound by: an instance path belongs to no single body, and a node that lives
// in a definition is not reachable from the root scope.
const Json &locateLayoutNode(const EditScope &scope, const Json &definitions,
                             const std::string &id) {
    if (id.find('/') != std::string::npos)
        throw GraphRegistryError("graph.definition_immutable@1",
            {{"node", id}, {"reason", "instance_path"}});
    for (const auto &node : *scope.nodes)
        if (node.contains("id") && node.at("id") == id) return node;
    if (scope.root)
        for (const auto &[definition, body] : definitions.items())
            for (const auto &node : body.value("nodes", Json::array()))
                if (node.value("id", std::string{}) == id)
                    throw GraphRegistryError("graph.definition_immutable@1",
                        {{"node", id}, {"definition", definition}});
    throw layoutInvalid("unknown_node",
        {{"node", id}, {"scope", scope.root ? std::string{kRootScope} : scope.name}});
}

double layoutCoordinate(const Json &value, const char *what) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw layoutInvalid("coordinate", {{"member", what}});
    return value.get<double>();
}

void requireLayoutPoint(const Json &point) {
    if (!point.is_array() || point.size() != 2)
        throw layoutInvalid("point", {{"point", point}});
    (void)layoutCoordinate(point.at(0), "x");
    (void)layoutCoordinate(point.at(1), "y");
}

// An edge is named by the input end it lands on, which is unique: one port of
// one node takes at most one edge. So the check is that the port is one this
// node really has an edge into.
void requireLayoutEdge(const EditScope &scope, const Json &definitions,
                       const Json &edge) {
    object(edge, "set_layout edge must be an object");
    if (edge.size() != 2 || !edge.contains("node") || !edge.contains("port"))
        throw std::invalid_argument("set_layout edge requires exactly node and port");
    const auto id = opNodeName(edge.at("node"));
    const auto port = identifier(edge.at("port"));
    const Json &node = locateLayoutNode(scope, definitions, id);
    const auto inputs = node.find("inputs");
    if (inputs == node.end() || !inputs->is_object() || !inputs->contains(port))
        throw layoutInvalid("unknown_edge", {{"node", id}, {"port", port}});
}

// N-8. The Frame Plan canvas draws passes and resources, and neither of those
// is a node of this document: a pass name comes from the rendering preset, a
// feature, or an authored fragment, and it can be renamed there without this
// document hearing about it. So this member is checked for SHAPE only --
// finite coordinates, objects where objects are expected -- and no name in it
// is looked up. `locateLayoutNode` deliberately never runs over it: a pass the
// document cannot see is not a broken placement, it is a placement about
// something else.
void requireFramePlanEdge(const Json &edge) {
    object(edge, "set_layout frame_plan edge must be an object");
    if (edge.size() != 2 || !edge.contains("node") || !edge.contains("resource"))
        throw layoutInvalid("frame_plan_edge", {{"edge", edge}});
    for (const auto *member : {"node", "resource"})
        if (!edge.at(member).is_string() ||
            edge.at(member).get_ref<const std::string &>().empty())
            throw layoutInvalid("frame_plan_edge", {{"member", member}});
}

void requireFramePlanLayout(const Json &frame_plan) {
    object(frame_plan, "set_layout frame_plan must be an object");
    for (const auto &[graph, placement] : frame_plan.items()) {
        object(placement, "set_layout frame_plan graph must be an object");
        for (const auto &[key, value] : placement.items()) {
            (void)value;
            if (key != "nodes" && key != "reroutes" && key != "ties" &&
                key != "groups")
                throw layoutInvalid("frame_plan_unknown_key",
                                    {{"graph", graph}, {"key", key}});
        }
        if (const auto nodes = placement.find("nodes"); nodes != placement.end()) {
            object(*nodes, "set_layout frame_plan nodes must be an object");
            for (const auto &[name, place] : nodes->items()) {
                if (!place.is_object() || !place.contains("x") ||
                    !place.contains("y"))
                    throw layoutInvalid("frame_plan_node_placement",
                                        {{"graph", graph}, {"node", name}});
                (void)layoutCoordinate(place.at("x"), "x");
                (void)layoutCoordinate(place.at("y"), "y");
            }
        }
        if (const auto reroutes = placement.find("reroutes");
            reroutes != placement.end()) {
            if (!reroutes->is_array())
                throw std::invalid_argument(
                    "set_layout frame_plan reroutes must be an array");
            for (const auto &entry : *reroutes) {
                object(entry, "set_layout frame_plan reroute must be an object");
                if (!entry.contains("edge") || !entry.contains("points"))
                    throw layoutInvalid("frame_plan_reroute",
                                        {{"graph", graph}, {"reroute", entry}});
                requireFramePlanEdge(entry.at("edge"));
                if (!entry.at("points").is_array())
                    throw std::invalid_argument(
                        "set_layout frame_plan reroute points must be an array");
                for (const auto &point : entry.at("points"))
                    requireLayoutPoint(point);
            }
        }
        if (const auto ties = placement.find("ties"); ties != placement.end()) {
            if (!ties->is_array())
                throw std::invalid_argument(
                    "set_layout frame_plan ties must be an array");
            for (const auto &tie : *ties) {
                object(tie, "set_layout frame_plan tie must be an object");
                if (!tie.contains("x") || !tie.contains("y") ||
                    !tie.contains("edges"))
                    throw layoutInvalid("frame_plan_tie",
                                        {{"graph", graph}, {"tie", tie}});
                (void)layoutCoordinate(tie.at("x"), "x");
                (void)layoutCoordinate(tie.at("y"), "y");
                if (!tie.at("edges").is_array())
                    throw std::invalid_argument(
                        "set_layout frame_plan tie edges must be an array");
                for (const auto &edge : tie.at("edges"))
                    requireFramePlanEdge(edge);
            }
        }
        if (const auto groups = placement.find("groups"); groups != placement.end()) {
            if (!groups->is_array())
                throw std::invalid_argument(
                    "set_layout frame_plan groups must be an array");
            for (const auto &group : *groups) {
                object(group, "set_layout frame_plan group must be an object");
                if (!group.contains("id") || !group.at("id").is_string() ||
                    !group.contains("members") || !group.at("members").is_array())
                    throw layoutInvalid("frame_plan_group",
                                        {{"graph", graph}, {"group", group}});
                for (const auto &member : group.at("members"))
                    if (!member.is_string())
                        throw layoutInvalid("frame_plan_group",
                                            {{"graph", graph}, {"member", member}});
            }
        }
    }
}

void requireLayout(const EditScope &scope, const Json &definitions, const Json &layout) {
    object(layout, "set_layout requires a layout object or null");
    for (const auto &[key, value] : layout.items()) {
        (void)value;
        if (key != "nodes" && key != "reroutes" && key != "ties" &&
            key != "frame_plan")
            throw layoutInvalid("unknown_key", {{"key", key}});
    }
    if (const auto frame_plan = layout.find("frame_plan");
        frame_plan != layout.end())
        requireFramePlanLayout(*frame_plan);
    if (const auto nodes = layout.find("nodes"); nodes != layout.end()) {
        object(*nodes, "set_layout nodes must be an object");
        for (const auto &[id, place] : nodes->items()) {
            (void)locateLayoutNode(scope, definitions, id);
            if (!place.is_object() || !place.contains("x") || !place.contains("y"))
                throw layoutInvalid("node_placement", {{"node", id}});
            (void)layoutCoordinate(place.at("x"), "x");
            (void)layoutCoordinate(place.at("y"), "y");
        }
    }
    if (const auto reroutes = layout.find("reroutes"); reroutes != layout.end()) {
        if (!reroutes->is_array())
            throw std::invalid_argument("set_layout reroutes must be an array");
        for (const auto &entry : *reroutes) {
            object(entry, "set_layout reroute must be an object");
            if (!entry.contains("edge") || !entry.contains("points"))
                throw layoutInvalid("reroute", {{"reroute", entry}});
            requireLayoutEdge(scope, definitions, entry.at("edge"));
            const auto &points = entry.at("points");
            if (!points.is_array())
                throw std::invalid_argument("set_layout reroute points must be an array");
            for (const auto &point : points) requireLayoutPoint(point);
        }
    }
    if (const auto ties = layout.find("ties"); ties != layout.end()) {
        if (!ties->is_array()) throw std::invalid_argument("set_layout ties must be an array");
        for (const auto &tie : *ties) {
            object(tie, "set_layout tie must be an object");
            if (!tie.contains("x") || !tie.contains("y") || !tie.contains("edges"))
                throw layoutInvalid("tie", {{"tie", tie}});
            (void)layoutCoordinate(tie.at("x"), "x");
            (void)layoutCoordinate(tie.at("y"), "y");
            const auto &edges = tie.at("edges");
            if (!edges.is_array())
                throw std::invalid_argument("set_layout tie edges must be an array");
            for (const auto &edge : edges) requireLayoutEdge(scope, definitions, edge);
        }
    }
}

// Topology and its placement are one edit. Leaving stale placement for the
// canvas to repair would create a second history entry whose inverse refers
// to an already removed node or edge and therefore cannot pass set_layout.
void pruneScopeLayout(const EditScope &scope) {
    const auto found = scope.container->find("layout");
    if (found == scope.container->end() || !found->is_object()) return;
    auto &layout = *found;
    std::set<std::string> present;
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto &node : *scope.nodes) {
        const auto id = node.at("id").get<std::string>();
        present.insert(id);
        if (const auto inputs = node.find("inputs");
            inputs != node.end() && inputs->is_object())
            for (const auto &[port, endpoint] : inputs->items()) {
                (void)endpoint;
                edges.emplace(id, port);
            }
    }
    const auto liveEdge = [&](const Json &edge) {
        return edge.is_object() && edge.contains("node") && edge.at("node").is_string() &&
            edge.contains("port") && edge.at("port").is_string() &&
            edges.contains({edge.at("node").get<std::string>(), edge.at("port").get<std::string>()});
    };
    if (const auto nodes = layout.find("nodes"); nodes != layout.end() && nodes->is_object()) {
        for (auto entry = nodes->begin(); entry != nodes->end();)
            if (!present.contains(entry.key())) entry = nodes->erase(entry);
            else ++entry;
    }
    if (const auto reroutes = layout.find("reroutes");
        reroutes != layout.end() && reroutes->is_array())
        reroutes->erase(std::remove_if(reroutes->begin(), reroutes->end(), [&](const Json &entry) {
            return entry.is_object() && entry.contains("edge") &&
                (!liveEdge(entry.at("edge")) ||
                 (entry.contains("points") && entry.at("points").empty()));
        }), reroutes->end());
    if (const auto ties = layout.find("ties"); ties != layout.end() && ties->is_array()) {
        for (auto &tie : *ties) {
            if (!tie.is_object() || !tie.contains("edges") || !tie.at("edges").is_array()) continue;
            auto &members = tie["edges"];
            members.erase(std::remove_if(members.begin(), members.end(), [&](const Json &edge) {
                return !liveEdge(edge);
            }), members.end());
            if (members.size() != 1 || !tie.contains("x") || !tie.at("x").is_number() ||
                !tie.contains("y") || !tie.at("y").is_number()) continue;
            // Match the canvas: a surviving line keeps the former bundle's
            // control point, ordered by x with its existing reroute points.
            if (!layout.contains("reroutes")) layout["reroutes"] = Json::array();
            auto &reroutes = layout["reroutes"];
            if (!reroutes.is_array()) continue;
            auto entry = std::find_if(reroutes.begin(), reroutes.end(), [&](const Json &reroute) {
                return reroute.is_object() && reroute.value("edge", Json{}) == members.front();
            });
            if (entry == reroutes.end()) {
                reroutes.push_back({{"edge", members.front()}, {"points", Json::array()}});
                entry = std::prev(reroutes.end());
            }
            auto &points = (*entry)["points"];
            if (!points.is_array()) continue;
            points.push_back(Json::array({tie.at("x"), tie.at("y")}));
            std::stable_sort(points.begin(), points.end(), [](const Json &a, const Json &b) {
                return a.at(0).get<double>() < b.at(0).get<double>();
            });
            members.clear();
        }
        ties->erase(std::remove_if(ties->begin(), ties->end(), [](const Json &tie) {
            return tie.is_object() && tie.contains("edges") && tie.at("edges").empty();
        }), ties->end());
    }
}

// The definitions whose boundary a structural node's ports are: an instance's
// one definition, or every branch body of a graph.loop (LP-1a).
std::vector<std::string> suppliedDefinitions(const Json &node) {
    std::vector<std::string> names;
    const auto op = node.value("op", std::string{});
    if (op == "graph.instance") {
        names.push_back(node.value("definition", std::string{}));
    } else if (op == "graph.loop") {
        const auto parameters = node.value("parameters", Json::object());
        for (const auto &branch : parameters.value("branches", Json::array()))
            if (branch.is_object() && branch.contains("body") && branch.at("body").is_string())
                names.push_back(branch.at("body").get<std::string>());
    }
    return names;
}

bool isStructuralSupplier(const Json &node) {
    const auto op = node.value("op", std::string{});
    return op == "graph.instance" || op == "graph.loop";
}

void requireInstancePort(const Json &node, const Json &definitions, const std::string &port) {
    for (const auto &definition : suppliedDefinitions(node)) {
        if (!definitions.contains(definition)) continue;
        std::set<std::string> declared;
        for (const auto &entry : definitions.at(definition).value("nodes", Json::array()))
            if (entry.value("op", std::string{}) == "graph.input")
                declared.insert(entry.value("id", std::string{}));
        // A known definition that declares no input at all is no exception:
        // the port is still one it does not have.
        if (declared.contains(port)) continue;
        throw GraphRegistryError("graph.input_unknown@1",
            {{"node", node.at("id")}, {"definition", definition}, {"port", port}});
    }
}

// --- Checks on the whole document, run once after the last op. LP-0c.
//
// An instance's ports and its definition's boundary are two bodies, and an
// editor changes them with two ops: "add a graph.input to the definition" and
// "supply it on the instance", in either order, or the removal of both. The
// agreement between the two -- and whether a removed definition is still named
// -- is therefore checked on the candidate the whole list produced, not after
// each op. Each op records what it would have checked; if the op itself then
// fails for another reason, its own records are checked first, so an op that
// is refused still reports the same code it always did.
struct DeferredCheck {
    enum class Kind : std::uint8_t {
        // `port` of instance `node` in `scope` is a graph.input of its
        // definition (set_input / set_node_parameters).
        instance_port,
        // No reference to `definition` still supplies its removed graph.input
        // `port` (remove_node of a graph.input), unless it was declared again.
        input_unreferenced,
        // Nothing names the removed `definition` any more (remove_definition),
        // unless a definition of that name is there again.
        definition_unreferenced,
        // Nothing outside `definition` reads its removed public output `port`
        // (set_outputs, DO-1), unless it was declared again.
        output_unreferenced,
    };
    Kind kind;
    // instance_port: the scope holding the instance, empty for the root.
    std::string scope;
    std::string node;
    std::string definition;
    std::string port;
    // instance_port recorded by a `set_input` that clears the port, when the
    // port was the definition's at that point: declared then, or declared at
    // the start of the list and removed by an earlier op. Clearing such a port
    // is how a supply is taken away, so it holds even if the port is gone by
    // the end -- but only if nothing supplies it again.
    bool cleared_known_port = false;
};

const Json *findScopeNode(const Json &candidate, const std::string &scope,
                          const std::string &id) {
    const Json *nodes = nullptr;
    if (scope.empty()) {
        if (const auto found = candidate.find("nodes"); found != candidate.end()) nodes = &*found;
    } else if (const auto definitions = candidate.find("definitions");
               definitions != candidate.end() && definitions->is_object() &&
               definitions->contains(scope) && definitions->at(scope).is_object()) {
        const auto &body = definitions->at(scope);
        if (const auto found = body.find("nodes"); found != body.end()) nodes = &*found;
    }
    if (nodes == nullptr || !nodes->is_array()) return nullptr;
    for (const auto &node : *nodes)
        if (node.is_object() && node.value("id", std::string{}) == id) return &node;
    return nullptr;
}

bool declaresInput(const Json &candidate, const std::string &definition,
                   const std::string &input) {
    const auto definitions = candidate.find("definitions");
    if (definitions == candidate.end() || !definitions->is_object() ||
        !definitions->contains(definition))
        return false;
    for (const auto &node : definitions->at(definition).value("nodes", Json::array()))
        if (node.value("op", std::string{}) == "graph.input" &&
            node.value("id", std::string{}) == input)
            return true;
    return false;
}

bool declaresOutput(const Json &candidate, const std::string &definition,
                    const std::string &output) {
    const auto definitions = candidate.find("definitions");
    if (definitions == candidate.end() || !definitions->is_object() ||
        !definitions->contains(definition))
        return false;
    const auto &body = definitions->at(definition);
    const auto outputs = body.find("outputs");
    return outputs != body.end() && outputs->is_object() && outputs->contains(output);
}

// The object a scope's `nodes` and `outputs` live on: the document for the
// root (the empty name), else that definition's body.
const Json *findScopeBody(const Json &candidate, const std::string &scope) {
    if (scope.empty()) return &candidate;
    const auto definitions = candidate.find("definitions");
    if (definitions == candidate.end() || !definitions->is_object() ||
        !definitions->contains(scope) || !definitions->at(scope).is_object())
        return nullptr;
    return &definitions->at(scope);
}

// DO-1: nothing outside `definition` reads its public output `output` any more.
// A graph.instance of it offers the output as its port of that name; a
// graph.loop over it (every branch body alike) as `p` and `p[]`, which the loop
// declares in its `outputs` and may carry from. An edge into any node of the
// referrer's scope, or that scope's own `outputs`, reads it by that port -- an
// edge with an `element` reads `p[]`. A loop whose other body still has the
// output is not asked: its bodies disagree, and the expansion names that
// (graph.loop_branch_interface@1).
void requireOutputUnreferenced(const Json &candidate, const std::string &definition,
                               const std::string &output) {
    for (const auto &reference : definitionReferences(candidate, definition)) {
        const auto &node = *reference.referrer;
        const auto op = node.value("op", std::string{});
        const bool loop = op == "graph.loop";
        if (!loop && op != "graph.instance") continue;
        if (loop) {
            const auto bodies = suppliedDefinitions(node);
            if (std::any_of(bodies.begin(), bodies.end(), [&](const std::string &body) {
                    return declaresOutput(candidate, body, output);
                }))
                continue;
        }
        const auto refuse = [&](Json detail) {
            detail["definition"] = definition;
            detail["output"] = output;
            detail["node"] = reference.node;
            throw GraphRegistryError("graph.output_referenced@1", std::move(detail));
        };
        const auto reads = [&](const Json &endpoint) {
            if (!endpoint.is_object() || endpoint.value("node", std::string{}) != reference.node)
                return false;
            const auto port = endpoint.value("port", std::string{});
            return port == output || (loop && port == output + "[]");
        };
        if (loop) {
            for (const auto &declared : node.value("outputs", Json::array()))
                if (declared == output || declared == output + "[]") refuse({{"field", "outputs"}});
            const auto parameters = node.value("parameters", Json::object());
            const auto carry = parameters.is_object() ? parameters.value("carry", Json::array()) : Json::array();
            if (carry.is_array())
                for (const auto &pair : carry)
                    if (pair.is_object() && pair.value("from", std::string{}) == output)
                        refuse({{"field", "carry"}});
        }
        const auto *scope = findScopeBody(candidate, reference.scope);
        if (scope == nullptr) continue;
        if (const auto nodes = scope->find("nodes"); nodes != scope->end() && nodes->is_array())
            for (const auto &consumer : *nodes) {
                if (!consumer.is_object()) continue;
                const auto inputs = consumer.find("inputs");
                if (inputs == consumer.end() || !inputs->is_object()) continue;
                for (const auto &[port, endpoint] : inputs->items())
                    if (reads(endpoint))
                        refuse({{"consumer", consumer.value("id", std::string{})}, {"port", port}});
            }
        if (const auto outputs = scope->find("outputs"); outputs != scope->end() && outputs->is_object())
            for (const auto &[port, endpoint] : outputs->items())
                if (reads(endpoint)) refuse({{"scope_output", port}});
    }
}

// `in_place` is the run for an op that failed on its own: the check is asked
// of the document as that op found it, where the removed port is still there.
void runDeferredCheck(const Json &candidate, const DeferredCheck &check, bool in_place = false) {
    static const Json no_definitions = Json::object();
    const auto definitions = candidate.find("definitions");
    const auto &known = definitions == candidate.end() || !definitions->is_object()
        ? no_definitions : *definitions;
    switch (check.kind) {
    case DeferredCheck::Kind::instance_port: {
        // An instance a later op removed supplies nothing.
        const auto *node = findScopeNode(candidate, check.scope, check.node);
        if (node == nullptr) return;
        const auto supplies = [&](const char *key) {
            const auto found = node->find(key);
            return found != node->end() && found->is_object() && found->contains(check.port);
        };
        if (check.cleared_known_port && !supplies("inputs") && !supplies("parameters")) return;
        requireInstancePort(*node, known, check.port);
        return;
    }
    case DeferredCheck::Kind::input_unreferenced:
        if (!in_place && (!known.contains(check.definition) ||
                          declaresInput(candidate, check.definition, check.port)))
            return;
        requireInputUnreferenced(candidate, check.definition, check.port);
        return;
    case DeferredCheck::Kind::output_unreferenced:
        if (!in_place && (!known.contains(check.definition) ||
                          declaresOutput(candidate, check.definition, check.port)))
            return;
        requireOutputUnreferenced(candidate, check.definition, check.port);
        return;
    case DeferredCheck::Kind::definition_unreferenced: {
        if (known.contains(check.definition)) return;
        auto referenced_by = Json::array();
        for (const auto &reference : definitionReferences(candidate, check.definition))
            if (std::find(referenced_by.begin(), referenced_by.end(), reference.node) ==
                referenced_by.end())
                referenced_by.push_back(reference.node);
        if (referenced_by.empty()) return;
        throw GraphRegistryError("graph.definition_referenced@1",
            {{"definition", check.definition}, {"referenced_by", std::move(referenced_by)}});
    }
    }
}

// --- add_definition / remove_definition / rename_definition. LP-0c.
//
// A definition's name follows the node id rule: a nonempty string without a
// slash. "root" is refused as well, because that token names the root document
// in an op's `scope`: a definition called that could never be edited.

bool isDefinitionOp(const std::string &kind) {
    return kind == "add_definition" || kind == "remove_definition" ||
           kind == "rename_definition";
}

std::string definitionName(const Json &operation, const char *member) {
    const auto found = operation.find(member);
    if (found == operation.end())
        throw std::invalid_argument(std::string{"graph definition op requires "} + member);
    return identifier(*found);
}

void requireNewDefinitionName(const std::string &name) {
    if (name == kRootScope)
        throw std::invalid_argument(
            "graph definition cannot be named \"root\": that scope names the root document");
}

// The same rules add_node applies inside a definition scope, so a body that
// arrives whole cannot hold what could not have been added one node at a time.
void requireDefinitionBody(const std::string &name, const Json &body) {
    object(body, "add_definition definition must be an object");
    const auto nodes = body.find("nodes");
    if (nodes == body.end() || !nodes->is_array())
        throw std::invalid_argument("graph definition requires a nodes array: " + name);
    const auto outputs = body.find("outputs");
    if (outputs == body.end() || !outputs->is_object())
        throw std::invalid_argument("graph definition requires an outputs object: " + name);
    std::set<std::string> ids;
    for (const auto &node : *nodes) {
        object(node, "graph definition node must be an object");
        const auto id = opNodeName(node.value("id", Json{}));
        if (id.find('/') != std::string::npos)
            throw GraphRegistryError("graph.definition_immutable@1",
                {{"node", id}, {"reason", "instance_path"}});
        if (!ids.insert(id).second)
            throw GraphRegistryError("graph.node_duplicate@1", {{"node", id}, {"scope", name}});
        if (node.value("op", std::string{}) == "graph.instance")
            throw GraphRegistryError("graph.nested_instance@1", {{"node", id}, {"scope", name}});
        if (node.value("op", std::string{}) == "graph.loop")
            throw GraphRegistryError("graph.loop_nested@1", {{"node", id}, {"definition", name}});
    }
}

void applyDefinitionOp(Json &candidate, const Json &operation, const std::string &kind,
                       std::vector<DeferredCheck> &checks,
                       std::vector<DeferredCheck> &recorded) {
    // A definition op acts on the document, not inside one body.
    if (const auto scope = operation.find("scope");
        scope != operation.end() && !scope->is_null() && *scope != kRootScope)
        throw std::invalid_argument("graph definition ops take no scope: " + kind);
    const auto definitions = candidate.find("definitions");
    const bool present = definitions != candidate.end();
    const auto exists = [&](const std::string &name) {
        return present && definitions->contains(name);
    };
    if (kind == "add_definition") {
        const auto name = definitionName(operation, "name");
        requireNewDefinitionName(name);
        if (exists(name))
            throw GraphRegistryError("graph.definition_duplicate@1", {{"definition", name}});
        Json body{{"nodes", Json::array()}, {"outputs", Json::object()}};
        if (const auto found = operation.find("definition");
            found != operation.end() && !found->is_null()) {
            requireDefinitionBody(name, *found);
            body = *found;
        }
        candidate["definitions"][name] = std::move(body);
    } else if (kind == "remove_definition") {
        const auto name = definitionName(operation, "name");
        if (!exists(name))
            throw GraphRegistryError("graph.definition_missing@1", {{"definition", name}});
        definitions->erase(name);
        checks.push_back({DeferredCheck::Kind::definition_unreferenced, {}, {}, name, {}});
    } else {
        const auto from = definitionName(operation, "from");
        const auto to = definitionName(operation, "to");
        if (!exists(from))
            throw GraphRegistryError("graph.definition_missing@1", {{"definition", from}});
        requireNewDefinitionName(to);
        if (exists(to))
            throw GraphRegistryError("graph.definition_duplicate@1", {{"definition", to}});
        auto body = std::move(definitions->at(from));
        definitions->erase(from);
        (*definitions)[to] = std::move(body);
        // Every name moves with the body in this same op, so the document is
        // never left naming a definition that is not there.
        visitDefinitionReferences(candidate, from,
            [&](const std::string &, Json &node, const Json::json_pointer &field) {
                node[field] = to;
            });
        // And what earlier ops asked to be checked follows it too.
        for (auto &check : recorded) {
            if (check.scope == from) check.scope = to;
            if (check.definition == from) check.definition = to;
        }
    }
}

void applyOperation(Json &candidate, const Json &operation, const std::string &kind,
                    std::vector<DeferredCheck> &checks, std::vector<DeferredCheck> &recorded) {
    if (isDefinitionOp(kind)) {
        applyDefinitionOp(candidate, operation, kind, checks, recorded);
        return;
    }
    // The definition bodies are read fresh for every op: one op in the list can
    // change what the next one is allowed to do.
    static const Json no_definitions = Json::object();
    const auto definitionsOf = [&]() -> const Json & {
        const auto found = candidate.find("definitions");
        return found == candidate.end() ? no_definitions : *found;
    };
    {
        const auto scope = resolveEditScope(candidate, operation);
        auto &nodes = *scope.nodes;
        const auto &definitions = definitionsOf();
        if (kind == "add_node" || kind == "replace_node") {
            const auto &node = operation.at("node");
            object(node, "graph node edit requires a node object");
            const auto id = opNodeName(node.at("id"));
            if (id.find('/') != std::string::npos)
                throw GraphRegistryError("graph.definition_immutable@1",
                    {{"node", id}, {"reason", "instance_path"}});
            const auto op = node.value("op", std::string{});
            // The two structural operations belong to one side each: an
            // instance stands in the root document, a boundary port inside a
            // definition. Nesting one in the wrong body is refused by name.
            if (op == "graph.instance" && !scope.root)
                throw GraphRegistryError("graph.nested_instance@1",
                    {{"node", id}, {"scope", scope.name}});
            // LP-1a: a loop stands in the root document only.
            if (op == "graph.loop" && !scope.root)
                throw GraphRegistryError("graph.loop_nested@1",
                    {{"node", id}, {"definition", scope.name}});
            if (op == "graph.input" && scope.root)
                throw GraphRegistryError("graph.input_scope@1", {{"node", id}});
            if (kind == "replace_node") {
                // Restoring a node preserves all authored fields, including
                // absent parameters and extension fields. The full candidate
                // is validated below just like an ordinary graph edit.
                locateNode(scope, definitions, id) = node;
                pruneScopeLayout(scope);
                return;
            }
            if (std::any_of(nodes.begin(), nodes.end(), [&](const Json &existing) {
                    return existing.contains("id") && existing.at("id") == id;
                }))
                throw GraphRegistryError("graph.node_duplicate@1", {{"node", id}});
            if (const auto index = operation.find("index"); index != operation.end()) {
                if ((!index->is_number_unsigned() && !index->is_number_integer()) ||
                    (index->is_number_integer() && index->get<std::int64_t>() < 0) ||
                    index->get<std::uint64_t>() > nodes.size())
                    throw std::invalid_argument("add_node index must be within the node array");
                nodes.insert(nodes.begin() + index->get<Json::difference_type>(), node);
            } else {
                nodes.push_back(node);
            }
        } else if (kind == "remove_node") {
            const auto id = opNodeName(operation.at("node"));
            const auto removed_op = locateNode(scope, definitions, id).value("op", std::string{});
            // A boundary port an instance is still supplying is that instance's
            // port. It is named first because it is the reference the canvas
            // cannot show: the value lives outside the body being edited. The
            // instance side may be cleared later in the list, so the check is
            // recorded, and runs here only if this op fails anyway.
            if (!scope.root && removed_op == "graph.input")
                checks.push_back({DeferredCheck::Kind::input_unreferenced, {}, {},
                                  scope.name, id});
            // items() borrows its container, so the object has to outlive the
            // loop rather than be a temporary from value().
            const auto outputs =
                scope.outputs != nullptr && scope.outputs->is_object() ? *scope.outputs : Json::object();
            for (const auto &[port, endpoint] : outputs.items())
                if (endpoint.is_object() && endpoint.value("node", std::string{}) == id)
                    throw GraphRegistryError("graph.output_referenced@1",
                        {{"node", id}, {"output", port}});
            for (const auto &node : nodes) {
                if (!node.contains("id") || node.at("id") == id) continue;
                const auto inputs = node.value("inputs", Json::object());
                for (const auto &[port, endpoint] : inputs.items())
                    if (endpoint.is_object() && endpoint.value("node", std::string{}) == id)
                        throw GraphRegistryError("graph.output_referenced@1",
                            {{"node", id}, {"consumer", node.at("id")}, {"port", port}});
            }
            nodes.erase(std::find_if(nodes.begin(), nodes.end(), [&](const Json &node) {
                return node.contains("id") && node.at("id") == id;
            }));
            pruneScopeLayout(scope);
        } else if (kind == "set_input") {
            const auto id = opNodeName(operation.at("node"));
            const auto port = identifier(operation.at("port"));
            auto &node = locateNode(scope, definitions, id);
            const auto &source = operation.at("source");
            // The port may be declared by a later op in the list. A graph.loop's
            // inputs are its branch bodies' ports, like an instance's (LP-1a).
            if (isStructuralSupplier(node)) {
                DeferredCheck check{DeferredCheck::Kind::instance_port,
                                    scope.root ? std::string{} : scope.name, id, {}, port};
                if (source.is_null()) {
                    for (const auto &reference : suppliedDefinitions(node))
                        check.cleared_known_port = check.cleared_known_port ||
                            declaresInput(candidate, reference, port) ||
                            std::any_of(recorded.begin(), recorded.end(), [&](const DeferredCheck &earlier) {
                                return earlier.kind == DeferredCheck::Kind::input_unreferenced &&
                                       earlier.definition == reference && earlier.port == port;
                            });
                }
                checks.push_back(std::move(check));
            }
            if (source.is_null()) {
                if (const auto inputs = node.find("inputs"); inputs != node.end()) {
                    inputs->erase(port);
                    if (inputs->empty()) node.erase("inputs");
                }
            } else {
                object(source, "set_input source must be an object or null");
                // LP-1b: an edge may name one element of a loop's `p[]`; the
                // expansion checks what it names and its expression.
                if (source.size() != (source.contains("element") ? 3u : 2u) || !source.contains("node") ||
                    !source.contains("port"))
                    throw std::invalid_argument("set_input source requires node and port, and at most element");
                (void)identifier(source.at("node"));
                (void)identifier(source.at("port"));
                node["inputs"][port] = source;
            }
            pruneScopeLayout(scope);
        } else if (kind == "set_node_parameters") {
            const auto id = opNodeName(operation.at("node"));
            const auto &parameters = operation.at("parameters");
            object(parameters, "set_node_parameters requires a parameters object");
            auto &node = locateNode(scope, definitions, id);
            if (node.value("op", std::string{}) == "graph.instance")
                for (const auto &[name, value] : parameters.items()) {
                    (void)value;
                    checks.push_back({DeferredCheck::Kind::instance_port,
                                      scope.root ? std::string{} : scope.name, id, {}, name});
                }
            if (parameters.empty()) node.erase("parameters");
            else node["parameters"] = parameters;
        } else if (kind == "set_node_label") {
            // The authored, human-chosen name. It is not an identifier: nothing
            // refers to it, so clearing it is spelled as the empty string
            // rather than as a separate op.
            const auto id = opNodeName(operation.at("node"));
            const auto &label = operation.at("label");
            if (!label.is_string())
                throw std::invalid_argument("set_node_label requires a string label");
            auto &node = locateNode(scope, definitions, id);
            if (label.get_ref<const std::string &>().empty()) node.erase("label");
            else node["label"] = label;
        } else if (kind == "set_outputs") {
            // DO-1: the scope's public outputs, replaced whole. Its inverse is
            // the same op carrying the outputs the scope held before. Each
            // endpoint names a node of this scope as it stands at this op, the
            // way remove_node's check of the outputs reads them; whether the
            // port of an instance or a loop exists is the expansion's to say,
            // since a later op may change what that node offers.
            const auto found = operation.find("outputs");
            if (found == operation.end())
                throw std::invalid_argument("set_outputs requires an outputs object");
            const auto &outputs = *found;
            object(outputs, "set_outputs requires an outputs object");
            if (scope.container == nullptr)
                throw std::invalid_argument("graph document scope has no container");
            for (const auto &[port, endpoint] : outputs.items()) {
                // A port name follows the node id rule; `[]` at its end is how
                // a loop spells a body output per element, so a body output
                // may not end with it.
                (void)identifier(Json(port));
                if (port.ends_with("[]"))
                    throw std::invalid_argument("graph output name cannot end with []: " + port);
                object(endpoint, "set_outputs endpoint must be an object");
                if (endpoint.size() != 2 || !endpoint.contains("node") || !endpoint.contains("port"))
                    throw std::invalid_argument("set_outputs endpoint requires exactly node and port: " + port);
                const auto id = opNodeName(endpoint.at("node"));
                const auto source_port = identifier(endpoint.at("port"));
                const auto &source = locateNode(scope, definitions, id);
                const auto op = source.value("op", std::string{});
                if (op == "graph.input") {
                    if (source_port != "value")
                        throw std::invalid_argument("graph input output port must be value");
                } else if (!isStructuralSupplier(source)) {
                    const auto ports = source.value("outputs", Json::array());
                    if (!ports.is_array() || std::find(ports.begin(), ports.end(), source_port) == ports.end())
                        throw std::invalid_argument("unknown graph output port: " + id + "." + source_port);
                }
            }
            const auto previous = scope.outputs != nullptr && scope.outputs->is_object()
                ? *scope.outputs : Json::object();
            (*scope.container)["outputs"] = outputs;
            // An output that is gone may still be read from outside the body;
            // the reader may be rewired later in the list.
            if (!scope.root)
                for (const auto &[port, endpoint] : previous.items()) {
                    (void)endpoint;
                    if (!outputs.contains(port))
                        checks.push_back({DeferredCheck::Kind::output_unreferenced, {}, {},
                                          scope.name, port});
                }
        } else if (kind == "set_layout") {
            // Where the editor put things. The engine carries it and reads
            // none of it, so this op touches nothing an evaluation depends on:
            // it replaces one member beside `nodes` and leaves the rest of the
            // scope exactly as it was.
            const auto &layout = operation.at("layout");
            if (scope.container == nullptr)
                throw std::invalid_argument("graph document scope has no container");
            if (layout.is_null()) {
                scope.container->erase("layout");
            } else {
                requireLayout(scope, definitions, layout);
                (*scope.container)["layout"] = layout;
            }
        } else {
            throw std::invalid_argument("unknown graph document op: " + kind);
        }
    }
}
} // namespace

nlohmann::json editGraphDocument(const Json &document, const Json &ops) {
    if (!ops.is_array()) throw std::invalid_argument("graph document ops must be an array");
    auto candidate = document;
    if (!candidate.at("nodes").is_array())
        throw std::invalid_argument("graph nodes must be an array");
    if (const auto found = candidate.find("definitions"); found != candidate.end())
        object(*found, "graph definitions must be an object");
    // What the ops leave for the end: see DeferredCheck.
    std::vector<DeferredCheck> deferred;
    for (const auto &operation : ops) {
        object(operation, "graph document op must be an object");
        const auto kind = operation.value("op", std::string{});
        std::vector<DeferredCheck> checks;
        try {
            applyOperation(candidate, operation, kind, checks, deferred);
        } catch (...) {
            // An op refused for its own reasons reports the whole-document
            // check it recorded first, as it did when that check ran in place.
            for (const auto &check : checks) runDeferredCheck(candidate, check, true);
            throw;
        }
        deferred.insert(deferred.end(), checks.begin(), checks.end());
    }
    for (const auto &check : deferred) runDeferredCheck(candidate, check);
    // One expansion at the end: every op landed, or the document is refused
    // whole and the caller's document is untouched.
    (void)expandGraphDocument(candidate);
    return candidate;
}

nlohmann::json editGraphInstances(const Json &document, const Json &attachments, const Json &detachments) {
    if (!attachments.is_array() || !detachments.is_array())
        throw std::invalid_argument("graph attachments and detachments must be arrays");
    const auto &nodes = document.at("nodes");
    if (!nodes.is_array()) throw std::invalid_argument("graph nodes must be an array");
    auto ops = Json::array();
    std::set<std::string> removed;
    for (const auto &value : detachments) {
        const auto id = identifier(value);
        if (!removed.insert(id).second) throw std::invalid_argument("duplicate graph detachment: " + id);
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) { return node.at("id") == id; });
        if (found == nodes.end() || found->at("op") != "graph.instance" || !found->contains("owner"))
            throw std::invalid_argument("graph detachment requires an owned root instance: " + id);
        ops.push_back({{"op", "remove_node"}, {"node", id}});
    }
    for (const auto &instance : attachments) {
        if (!instance.is_object() || instance.value("op", std::string{}) != "graph.instance" || !instance.contains("owner"))
            throw std::invalid_argument("graph attachment requires an owned graph.instance");
        ops.push_back({{"op", "add_node"}, {"node", instance}});
    }
    return editGraphDocument(document, ops);
}

} // namespace Pelican
