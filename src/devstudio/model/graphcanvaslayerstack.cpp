#include "graphcanvaslayerstack.hpp"

#include "../../project/canvas/canvasblend.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

constexpr auto kScalarType = "pelican.value.scalar@1";

std::string scopeToken(std::string_view scope) {
    return scope.empty() ? std::string{"root"} : std::string{scope};
}

const GraphCanvasNode *findNode(const std::vector<GraphCanvasNode> &nodes, std::string_view id) {
    const auto found = std::ranges::find(nodes, id, &GraphCanvasNode::id);
    return found == nodes.end() ? nullptr : &*found;
}

const GraphCanvasPort *findPort(const GraphCanvasNode &node, std::string_view port) {
    const auto found = std::ranges::find(node.inputs, port, &GraphCanvasPort::port);
    return found == node.inputs.end() ? nullptr : &*found;
}

// The node on a connected input, or empty.
std::string sourceOf(const GraphCanvasNode &node, std::string_view port) {
    const auto *found = findPort(node, port);
    return found != nullptr && found->connected ? found->source_node : std::string{};
}

std::string opacityPort(std::string_view input) { return std::string{input} + ".opacity"; }

const GraphCanvasNode *stackNode(const std::vector<GraphCanvasNode> &nodes, std::string_view stack) {
    const auto *found = findNode(nodes, stack);
    return found != nullptr && found->op == "canvas.stack" ? found : nullptr;
}

Json layersOf(const GraphCanvasNode &stack) {
    const auto layers = stack.parameters.find("layers");
    return layers != stack.parameters.end() && layers->is_array() ? *layers : Json::array();
}

std::array<double, 2> stackSize(const GraphCanvasNode &stack) {
    const auto size = stack.parameters.find("size");
    if (size != stack.parameters.end() && size->is_array() && size->size() == 2 &&
        size->at(0).is_number() && size->at(1).is_number())
        return {size->at(0).get<double>(), size->at(1).get<double>()};
    return graphCanvasDefaultSize;
}

Json addNode(std::string_view scope, Json node) {
    return Json{{"op", "add_node"}, {"scope", scopeToken(scope)}, {"node", std::move(node)}};
}

Json setParameters(std::string_view scope, const std::string &node, Json parameters) {
    return Json{{"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
                {"parameters", std::move(parameters)}};
}

Json setInput(std::string_view scope, const std::string &node, const std::string &port, Json source) {
    return Json{{"op", "set_input"}, {"scope", scopeToken(scope)}, {"node", node}, {"port", port},
                {"source", std::move(source)}};
}

Json edge(const std::string &node, const std::string &port) {
    return Json{{"node", node}, {"port", port}};
}

// `<base>_<n>` for the first n from 1 whose ids are all free: the node ids
// of the scope and the stack's input names. `extra` are suffixes the same
// number also takes ("_fill").
struct Numbered {
    std::string id;
    int number = 1;
};
Numbered numberedId(const std::vector<GraphCanvasNode> &nodes, const GraphCanvasNode *stack,
                    const std::string &base, std::initializer_list<const char *> extra = {}) {
    const auto taken = [&](const std::string &id) {
        if (findNode(nodes, id) != nullptr) return true;
        return stack != nullptr && findPort(*stack, id) != nullptr;
    };
    for (int number = 1; number < 10000; ++number) {
        const auto id = base + "_" + std::to_string(number);
        if (taken(id)) continue;
        if (std::ranges::any_of(extra, [&](const char *suffix) { return taken(id + suffix); }))
            continue;
        return {id, number};
    }
    return {base, 0};
}

// The id itself when free, else `<id>_2`, `<id>_3`...
std::string freeId(const std::vector<GraphCanvasNode> &nodes, const GraphCanvasNode *stack,
                   const std::string &base) {
    const auto taken = [&](const std::string &id) {
        return findNode(nodes, id) != nullptr || (stack != nullptr && findPort(*stack, id) != nullptr);
    };
    if (!taken(base)) return base;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = base + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return base;
}

Json colorJson(const std::array<double, 4> &color) {
    return Json::array({color[0], color[1], color[2], color[3]});
}

const GraphCanvasSwatch &swatch(std::string_view name) {
    const auto &all = graphCanvasSwatches();
    const auto found = std::ranges::find(all, name, &GraphCanvasSwatch::name);
    return found != all.end() ? *found : all.front();
}

Json shapeParameters(std::string_view shape, const std::array<double, 2> &size) {
    Json parameters{{"size", Json::array({size[0] / 4.0, size[1] / 4.0})},
                    {"center", Json::array({size[0] / 2.0, size[1] / 2.0})}};
    if (shape == "rect") parameters["corner_radius"] = 0.0;
    parameters["rotation"] = 0.0;
    return parameters;
}

std::string capitalized(std::string_view word) {
    std::string text{word};
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

// `layers` with one element added at `at` (the end when past it), and the
// set_node_parameters and set_input that put it on the stack.
Json addLayerOps(std::string_view scope, const GraphCanvasNode &stack, std::size_t at,
                 const std::string &input, const std::string &name, const std::string &source,
                 const std::string &source_port) {
    auto parameters = stack.parameters;
    auto layers = layersOf(stack);
    Json element{{"input", input}, {"name", name}};
    at = std::min(at, layers.size());
    layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(at), std::move(element));
    parameters["layers"] = std::move(layers);
    return Json::array({setParameters(scope, stack.id, std::move(parameters)),
                        setInput(scope, stack.id, input, edge(source, source_port))});
}

// NB-2a: where a + puts its layer -- just above the picked row `above`, on
// top without one (or with one past the top).
std::size_t aboveAt(const GraphCanvasNode &stack, std::optional<std::size_t> above) {
    const auto count = layersOf(stack).size();
    return above && *above < count ? *above + 1 : count;
}

void append(Json &ops, Json more) {
    for (auto &op : more) ops.push_back(std::move(op));
}

} // namespace

bool isGraphCanvasOperation(std::string_view op) {
    return op.starts_with("canvas.") || op.starts_with("shape.") || op.starts_with("filter.");
}

std::optional<std::string> graphCanvasPaletteLabel(std::string_view op) {
    if (op == "canvas.output")
        return std::string{"Canvas   (canvas.output + canvas.stack)"};
    return std::nullopt;
}

std::vector<GraphCanvasPort> graphCanvasStackPorts(const Json &parameters) {
    std::vector<GraphCanvasPort> ports;
    const auto layers = parameters.is_object() ? parameters.find("layers") : parameters.end();
    if (parameters.is_object() && layers != parameters.end() && layers->is_array())
        for (const auto &element : *layers) {
            if (!element.is_object()) continue;
            const auto input = element.find("input");
            if (input == element.end() || !input->is_string() || input->get_ref<const std::string &>().empty())
                continue;
            const auto name = input->get<std::string>();
            if (std::ranges::any_of(ports, [&](const auto &port) { return port.port == name; })) continue;
            GraphCanvasPort layer;
            layer.port = name;
            layer.declared = true;
            layer.required = true;
            layer.type = std::string{graphCanvasLayerType};
            ports.push_back(std::move(layer));
            GraphCanvasPort opacity;
            opacity.port = opacityPort(name);
            opacity.declared = true;
            opacity.type = kScalarType;
            ports.push_back(std::move(opacity));
        }
    // SG-2 (CV-2a): a layer's `mask` is one more layer port, after them all.
    if (parameters.is_object() && layers != parameters.end() && layers->is_array())
        for (const auto &element : *layers) {
            const auto mask = element.is_object() ? element.find("mask") : element.end();
            if (!element.is_object() || mask == element.end() || !mask->is_string() ||
                mask->get_ref<const std::string &>().empty())
                continue;
            const auto name = mask->get<std::string>();
            if (std::ranges::any_of(ports, [&](const auto &port) { return port.port == name; })) continue;
            GraphCanvasPort port;
            port.port = name;
            port.declared = true;
            port.required = true;
            port.type = std::string{graphCanvasLayerType};
            ports.push_back(std::move(port));
        }
    GraphCanvasPort open;
    open.port = std::string{graphCanvasStackAddPort};
    open.declared = true;
    open.type = std::string{graphCanvasLayerType};
    ports.push_back(std::move(open));
    return ports;
}

std::vector<GraphCanvasStackRow> graphCanvasLayerRows(const std::vector<GraphCanvasNode> &nodes,
                                                      std::string_view stack) {
    std::vector<GraphCanvasStackRow> rows;
    const auto *found = stackNode(nodes, stack);
    if (found == nullptr) return rows;
    const auto layers = layersOf(*found);
    for (std::size_t index = layers.size(); index-- > 0;) {
        const auto &element = layers.at(index);
        if (!element.is_object()) continue;
        GraphCanvasStackRow row;
        row.index = index;
        row.input = element.value("input", std::string{});
        row.name = element.contains("name") && element.at("name").is_string()
                       ? element.at("name").get<std::string>()
                       : row.input;
        if (const auto visible = element.find("visible"); visible != element.end() && visible->is_boolean())
            row.visible = visible->get<bool>();
        if (const auto opacity = element.find("opacity"); opacity != element.end() && opacity->is_number())
            row.opacity = opacity->get<double>();
        row.opacity_driven = !sourceOf(*found, opacityPort(row.input)).empty();
        row.clip = element.value("clip", false) == true;
        row.pass_through = element.value("pass_through", false) == true;
        if (const auto blend = element.find("blend"); blend != element.end() && blend->is_string())
            row.blend = blend->get<std::string>();
        if (const auto mask = element.find("mask"); mask != element.end() && mask->is_string()) {
            row.mask = mask->get<std::string>();
            row.mask_source = sourceOf(*found, row.mask);
        }
        row.source = sourceOf(*found, row.input);
        // SG-2: a layer moved, turned or scaled draws what its transform
        // reads; SG-3: one filtered draws what its filters read. The chain is
        // read down from the stack, whatever order it was authored in.
        for (std::size_t guard = 0; !row.source.empty() && guard < nodes.size(); ++guard) {
            const auto *link = findNode(nodes, row.source);
            if (link == nullptr) break;
            if (link->op == "canvas.transform" && row.transform.empty()) {
                row.transform = row.source;
            } else if (link->op.starts_with("filter.")) {
                row.filters.insert(row.filters.begin(), row.source);
            } else {
                break;
            }
            row.source = sourceOf(*link, "layer");
        }
        if (const auto *source = row.source.empty() ? nullptr : findNode(nodes, row.source)) {
            if (source->op == "canvas.stack") {
                row.kind = "group";
            } else if (source->op == "canvas.image") {
                row.kind = "image";
            } else if (source->op == "canvas.color") {
                row.kind = "color";
            } else if (source->op == "graph.instance") {
                row.kind = "import";
                row.definition = source->definition;
            } else if (source->op == "canvas.backdrop") {
                row.kind = "adjustment";
            } else if (source->op == "shape.fill" || source->op == "shape.stroke") {
                row.paint = source->op == "shape.fill" ? "fill" : "stroke";
                row.shape = sourceOf(*source, "geometry");
                if (const auto *shape = row.shape.empty() ? nullptr : findNode(nodes, row.shape)) {
                    if (shape->op == "shape.rect") row.kind = "rect";
                    else if (shape->op == "shape.ellipse") row.kind = "ellipse";
                    else if (shape->op == "shape.text") row.kind = "text";
                }
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string graphCanvasLayerKindText(const GraphCanvasStackRow &row) {
    if (row.kind == "import") return "PSD";
    auto text = capitalized(row.kind);
    if (row.paint == "stroke") text += " stroke";
    return text;
}

std::string graphCanvasLayerOpacityPort(std::string_view input) { return opacityPort(input); }

std::optional<GraphCanvasRowEntry> graphCanvasRowEntry(const GraphCanvasStackRow &row, std::string_view scope,
                                                       bool read_only,
                                                       const std::vector<GraphCanvasNode> &definition_nodes) {
    if (row.kind == "group" && !row.source.empty())
        return GraphCanvasRowEntry{std::string{scope}, row.source, read_only};
    if (row.kind == "import" && !row.definition.empty())
        return GraphCanvasRowEntry{row.definition, graphCanvasRootStack(definition_nodes), true};
    return std::nullopt;
}

std::string graphCanvasRootStack(const std::vector<GraphCanvasNode> &nodes) {
    for (const auto &node : nodes) {
        if (node.op != "canvas.stack") continue;
        const bool read = std::ranges::any_of(nodes, [&](const GraphCanvasNode &reader) {
            return std::ranges::any_of(reader.inputs, [&](const GraphCanvasPort &port) {
                return port.connected && port.source_node == node.id;
            });
        });
        if (!read) return node.id;
    }
    return {};
}

bool graphCanvasImportedDefinition(const std::vector<GraphCanvasNode> &nodes, std::string_view definition) {
    if (definition.empty()) return false;
    for (const auto &each : nodes) {
        if (each.op != "canvas.stack") continue;
        for (const auto &row : graphCanvasLayerRows(nodes, each.id))
            if (row.kind == "import" && row.definition == definition) return true;
    }
    return false;
}

std::string graphCanvasStackFor(const std::vector<GraphCanvasNode> &nodes, std::string_view node) {
    const auto *found = findNode(nodes, node);
    if (found == nullptr) return {};
    if (found->op == "canvas.stack") return found->id;
    if (found->op == "canvas.output") {
        const auto source = sourceOf(*found, "layer");
        return stackNode(nodes, source) != nullptr ? source : std::string{};
    }
    // SG-3: a placed stack is shown for its canvas.place.
    if (found->op == "canvas.place") {
        const auto source = sourceOf(*found, "layer");
        return stackNode(nodes, source) != nullptr ? source : std::string{};
    }
    // The paints a picked shape is drawn by, or the picked paint itself.
    std::vector<std::string> paints;
    if (found->op == "shape.fill" || found->op == "shape.stroke" || found->op == "canvas.image" ||
        found->op == "canvas.color" || found->op == "canvas.transform" ||
        found->op == "graph.instance" || found->op == "canvas.backdrop" || found->op.starts_with("filter.")) {
        paints.push_back(found->id);
    } else if (found->op == "shape.rect" || found->op == "shape.ellipse" || found->op == "shape.text") {
        for (const auto &each : nodes)
            if ((each.op == "shape.fill" || each.op == "shape.stroke") &&
                sourceOf(each, "geometry") == found->id)
                paints.push_back(each.id);
    } else {
        return {};
    }
    // SG-2: and the transforms that move them (SG-3: and the filters).
    for (std::size_t at = 0; at < paints.size(); ++at)
        for (const auto &each : nodes)
            if ((each.op == "canvas.transform" || each.op.starts_with("filter.")) &&
                sourceOf(each, "layer") == paints[at] &&
                std::ranges::find(paints, each.id) == paints.end())
                paints.push_back(each.id);
    for (const auto &paint : paints)
        for (const auto &each : nodes) {
            if (each.op != "canvas.stack") continue;
            for (const auto &port : each.inputs)
                if (port.connected && port.source_node == paint && port.type == graphCanvasLayerType)
                    return each.id;
        }
    return {};
}

std::optional<std::size_t> graphCanvasRowFor(const std::vector<GraphCanvasNode> &nodes,
                                             std::string_view stack, std::string_view node) {
    for (const auto &row : graphCanvasLayerRows(nodes, stack))
        if (row.source == node || (!row.shape.empty() && row.shape == node) ||
            (!row.transform.empty() && row.transform == node) ||
            (!row.mask_source.empty() && row.mask_source == node) ||
            std::ranges::find(row.filters, node) != row.filters.end())
            return row.index;
    return std::nullopt;
}

Json graphCanvasCompositionOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope) {
    // canvas, canvas_stack; canvas_2, canvas_stack_2 when either is taken.
    std::string output = "canvas";
    std::string stack = "canvas_stack";
    for (int suffix = 2; findNode(nodes, output) != nullptr || findNode(nodes, stack) != nullptr; ++suffix) {
        output = "canvas_" + std::to_string(suffix);
        stack = "canvas_stack_" + std::to_string(suffix);
        if (suffix > 9999) return Json::array();
    }
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", stack}, {"op", "canvas.stack"},
                                      {"parameters", Json{{"size", Json::array({graphCanvasDefaultSize[0],
                                                                                graphCanvasDefaultSize[1]})},
                                                          {"layers", Json::array()}}},
                                      {"outputs", Json::array({"layer"})}}));
    ops.push_back(addNode(scope, Json{{"id", output}, {"op", "canvas.output"},
                                      {"parameters", Json::object()},
                                      {"inputs", Json{{"layer", edge(stack, "layer")}}},
                                      {"outputs", Json::array({"image"})}}));
    return ops;
}

Json graphCanvasPaletteOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                           std::string_view op) {
    if (op == "canvas.output") return graphCanvasCompositionOps(nodes, scope);
    if (op == "canvas.stack")
        return Json::array({addNode(scope, Json{{"id", freeId(nodes, nullptr, "canvas_stack")},
                                                {"op", "canvas.stack"},
                                                {"parameters", Json{{"size", Json::array({graphCanvasDefaultSize[0],
                                                                                          graphCanvasDefaultSize[1]})},
                                                                    {"layers", Json::array()}}},
                                                {"outputs", Json::array({"layer"})}})});
    if (op == "shape.rect" || op == "shape.ellipse") {
        const auto shape = op.substr(6);
        const auto id = numberedId(nodes, nullptr, std::string{shape}).id;
        return Json::array({addNode(scope, Json{{"id", id}, {"op", std::string{op}},
                                                {"parameters", shapeParameters(shape, graphCanvasDefaultSize)},
                                                {"outputs", Json::array({"geometry"})}})});
    }
    if (op == "shape.fill" || op == "shape.stroke") {
        const bool stroke = op == "shape.stroke";
        Json parameters{{"color", colorJson(swatch(stroke ? "White" : "Blue").color)}};
        if (stroke) {
            parameters["width"] = 4.0;
            parameters["align"] = "center";
        }
        return Json::array({addNode(scope, Json{{"id", numberedId(nodes, nullptr, stroke ? "stroke" : "fill").id},
                                                {"op", std::string{op}}, {"parameters", std::move(parameters)},
                                                {"outputs", Json::array({"layer"})}})});
    }
    return Json::array();
}

Json graphCanvasAddShapeLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                 std::string_view stack, std::string_view shape,
                                 std::optional<std::size_t> above) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr || (shape != "rect" && shape != "ellipse")) return Json::array();
    const auto numbered = numberedId(nodes, target, std::string{shape}, {"_fill"});
    if (numbered.number == 0) return Json::array();
    const auto fill = numbered.id + "_fill";
    const auto &color = swatch(shape == "rect" ? "Blue" : "Orange").color;
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", numbered.id}, {"op", "shape." + std::string{shape}},
                                      {"parameters", shapeParameters(shape, stackSize(*target))},
                                      {"outputs", Json::array({"geometry"})}}));
    ops.push_back(addNode(scope, Json{{"id", fill}, {"op", "shape.fill"},
                                      {"parameters", Json{{"color", colorJson(color)}}},
                                      {"inputs", Json{{"geometry", edge(numbered.id, "geometry")}}},
                                      {"outputs", Json::array({"layer"})}}));
    append(ops, addLayerOps(scope, *target, aboveAt(*target, above), numbered.id,
                            capitalized(shape) + " " + std::to_string(numbered.number), fill, "layer"));
    return ops;
}

Json graphCanvasAddGroupLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                 std::string_view stack, std::optional<std::size_t> above) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    const auto numbered = numberedId(nodes, target, "group");
    if (numbered.number == 0) return Json::array();
    const auto size = stackSize(*target);
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", numbered.id}, {"op", "canvas.stack"},
                                      {"parameters", Json{{"size", Json::array({size[0], size[1]})},
                                                          {"layers", Json::array()}}},
                                      {"outputs", Json::array({"layer"})}}));
    append(ops, addLayerOps(scope, *target, aboveAt(*target, above), numbered.id,
                            "Group " + std::to_string(numbered.number), numbered.id, "layer"));
    return ops;
}

Json graphCanvasAddStrokeLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                  std::string_view stack, std::size_t index) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    const auto rows = graphCanvasLayerRows(nodes, stack);
    const auto row = std::ranges::find(rows, index, &GraphCanvasStackRow::index);
    if (row == rows.end() || row->shape.empty()) return Json::array();
    const auto id = freeId(nodes, target, row->shape + "_stroke");
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", id}, {"op", "shape.stroke"},
                                      {"parameters", Json{{"color", colorJson(swatch("White").color)},
                                                          {"width", 4.0},
                                                          {"align", "center"}}},
                                      {"inputs", Json{{"geometry", edge(row->shape, "geometry")}}},
                                      {"outputs", Json::array({"layer"})}}));
    append(ops, addLayerOps(scope, *target, index + 1, id, row->name + " stroke", id, "layer"));
    return ops;
}

Json graphCanvasDeleteLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                               std::string_view stack, std::size_t index) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    auto layers = layersOf(*target);
    if (index >= layers.size() || !layers.at(index).is_object()) return Json::array();
    const auto input = layers.at(index).value("input", std::string{});
    // SG-2: its mask goes with it, unless another layer reads the same one.
    auto mask = layers.at(index).value("mask", std::string{});
    auto parameters = target->parameters;
    layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(index));
    if (std::ranges::any_of(layers, [&](const Json &other) {
            return other.is_object() && other.value("mask", std::string{}) == mask;
        }))
        mask.clear();
    parameters["layers"] = std::move(layers);
    Json ops = Json::array({setParameters(scope, target->id, std::move(parameters))});
    if (!sourceOf(*target, input).empty()) ops.push_back(setInput(scope, target->id, input, nullptr));
    if (!sourceOf(*target, opacityPort(input)).empty())
        ops.push_back(setInput(scope, target->id, opacityPort(input), nullptr));
    if (!mask.empty() && !sourceOf(*target, mask).empty())
        ops.push_back(setInput(scope, target->id, mask, nullptr));
    // The canvas nodes upstream of the layer that nothing else reads: a node
    // goes once every reader of it has gone (or is the line cut here), which
    // removes readers before what they read.
    const auto source = sourceOf(*target, input);
    std::vector<std::string> upstream;
    if (!source.empty() || !mask.empty()) {
        std::vector<std::string> pending;
        if (!source.empty()) pending.push_back(source);
        if (!mask.empty() && !sourceOf(*target, mask).empty()) pending.push_back(sourceOf(*target, mask));
        while (!pending.empty()) {
            auto id = std::move(pending.back());
            pending.pop_back();
            const auto *node = findNode(nodes, id);
            // SG-2: an imported PSD's instance goes too (its definition stays).
            if (node == nullptr || (!isGraphCanvasOperation(node->op) && node->op != "graph.instance") ||
                node->op == "canvas.output" ||
                std::ranges::find(upstream, id) != upstream.end())
                continue;
            upstream.push_back(id);
            for (const auto &port : node->inputs)
                if (port.connected) pending.push_back(port.source_node);
        }
    }
    std::vector<std::string> removed;
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto &id : upstream) {
            if (std::ranges::find(removed, id) != removed.end()) continue;
            const bool read = std::ranges::any_of(nodes, [&](const GraphCanvasNode &reader) {
                if (std::ranges::find(removed, reader.id) != removed.end()) return false;
                return std::ranges::any_of(reader.inputs, [&](const GraphCanvasPort &port) {
                    if (!port.connected || port.source_node != id) return false;
                    return !(reader.id == target->id &&
                             (port.port == input || port.port == opacityPort(input) ||
                              (!mask.empty() && port.port == mask)));
                });
            });
            if (read) continue;
            removed.push_back(id);
            changed = true;
        }
    }
    for (const auto &id : removed)
        ops.push_back(Json{{"op", "remove_node"}, {"scope", scopeToken(scope)}, {"node", id}});
    return ops;
}

Json graphCanvasMoveLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                             std::string_view stack, std::size_t index, int delta) {
    if (delta != 1 && delta != -1) return Json::array();
    if (delta < 0 && index == 0) return Json::array();
    return graphCanvasMoveLayerToOps(nodes, scope, stack, index, delta > 0 ? index + 1 : index - 1);
}

Json graphCanvasMoveLayerToOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                               std::string_view stack, std::size_t from, std::size_t to) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    auto layers = layersOf(*target);
    if (from >= layers.size() || to >= layers.size() || from == to) return Json::array();
    // The element itself travels: its input, name, opacity, blend, mask...
    auto moved = std::move(layers.at(from));
    layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(from));
    layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    auto parameters = target->parameters;
    parameters["layers"] = std::move(layers);
    return Json::array({setParameters(scope, target->id, std::move(parameters))});
}

Json graphCanvasSetLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                            std::string_view stack, std::size_t index, std::string_view key,
                            const Json &value) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    auto layers = layersOf(*target);
    if (index >= layers.size() || !layers.at(index).is_object()) return Json::array();
    auto &element = layers.at(index);
    const std::string member{key};
    if (member == "visible" && value.is_boolean()) {
        if (value.get<bool>()) element.erase("visible");
        else element["visible"] = false;
    } else if (member == "opacity" && value.is_number()) {
        const auto opacity = std::clamp(value.get<double>(), 0.0, 1.0);
        if (std::abs(opacity - 1.0) < 1e-9) element.erase("opacity");
        else element["opacity"] = opacity;
    } else if (member == "name" && value.is_string()) {
        if (value.get_ref<const std::string &>().empty()) element.erase("name");
        else element["name"] = value;
    } else {
        return Json::array();
    }
    if (layers == layersOf(*target)) return Json::array();
    auto parameters = target->parameters;
    parameters["layers"] = std::move(layers);
    return Json::array({setParameters(scope, target->id, std::move(parameters))});
}

Json graphCanvasConnectLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                std::string_view stack, const std::string &source_node,
                                const std::string &source_port) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    return graphCanvasInsertLayerOps(nodes, scope, stack, layersOf(*target).size(), source_node, source_port);
}

Json graphCanvasInsertLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                               std::string_view stack, std::size_t at, const std::string &source_node,
                               const std::string &source_port) {
    const auto *target = stackNode(nodes, stack);
    const auto *source = findNode(nodes, source_node);
    if (target == nullptr || source == nullptr || source_node == target->id) return Json::array();
    // The input is named after what it reads (no dot: the engine refuses one).
    auto base = source_node;
    std::ranges::replace(base, '.', '_');
    const auto input = freeId({}, target, base);
    return addLayerOps(scope, *target, at, input, source->heading(), source_node, source_port);
}

Json graphCanvasDisconnectLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                   std::string_view stack, std::string_view port) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    const auto layers = layersOf(*target);
    for (std::size_t index = 0; index < layers.size(); ++index) {
        if (!layers.at(index).is_object() || layers.at(index).value("input", std::string{}) != port) continue;
        auto kept = layers;
        kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(index));
        auto parameters = target->parameters;
        parameters["layers"] = std::move(kept);
        Json ops = Json::array({setParameters(scope, target->id, std::move(parameters))});
        const std::string input{port};
        if (!sourceOf(*target, input).empty()) ops.push_back(setInput(scope, target->id, input, nullptr));
        if (!sourceOf(*target, opacityPort(input)).empty())
            ops.push_back(setInput(scope, target->id, opacityPort(input), nullptr));
        return ops;
    }
    return Json::array();
}

Json graphCanvasSetParameterOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                std::string_view node, std::string_view key, const Json &value) {
    const auto *found = findNode(nodes, node);
    if (found == nullptr || !isGraphCanvasOperation(found->op)) return Json::array();
    auto parameters = found->parameters.is_object() ? found->parameters : Json::object();
    if (parameters.contains(std::string{key}) && parameters.at(std::string{key}) == value)
        return Json::array();
    parameters[std::string{key}] = value;
    return Json::array({setParameters(scope, found->id, std::move(parameters))});
}

// --- SG-2.

std::vector<GraphCanvasOutputStack> graphCanvasOutputStacks(const std::vector<GraphCanvasNode> &nodes) {
    std::vector<GraphCanvasOutputStack> found;
    for (const auto &node : nodes) {
        if (node.op != "canvas.output") continue;
        const auto source = sourceOf(node, "layer");
        if (stackNode(nodes, source) != nullptr) found.push_back({node.id, source});
    }
    return found;
}

std::string graphCanvasDefinitionStack(const Json &definition) {
    if (!definition.is_object()) return {};
    const auto outputs = definition.find("outputs");
    if (outputs == definition.end() || !outputs->is_object()) return {};
    const auto layer = outputs->find("layer");
    if (layer == outputs->end() || !layer->is_object()) return {};
    const auto node = layer->value("node", std::string{});
    const auto nodes = definition.find("nodes");
    if (node.empty() || nodes == definition.end() || !nodes->is_array()) return {};
    for (const auto &each : *nodes)
        if (each.is_object() && each.value("id", std::string{}) == node &&
            each.value("op", std::string{}) == "canvas.stack")
            return node;
    return {};
}

Json graphCanvasAddImageLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                 std::string_view stack, const std::string &asset,
                                 const std::array<double, 2> &size, const std::string &name,
                                 std::optional<std::size_t> above) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr || asset.empty() || size[0] <= 0.0 || size[1] <= 0.0) return Json::array();
    const auto numbered = numberedId(nodes, target, "image");
    if (numbered.number == 0) return Json::array();
    const auto canvas = stackSize(*target);
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", numbered.id}, {"op", "canvas.image"},
                                      {"parameters", Json{{"asset", asset},
                                                          {"offset", Json::array({(canvas[0] - size[0]) / 2.0,
                                                                                  (canvas[1] - size[1]) / 2.0})},
                                                          {"size", Json::array({size[0], size[1]})}}},
                                      {"outputs", Json::array({"layer"})}}));
    append(ops, addLayerOps(scope, *target, aboveAt(*target, above), numbered.id,
                            name.empty() ? "Image " + std::to_string(numbered.number) : name, numbered.id,
                            "layer"));
    return ops;
}

bool graphCanvasClipAllowed(const std::vector<GraphCanvasNode> &nodes, std::string_view stack,
                            std::size_t index) {
    // Every clip layer needs an unclipped one somewhere below it: with the
    // bottom layer unclipped, each layer above has one.
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr || index == 0) return false;
    const auto layers = layersOf(*target);
    if (index >= layers.size() || layers.empty() || !layers.at(0).is_object()) return false;
    return layers.at(0).value("clip", false) != true;
}

Json graphCanvasSetLayerFlagOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                std::string_view stack, std::size_t index, std::string_view key,
                                const Json &value) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    auto layers = layersOf(*target);
    if (index >= layers.size() || !layers.at(index).is_object()) return Json::array();
    auto &element = layers.at(index);
    const std::string member{key};
    if ((member == "clip" || member == "pass_through") && value.is_boolean()) {
        if (value.get<bool>()) element[member] = true;
        else element.erase(member);
    } else if (member == "blend" && value.is_string()) {
        if (value == "normal") element.erase("blend");
        else element["blend"] = value;
    } else {
        return Json::array();
    }
    if (layers == layersOf(*target)) return Json::array();
    auto parameters = target->parameters;
    parameters["layers"] = std::move(layers);
    return Json::array({setParameters(scope, target->id, std::move(parameters))});
}

Json graphCanvasAddMaskOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                           std::string_view stack, std::size_t index, const std::string &asset,
                           const std::array<double, 2> &size) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr || asset.empty() || size[0] <= 0.0 || size[1] <= 0.0) return Json::array();
    const auto rows = graphCanvasLayerRows(nodes, stack);
    const auto row = std::ranges::find(rows, index, &GraphCanvasStackRow::index);
    if (row == rows.end() || !row->mask.empty() || row->input.empty()) return Json::array();
    // Over an image layer where the image is; over anything else, centred.
    const auto canvas = stackSize(*target);
    std::array<double, 2> offset{(canvas[0] - size[0]) / 2.0, (canvas[1] - size[1]) / 2.0};
    if (const auto *image = row->kind == "image" ? findNode(nodes, row->source) : nullptr) {
        const auto found = image->parameters.find("offset");
        if (found != image->parameters.end() && found->is_array() && found->size() == 2 &&
            found->at(0).is_number() && found->at(1).is_number())
            offset = {found->at(0).get<double>(), found->at(1).get<double>()};
    }
    const auto id = freeId(nodes, target, row->input + "_mask");
    auto layers = layersOf(*target);
    layers.at(index)["mask"] = id;
    auto parameters = target->parameters;
    parameters["layers"] = std::move(layers);
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", id}, {"op", "canvas.image"},
                                      {"parameters", Json{{"asset", asset},
                                                          {"channel", "luminance"},
                                                          {"offset", Json::array({offset[0], offset[1]})},
                                                          {"size", Json::array({size[0], size[1]})}}},
                                      {"outputs", Json::array({"layer"})}}));
    ops.push_back(setParameters(scope, target->id, std::move(parameters)));
    ops.push_back(setInput(scope, target->id, id, edge(id, "layer")));
    return ops;
}

Json graphCanvasRemoveMaskOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                              std::string_view stack, std::size_t index) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    auto layers = layersOf(*target);
    if (index >= layers.size() || !layers.at(index).is_object()) return Json::array();
    const auto mask = layers.at(index).value("mask", std::string{});
    if (mask.empty()) return Json::array();
    layers.at(index).erase("mask");
    const bool shared = std::ranges::any_of(layers, [&](const Json &other) {
        return other.is_object() && other.value("mask", std::string{}) == mask;
    });
    auto parameters = target->parameters;
    parameters["layers"] = std::move(layers);
    Json ops = Json::array({setParameters(scope, target->id, std::move(parameters))});
    if (shared) return ops;
    const auto source = sourceOf(*target, mask);
    if (source.empty()) return ops;
    ops.push_back(setInput(scope, target->id, mask, nullptr));
    const bool read_elsewhere = std::ranges::any_of(nodes, [&](const GraphCanvasNode &reader) {
        return std::ranges::any_of(reader.inputs, [&](const GraphCanvasPort &port) {
            return port.connected && port.source_node == source &&
                   !(reader.id == target->id && port.port == mask);
        });
    });
    if (!read_elsewhere)
        ops.push_back(Json{{"op", "remove_node"}, {"scope", scopeToken(scope)}, {"node", source}});
    return ops;
}

Json graphCanvasSetStackOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                            std::string_view stack, std::string_view key, const Json &value) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    auto parameters = target->parameters;
    if (key == "blend_space" && value.is_string() && (value == "linear" || value == "srgb")) {
        if (value == "linear") parameters.erase("blend_space");
        else parameters["blend_space"] = value;
    } else if (key == "size" && value.is_array() && value.size() == 2 && value.at(0).is_number() &&
               value.at(1).is_number() && value.at(0).get<double>() > 0.0 && value.at(1).get<double>() > 0.0) {
        parameters["size"] = value;
    } else {
        return Json::array();
    }
    if (parameters == target->parameters) return Json::array();
    return Json::array({setParameters(scope, target->id, std::move(parameters))});
}

bool GraphCanvasTransformValues::isDefault() const noexcept {
    return translate[0] == 0.0 && translate[1] == 0.0 && rotate == 0.0 && scale[0] == 1.0 && scale[1] == 1.0;
}

namespace {

std::array<double, 2> pairOf(const Json &parameters, const char *key, double fallback) {
    const auto found = parameters.find(key);
    if (found == parameters.end() || !found->is_array() || found->size() != 2 || !found->at(0).is_number() ||
        !found->at(1).is_number())
        return {fallback, fallback};
    return {found->at(0).get<double>(), found->at(1).get<double>()};
}

// Where a layer turns and scales about: its own middle.
std::array<double, 2> layerPivot(const std::vector<GraphCanvasNode> &nodes, const GraphCanvasNode &stack,
                                 const GraphCanvasStackRow &row) {
    const auto canvas = stackSize(stack);
    if (const auto *source = findNode(nodes, row.source); source != nullptr && source->op == "canvas.image") {
        const auto offset = pairOf(source->parameters, "offset", 0.0);
        const auto size = pairOf(source->parameters, "size", 0.0);
        return {offset[0] + size[0] / 2.0, offset[1] + size[1] / 2.0};
    }
    if (const auto *shape = findNode(nodes, row.shape))
        return pairOf(shape->parameters, shape->op == "shape.text" ? "origin" : "center", 0.0);
    return {canvas[0] / 2.0, canvas[1] / 2.0};
}

Json transformParameters(const GraphCanvasTransformValues &values, const std::array<double, 2> &pivot) {
    Json parameters = Json::object();
    if (values.translate[0] != 0.0 || values.translate[1] != 0.0)
        parameters["translate"] = Json::array({values.translate[0], values.translate[1]});
    if (values.rotate != 0.0) parameters["rotate"] = values.rotate;
    if (values.scale[0] != 1.0 || values.scale[1] != 1.0)
        parameters["scale"] = Json::array({values.scale[0], values.scale[1]});
    parameters["pivot"] = Json::array({pivot[0], pivot[1]});
    return parameters;
}

} // namespace

GraphCanvasTransformValues graphCanvasLayerTransform(const std::vector<GraphCanvasNode> &nodes,
                                                     const GraphCanvasStackRow &row) {
    GraphCanvasTransformValues values;
    const auto *transform = row.transform.empty() ? nullptr : findNode(nodes, row.transform);
    if (transform == nullptr) return values;
    values.translate = pairOf(transform->parameters, "translate", 0.0);
    values.scale = pairOf(transform->parameters, "scale", 1.0);
    if (const auto rotate = transform->parameters.find("rotate");
        rotate != transform->parameters.end() && rotate->is_number())
        values.rotate = rotate->get<double>();
    return values;
}

Json graphCanvasSetTransformOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                std::string_view stack, std::size_t index,
                                const GraphCanvasTransformValues &values) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    const auto rows = graphCanvasLayerRows(nodes, stack);
    const auto row = std::ranges::find(rows, index, &GraphCanvasStackRow::index);
    if (row == rows.end() || row->input.empty()) return Json::array();
    if (row->transform.empty()) {
        // None yet: one between the layer's input and what it reads -- SG-3:
        // under its filters (transform, then filters, then the stack).
        const auto *lowest = row->filters.empty() ? nullptr : findNode(nodes, row->filters.front());
        const auto *line = lowest != nullptr ? findPort(*lowest, "layer") : findPort(*target, row->input);
        if (values.isDefault() || line == nullptr || !line->connected) return Json::array();
        const auto id = freeId(nodes, target, row->input + "_transform");
        Json ops = Json::array();
        ops.push_back(addNode(scope, Json{{"id", id}, {"op", "canvas.transform"},
                                          {"parameters", transformParameters(values, layerPivot(nodes, *target, *row))},
                                          {"inputs", Json{{"layer", edge(line->source_node, line->source_port)}}},
                                          {"outputs", Json::array({"layer"})}}));
        ops.push_back(lowest != nullptr ? setInput(scope, lowest->id, "layer", edge(id, "layer"))
                                        : setInput(scope, target->id, row->input, edge(id, "layer")));
        return ops;
    }
    const auto *transform = findNode(nodes, row->transform);
    if (transform == nullptr) return Json::array();
    if (values.isDefault()) {
        // Back at the defaults: what read the transform (the stack, or a
        // filter over it) reads what the transform read.
        const auto *read = findPort(*transform, "layer");
        if (read == nullptr || !read->connected) return Json::array();
        auto reconnect = setInput(scope, target->id, row->input, edge(read->source_node, read->source_port));
        for (const auto &filter : row->filters)
            if (const auto *over = findNode(nodes, filter); over != nullptr && sourceOf(*over, "layer") == transform->id)
                reconnect = setInput(scope, over->id, "layer", edge(read->source_node, read->source_port));
        return Json::array({std::move(reconnect),
                            Json{{"op", "remove_node"}, {"scope", scopeToken(scope)}, {"node", transform->id}}});
    }
    auto parameters = transformParameters(values, pairOf(transform->parameters, "pivot", 0.0));
    if (parameters == transform->parameters) return Json::array();
    return Json::array({setParameters(scope, transform->id, std::move(parameters))});
}

Json graphCanvasImportDefinitionOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                    std::string_view stack, const std::string &name, const Json &definition,
                                    const std::string &label) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr || name.empty() || graphCanvasDefinitionStack(definition).empty()) return Json::array();
    const auto id = freeId(nodes, target, name);
    Json ops = Json::array();
    ops.push_back(Json{{"op", "add_definition"}, {"name", name}, {"definition", definition}});
    ops.push_back(addNode(scope, Json{{"id", id}, {"op", "graph.instance"}, {"definition", name},
                                      {"parameters", Json::object()}}));
    append(ops, addLayerOps(scope, *target, layersOf(*target).size(), id, "PSD: " + label, id, "layer"));
    return ops;
}

Json graphCanvasRebaseImportAssets(const Json &definition, std::string_view directory) {
    auto rebased = definition;
    const auto nodes = rebased.find("nodes");
    if (nodes == rebased.end() || !nodes->is_array()) return rebased;
    constexpr std::string_view imports = "project://imports/";
    for (auto &node : *nodes) {
        if (!node.is_object() || node.value("op", std::string{}) != "canvas.image") continue;
        const auto parameters = node.find("parameters");
        if (parameters == node.end() || !parameters->is_object()) continue;
        const auto asset = parameters->find("asset");
        if (asset == parameters->end() || !asset->is_string()) continue;
        std::string path = asset->get<std::string>();
        if (path.starts_with(imports)) {
            // Another import's directory: the path inside it is kept.
            const auto slash = path.find('/', imports.size());
            if (slash == std::string::npos) continue;
            path = path.substr(slash + 1);
        } else if (path.find("://") != std::string::npos) {
            continue;
        }
        while (path.starts_with("./")) path = path.substr(2);
        *asset = std::string{imports} + std::string{directory} + "/" + path;
    }
    return rebased;
}

std::string graphCanvasRefusalSentence(std::string_view notice) {
    struct Known {
        std::string_view code;
        std::string_view sentence;
    };
    static constexpr Known known[] = {
        {"canvas.clip_base_missing@1",
         "is clipped, but no unclipped layer lies below it to clip to: turn its clip off or move it up."},
        {"canvas.mask_unsupported@1", "has a mask that is not an image: only a canvas.image can be a mask."},
        // SG-3 (CV-2b): every blend of the combo is drawn; this is a name
        // outside it.
        {"canvas.blend_unsupported@1", "asks for a blend the engine does not know: pick one from the list."},
        {"canvas.op_unsupported@1",
         "The canvas reads an op the engine does not draw: only the filters of + Filter are drawn."},
        // CV-3a.
        {"canvas.filter_edge_unsupported@1",
         "A line drives a filter number the engine bakes (Brightness/Contrast): take the line off."},
        {"canvas.filter_backdrop_unsupported@1",
         "A blur, a shadow or a glow cannot read the layers under it (an adjustment): put it on a layer."},
        {"canvas.filter_size_exceeded@1",
         "A blur, a shadow or a glow would need an image over 16384 pixels a side: make its layer or radius smaller."},
        {"canvas.instance_inputs_unsupported@1",
         "An imported group takes no inputs: its definition must have no graph.input."},
        // SG-3 (CV-6a, CV-2b).
        {"canvas.place_backdrop_unsupported@1",
         "is drawn straight into the 3D scene, where a blend mode or an adjustment cannot be used: put it in a "
         "group."},
        {"canvas.filter_parameter_unsupported@1",
         "A filter sets something the engine does not draw yet (Hue/Saturation ranges, a gradient's opacity "
         "stops, dithering or legacy contrast): take it out."},
        {"canvas.method_unavailable@1", "The canvas asks for an effect the engine has no way to draw yet."},
        {"canvas.range_unbounded@1",
         "An effect's reach has no limit to size it by: give the parameter that drives it a maximum."},
        {"graph.image_source_conflict@1",
         "An image read from a file also sets what the file decides (its size, format, lifetime or first "
         "contents): take that parameter out."},
    };
    for (const auto &each : known) {
        if (notice.find(each.code) == std::string_view::npos) continue;
        if (!each.sentence.starts_with("is ") && !each.sentence.starts_with("has ") &&
            !each.sentence.starts_with("asks "))
            return std::string{each.sentence};
        // The layer it names, when the notice carries the refusal's detail.
        std::string layer;
        constexpr std::string_view key = "\"layer\":\"";
        if (const auto at = notice.find(key); at != std::string_view::npos) {
            const auto end = notice.find('"', at + key.size());
            if (end != std::string_view::npos) layer = std::string{notice.substr(at + key.size(), end - at - key.size())};
        }
        return (layer.empty() ? std::string{"A layer "} : "Layer \"" + layer + "\" ") + std::string{each.sentence};
    }
    return {};
}

const std::vector<GraphCanvasSwatch> &graphCanvasSwatches() {
    static const std::vector<GraphCanvasSwatch> swatches{
        {"Blue", {0.2, 0.45, 0.9, 1.0}},   {"Orange", {0.95, 0.55, 0.15, 1.0}},
        {"Red", {0.9, 0.2, 0.2, 1.0}},     {"Green", {0.25, 0.75, 0.35, 1.0}},
        {"Yellow", {0.95, 0.85, 0.2, 1.0}}, {"Purple", {0.6, 0.35, 0.85, 1.0}},
        {"White", {1.0, 1.0, 1.0, 1.0}},   {"Grey", {0.5, 0.5, 0.5, 1.0}},
        {"Black", {0.0, 0.0, 0.0, 1.0}},
    };
    return swatches;
}

// --- SG-3.

const std::vector<GraphCanvasBlendChoice> &graphCanvasBlendChoices() {
    static const std::vector<GraphCanvasBlendChoice> choices = [] {
        struct Known {
            std::string_view name;
            std::string_view label;
            int group;
        };
        // Photoshop's words and menu groups.
        static constexpr Known known[] = {
            {"normal", "Normal", 0},
            {"darken", "Darken", 1},           {"multiply", "Multiply", 1},
            {"color_burn", "Color Burn", 1},   {"linear_burn", "Linear Burn", 1},
            {"darker_color", "Darker Color", 1},
            {"lighten", "Lighten", 2},         {"screen", "Screen", 2},
            {"color_dodge", "Color Dodge", 2}, {"linear_dodge", "Linear Dodge (Add)", 2},
            {"lighter_color", "Lighter Color", 2},
            {"overlay", "Overlay", 3},         {"soft_light", "Soft Light", 3},
            {"hard_light", "Hard Light", 3},   {"vivid_light", "Vivid Light", 3},
            {"linear_light", "Linear Light", 3}, {"pin_light", "Pin Light", 3},
            {"hard_mix", "Hard Mix", 3},
            {"difference", "Difference", 4},   {"exclusion", "Exclusion", 4},
            {"subtract", "Subtract", 4},       {"divide", "Divide", 4},
            {"hue", "Hue", 5},                 {"saturation", "Saturation", 5},
            {"color", "Color", 5},             {"luminosity", "Luminosity", 5},
        };
        std::vector<GraphCanvasBlendChoice> made;
        for (const auto &name : Pelican::canvasBlendNames()) {
            const auto found = std::ranges::find(known, std::string_view{name}, &Known::name);
            made.push_back(found != std::end(known)
                               ? GraphCanvasBlendChoice{name, std::string{found->label}, found->group}
                               : GraphCanvasBlendChoice{name, name, 6});
        }
        // By group; within one, the engine's order.
        std::ranges::stable_sort(made, std::less<>{}, &GraphCanvasBlendChoice::group);
        return made;
    }();
    return choices;
}

namespace {

constexpr std::string_view kTextDefaultsAlign = "left";

// The first line of a text, trimmed, as a layer's name (at most 32 bytes,
// not cutting a UTF-8 sequence).
std::string textLayerName(const std::string &text) {
    auto line = text.substr(0, text.find('\n'));
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (line.size() > 32) {
        std::size_t cut = 32;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
        line = line.substr(0, cut) + "\xE2\x80\xA6";
    }
    return line;
}

// The parameter left out when it is the engine's default, else set.
Json setOrDefault(const GraphCanvasNode &node, std::string_view scope, std::string_view key, const Json &value,
                  const Json &fallback) {
    auto parameters = node.parameters.is_object() ? node.parameters : Json::object();
    const std::string member{key};
    const bool is_default = value.is_number() && fallback.is_number()
                                ? std::abs(value.get<double>() - fallback.get<double>()) < 1e-9
                                : value == fallback;
    if (is_default) parameters.erase(member);
    else parameters[member] = value;
    if (parameters == node.parameters) return Json::array();
    return Json::array({setParameters(scope, node.id, std::move(parameters))});
}

const GraphCanvasStackRow *rowAt(const std::vector<GraphCanvasStackRow> &rows, std::size_t index) {
    const auto found = std::ranges::find(rows, index, &GraphCanvasStackRow::index);
    return found == rows.end() ? nullptr : &*found;
}

} // namespace

Json graphCanvasAddTextLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                std::string_view stack, const std::string &text, const std::string &font,
                                double size, std::optional<std::size_t> above) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr || font.empty() || !(size > 0.0)) return Json::array();
    const auto numbered = numberedId(nodes, target, "text", {"_fill"});
    if (numbered.number == 0) return Json::array();
    const auto fill = numbered.id + "_fill";
    const auto canvas = stackSize(*target);
    auto name = textLayerName(text);
    if (name.empty()) name = "Text " + std::to_string(numbered.number);
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", numbered.id}, {"op", "shape.text"},
                                      {"parameters", Json{{"font", font},
                                                          {"text", text},
                                                          {"size", size},
                                                          {"origin", Json::array({canvas[0] / 2.0, canvas[1] / 2.0})},
                                                          {"align", "center"}}},
                                      {"outputs", Json::array({"geometry"})}}));
    ops.push_back(addNode(scope, Json{{"id", fill}, {"op", "shape.fill"},
                                      {"parameters", Json{{"color", colorJson(swatch("White").color)}}},
                                      {"inputs", Json{{"geometry", edge(numbered.id, "geometry")}}},
                                      {"outputs", Json::array({"layer"})}}));
    append(ops, addLayerOps(scope, *target, aboveAt(*target, above), numbered.id, name, fill, "layer"));
    return ops;
}

Json graphCanvasSetTextParameterOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                    std::string_view node, std::string_view key, const Json &value) {
    const auto *found = findNode(nodes, node);
    if (found == nullptr || found->op != "shape.text") return Json::array();
    if (key == "align") {
        if (!value.is_string() || (value != "left" && value != "center" && value != "right")) return Json::array();
        return setOrDefault(*found, scope, key, value, Json(kTextDefaultsAlign));
    }
    if (key == "line_height" && value.is_number()) return setOrDefault(*found, scope, key, value, Json(1.2));
    if (key == "tracking" && value.is_number()) return setOrDefault(*found, scope, key, value, Json(0.0));
    if (key == "box_width" && value.is_number() && value.get<double>() >= 0.0)
        return setOrDefault(*found, scope, key, value, Json(0.0));
    if ((key == "text" && value.is_string()) || (key == "font" && value.is_string() && !value.get_ref<const std::string &>().empty()) ||
        (key == "size" && value.is_number() && value.get<double>() > 0.0))
        return graphCanvasSetParameterOps(nodes, scope, node, key, value);
    return Json::array();
}

const std::vector<double> &graphCanvasTextSizes() {
    static const std::vector<double> sizes{6, 8, 10, 12, 14, 18, 24, 30, 36, 48, 60, 72, 96, 120, 144, 200, 288};
    return sizes;
}

std::optional<std::size_t> graphCanvasStrokeRowOf(const std::vector<GraphCanvasNode> &nodes,
                                                  std::string_view stack, std::size_t index) {
    const auto rows = graphCanvasLayerRows(nodes, stack);
    const auto *row = rowAt(rows, index);
    if (row == nullptr || row->shape.empty()) return std::nullopt;
    for (const auto &other : rows)
        if (other.paint == "stroke" && other.shape == row->shape && other.index != index) return other.index;
    return std::nullopt;
}

const std::vector<GraphCanvasFilterChoice> &graphCanvasFilterChoices() {
    static const std::vector<GraphCanvasFilterChoice> choices = [] {
        std::vector<GraphCanvasFilterChoice> made;
        for (const auto &op : Pelican::canvasFilterOperations()) made.push_back({op, graphCanvasFilterLabel(op)});
        return made;
    }();
    return choices;
}

const std::vector<GraphCanvasFilterChoice> &graphCanvasAdjustmentChoices() {
    static const std::vector<GraphCanvasFilterChoice> choices = [] {
        std::vector<GraphCanvasFilterChoice> made;
        for (const auto &op : Pelican::canvasColorFilterOperations()) made.push_back({op, graphCanvasFilterLabel(op)});
        return made;
    }();
    return choices;
}

std::string graphCanvasFilterLabel(std::string_view op) {
    static constexpr std::pair<std::string_view, std::string_view> labels[] = {
        {"filter.invert", "Invert"},
        {"filter.brightness_contrast", "Brightness/Contrast"},
        {"filter.levels", "Levels"},
        {"filter.hue_saturation", "Hue/Saturation"},
        {"filter.gradient_map", "Gradient Map"},
        {"filter.color_overlay", "Color Overlay"},
        // CV-3a: Photoshop's words.
        {"filter.blur", "Gaussian Blur"},
        {"filter.drop_shadow", "Drop Shadow"},
        {"filter.outer_glow", "Outer Glow"},
    };
    for (const auto &[known, label] : labels)
        if (known == op) return std::string{label};
    return std::string{op};
}

Json graphCanvasFilterDefaults(std::string_view op) {
    // CV-3a: the engine's defaults, which are Photoshop's (a drop shadow 5 px
    // away at 120 degrees, size 5, black at 75 %; an outer glow size 5,
    // (255, 255, 190) at 75 %; a Gaussian Blur of 1 px).
    if (op == "filter.blur") return Json{{"radius", 1.0}};
    if (op == "filter.drop_shadow")
        return Json{{"offset", Json::array({2.5, 4.330127018922193})}, {"radius", 5.0}, {"spread", 0.0},
                    {"color", Json::array({0.0, 0.0, 0.0, 1.0})}, {"opacity", 0.75}};
    if (op == "filter.outer_glow")
        return Json{{"radius", 5.0}, {"spread", 0.0}, {"color", Json::array({1.0, 1.0, 190.0 / 255.0, 1.0})},
                    {"opacity", 0.75}};
    if (!Pelican::isCanvasColorFilter(op)) return nullptr;
    if (op == "filter.brightness_contrast") return Json{{"brightness", 0.0}, {"contrast", 0.0}};
    if (op == "filter.levels")
        return Json{{"input_floor", 0.0}, {"input_ceiling", 255.0}, {"gamma", 1.0},
                    {"output_floor", 0.0}, {"output_ceiling", 255.0}};
    if (op == "filter.hue_saturation") return Json{{"hue", 0.0}, {"saturation", 0.0}, {"lightness", 0.0}};
    if (op == "filter.gradient_map")
        return Json{{"stops", Json::array({Json{{"position", 0.0}, {"color", Json::array({0.0, 0.0, 0.0})}},
                                           Json{{"position", 1.0}, {"color", Json::array({1.0, 1.0, 1.0})}}})}};
    if (op == "filter.color_overlay") return Json{{"color", colorJson(swatch("Red").color)}};
    return Json::object();
}

namespace {

// `<base>_<n>` from a filter op: invert, brightness_contrast...
std::string filterBase(std::string_view op) { return std::string{op.substr(op.find('.') + 1)}; }

} // namespace

Json graphCanvasAddAdjustmentLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                      std::string_view stack, std::optional<std::size_t> above, std::string_view op) {
    const auto *target = stackNode(nodes, stack);
    const auto parameters = graphCanvasFilterDefaults(op);
    // CV-3a: an adjustment is a colour filter (a blur of the backdrop is refused).
    if (target == nullptr || parameters.is_null() || !Pelican::isCanvasColorFilter(op)) return Json::array();
    const auto numbered = numberedId(nodes, target, filterBase(op), {"_backdrop"});
    if (numbered.number == 0) return Json::array();
    const auto backdrop = numbered.id + "_backdrop";
    const auto at = aboveAt(*target, above);
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", backdrop}, {"op", "canvas.backdrop"}, {"parameters", Json::object()},
                                      {"outputs", Json::array({"layer"})}}));
    ops.push_back(addNode(scope, Json{{"id", numbered.id}, {"op", std::string{op}}, {"parameters", parameters},
                                      {"inputs", Json{{"layer", edge(backdrop, "layer")}}},
                                      {"outputs", Json::array({"layer"})}}));
    append(ops, addLayerOps(scope, *target, at, numbered.id,
                            graphCanvasFilterLabel(op) + " " + std::to_string(numbered.number), numbered.id,
                            "layer"));
    return ops;
}

Json graphCanvasAddFilterOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                             std::string_view stack, std::size_t index, std::string_view op) {
    const auto *target = stackNode(nodes, stack);
    const auto parameters = graphCanvasFilterDefaults(op);
    if (target == nullptr || parameters.is_null()) return Json::array();
    const auto rows = graphCanvasLayerRows(nodes, stack);
    const auto *row = rowAt(rows, index);
    const auto *line = row == nullptr || row->input.empty() ? nullptr : findPort(*target, row->input);
    if (line == nullptr || !line->connected) return Json::array();
    const auto id = freeId(nodes, target, row->input + "_" + filterBase(op));
    Json ops = Json::array();
    ops.push_back(addNode(scope, Json{{"id", id}, {"op", std::string{op}}, {"parameters", parameters},
                                      {"inputs", Json{{"layer", edge(line->source_node, line->source_port)}}},
                                      {"outputs", Json::array({"layer"})}}));
    ops.push_back(setInput(scope, target->id, row->input, edge(id, "layer")));
    return ops;
}

Json graphCanvasRemoveFilterOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                std::string_view stack, std::size_t index, std::string_view filter) {
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr) return Json::array();
    const auto rows = graphCanvasLayerRows(nodes, stack);
    const auto *row = rowAt(rows, index);
    if (row == nullptr || std::ranges::find(row->filters, filter) == row->filters.end()) return Json::array();
    if (row->kind == "adjustment" && row->filters.size() == 1) return Json::array();
    const auto *node = findNode(nodes, filter);
    const auto *read = node == nullptr ? nullptr : findPort(*node, "layer");
    if (read == nullptr || !read->connected) return Json::array();
    // Its reader: the stack's input, or a filter or transform above it.
    auto reconnect = setInput(scope, target->id, row->input, edge(read->source_node, read->source_port));
    for (const auto &above : nodes)
        if ((above.op.starts_with("filter.") || above.op == "canvas.transform") && sourceOf(above, "layer") == filter &&
            (std::ranges::find(row->filters, above.id) != row->filters.end() || above.id == row->transform))
            reconnect = setInput(scope, above.id, "layer", edge(read->source_node, read->source_port));
    return Json::array({std::move(reconnect),
                        Json{{"op", "remove_node"}, {"scope", scopeToken(scope)}, {"node", std::string{filter}}}});
}

std::vector<GraphCanvasScenePass> graphCanvasScenePasses(const std::vector<GraphCanvasNode> &nodes) {
    std::vector<GraphCanvasScenePass> passes;
    for (const auto &node : nodes) {
        if (node.op != "render.material_pass") continue;
        // The depth it hands on is an image only when one is attached.
        const auto *depth = findPort(node, "depth");
        const bool has_depth = depth != nullptr && depth->connected &&
                               std::ranges::any_of(node.outputs, [](const GraphCanvasOutput &output) {
                                   return output.port == "depth";
                               });
        for (const auto &output : node.outputs)
            if (output.port != "depth" && output.port != "draws")
                passes.push_back({node.id, output.port, has_depth ? std::string{"depth"} : std::string{}});
    }
    return passes;
}

std::vector<std::string> graphCanvasPlacesOf(const std::vector<GraphCanvasNode> &nodes, std::string_view stack) {
    std::vector<std::string> places;
    for (const auto &node : nodes)
        if (node.op == "canvas.place" && sourceOf(node, "layer") == stack) places.push_back(node.id);
    return places;
}

Json graphCanvasPlaceOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope, std::string_view stack,
                         const GraphCanvasScenePass &pass) {
    const auto *target = stackNode(nodes, stack);
    const auto *drawn = findNode(nodes, pass.node);
    if (target == nullptr || drawn == nullptr || drawn->op != "render.material_pass" || pass.color.empty())
        return Json::array();
    const auto canvas = stackSize(*target);
    Json inputs{{"layer", edge(target->id, "layer")}, {"color", edge(pass.node, pass.color)}};
    if (!pass.depth.empty()) inputs["depth"] = edge(pass.node, pass.depth);
    return Json::array({addNode(scope, Json{{"id", freeId(nodes, nullptr, target->id + "_place")},
                                            {"op", "canvas.place"},
                                            {"parameters", Json{{"position", Json::array({0.0, 0.0, 0.0})},
                                                                {"rotation", Json::array({0.0, 0.0, 0.0})},
                                                                {"scale", 1.0},
                                                                {"pixels_per_unit", canvas[0] / 2.0}}},
                                            {"inputs", std::move(inputs)},
                                            {"outputs", Json::array({"color"})}})});
}

Json graphCanvasSetPlaceParameterOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                     std::string_view place, std::string_view key, const Json &value) {
    const auto *found = findNode(nodes, place);
    if (found == nullptr || found->op != "canvas.place") return Json::array();
    const auto triple = value.is_array() && value.size() == 3 &&
                        std::ranges::all_of(value, [](const Json &each) { return each.is_number(); });
    if (key == "position" && triple) return graphCanvasSetParameterOps(nodes, scope, place, key, value);
    if (key == "rotation" && triple)
        return setOrDefault(*found, scope, key, value, Json::array({0.0, 0.0, 0.0}));
    if (key == "scale" && value.is_number()) return setOrDefault(*found, scope, key, value, Json(1.0));
    if (key == "pixels_per_unit" && value.is_number() && value.get<double>() > 0.0)
        return graphCanvasSetParameterOps(nodes, scope, place, key, value);
    if (key == "anchor" && value.is_array() && value.size() == 2 && value.at(0).is_number() && value.at(1).is_number())
        return setOrDefault(*found, scope, key, value, Json::array({0.5, 0.5}));
    if (key == "depth_test" && value.is_boolean()) return setOrDefault(*found, scope, key, value, Json(true));
    if (key == "depth_write" && value.is_boolean()) return setOrDefault(*found, scope, key, value, Json(false));
    if (key == "double_sided" && value.is_boolean()) return setOrDefault(*found, scope, key, value, Json(true));
    return Json::array();
}

} // namespace PelicanStudio
