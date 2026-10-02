#include "graphcanvaslayerstack.hpp"

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
        row.source = sourceOf(*found, row.input);
        if (const auto *source = row.source.empty() ? nullptr : findNode(nodes, row.source)) {
            if (source->op == "canvas.stack") {
                row.kind = "group";
            } else if (source->op == "shape.fill" || source->op == "shape.stroke") {
                row.paint = source->op == "shape.fill" ? "fill" : "stroke";
                row.shape = sourceOf(*source, "geometry");
                if (const auto *shape = row.shape.empty() ? nullptr : findNode(nodes, row.shape)) {
                    if (shape->op == "shape.rect") row.kind = "rect";
                    else if (shape->op == "shape.ellipse") row.kind = "ellipse";
                }
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string graphCanvasLayerKindText(const GraphCanvasStackRow &row) {
    auto text = capitalized(row.kind);
    if (row.paint == "stroke") text += " stroke";
    return text;
}

std::string graphCanvasStackFor(const std::vector<GraphCanvasNode> &nodes, std::string_view node) {
    const auto *found = findNode(nodes, node);
    if (found == nullptr) return {};
    if (found->op == "canvas.stack") return found->id;
    if (found->op == "canvas.output") {
        const auto source = sourceOf(*found, "layer");
        return stackNode(nodes, source) != nullptr ? source : std::string{};
    }
    // The paints a picked shape is drawn by, or the picked paint itself.
    std::vector<std::string> paints;
    if (found->op == "shape.fill" || found->op == "shape.stroke") {
        paints.push_back(found->id);
    } else if (found->op == "shape.rect" || found->op == "shape.ellipse") {
        for (const auto &each : nodes)
            if ((each.op == "shape.fill" || each.op == "shape.stroke") &&
                sourceOf(each, "geometry") == found->id)
                paints.push_back(each.id);
    } else {
        return {};
    }
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
        if (row.source == node || (!row.shape.empty() && row.shape == node)) return row.index;
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
                                 std::string_view stack, std::string_view shape) {
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
    append(ops, addLayerOps(scope, *target, layersOf(*target).size(), numbered.id,
                            capitalized(shape) + " " + std::to_string(numbered.number), fill, "layer"));
    return ops;
}

Json graphCanvasAddGroupLayerOps(const std::vector<GraphCanvasNode> &nodes, std::string_view scope,
                                 std::string_view stack) {
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
    append(ops, addLayerOps(scope, *target, layersOf(*target).size(), numbered.id,
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
    auto parameters = target->parameters;
    layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(index));
    parameters["layers"] = std::move(layers);
    Json ops = Json::array({setParameters(scope, target->id, std::move(parameters))});
    if (!sourceOf(*target, input).empty()) ops.push_back(setInput(scope, target->id, input, nullptr));
    if (!sourceOf(*target, opacityPort(input)).empty())
        ops.push_back(setInput(scope, target->id, opacityPort(input), nullptr));
    // The canvas nodes upstream of the layer that nothing else reads: a node
    // goes once every reader of it has gone (or is the line cut here), which
    // removes readers before what they read.
    const auto source = sourceOf(*target, input);
    std::vector<std::string> upstream;
    if (!source.empty()) {
        std::vector<std::string> pending{source};
        while (!pending.empty()) {
            auto id = std::move(pending.back());
            pending.pop_back();
            const auto *node = findNode(nodes, id);
            if (node == nullptr || !isGraphCanvasOperation(node->op) || node->op == "canvas.output" ||
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
                    return !(reader.id == target->id && (port.port == input || port.port == opacityPort(input)));
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
    const auto *target = stackNode(nodes, stack);
    if (target == nullptr || (delta != 1 && delta != -1)) return Json::array();
    auto layers = layersOf(*target);
    if (index >= layers.size()) return Json::array();
    if (delta < 0 && index == 0) return Json::array();
    const auto other = delta > 0 ? index + 1 : index - 1;
    if (other >= layers.size()) return Json::array();
    std::swap(layers.at(index), layers.at(other));
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
    const auto *source = findNode(nodes, source_node);
    if (target == nullptr || source == nullptr || source_node == target->id) return Json::array();
    // The input is named after what it reads (no dot: the engine refuses one).
    auto base = source_node;
    std::ranges::replace(base, '.', '_');
    const auto input = freeId({}, target, base);
    return addLayerOps(scope, *target, layersOf(*target).size(), input, source->heading(),
                       source_node, source_port);
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

} // namespace PelicanStudio
