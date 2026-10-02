#include "canvaslowering.hpp"

#include "../graphcomposition.hpp"
#include "canvasmodule.hpp"

#include <picosha2.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace Pelican {
namespace {
using Json = nlohmann::json;

const char *const default_canvas_format = "R16G16B16A16_SFLOAT";

std::string nodeId(const Json &node) {
    const auto found = node.find("id");
    return found == node.end() || !found->is_string() ? std::string{} : found->get<std::string>();
}

std::string nodeOp(const Json &node) {
    const auto found = node.find("op");
    return found == node.end() || !found->is_string() ? std::string{} : found->get<std::string>();
}

const Json &emptyObject() {
    static const Json empty = Json::object();
    return empty;
}

const Json &objectMember(const Json &node, const char *key) {
    const auto found = node.find(key);
    return found != node.end() && found->is_object() ? *found : emptyObject();
}

double number(const Json &value, const std::string &what) {
    if (!value.is_number()) throw std::invalid_argument(what + " must be a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) throw std::invalid_argument(what + " must be finite");
    return result;
}

// The keys each canvas op takes; anything else is refused rather than ignored.
void onlyKeys(const Json &node, std::initializer_list<const char *> keys) {
    for (const auto &[key, value] : objectMember(node, "parameters").items()) {
        (void)value;
        if (std::none_of(keys.begin(), keys.end(), [&](const char *known) { return key == known; }))
            throw std::invalid_argument(nodeOp(node) + " does not take " + key + ": " + nodeId(node));
    }
}

// The nodes of one scope (the root or a definition body), by id.
struct Scope {
    std::string name;
    std::map<std::string, const Json *> nodes;
};

Scope indexScope(const std::string &name, const Json &nodes) {
    Scope scope{name, {}};
    if (!nodes.is_array()) return scope;
    for (const auto &node : nodes)
        if (node.is_object()) scope.nodes.emplace(nodeId(node), &node);
    return scope;
}

const Json *producer(const Scope &scope, const Json &endpoint) {
    if (!endpoint.is_object()) return nullptr;
    const auto node = endpoint.find("node");
    if (node == endpoint.end() || !node->is_string()) return nullptr;
    const auto found = scope.nodes.find(node->get<std::string>());
    return found == scope.nodes.end() ? nullptr : found->second;
}

// A value edge into a canvas parameter: a plain {node, port} from a node
// that stays in the lowered document.
CanvasValueSource valueSource(const Scope &scope, const Json &consumer, const std::string &port,
                              const Json &endpoint) {
    const auto where = nodeId(consumer) + "." + port;
    if (!endpoint.is_object() || endpoint.size() != 2 || !endpoint.contains("node") || !endpoint.contains("port") ||
        !endpoint.at("node").is_string() || !endpoint.at("port").is_string())
        throw std::invalid_argument("a canvas parameter takes a plain {node, port} value edge: " + where);
    if (const auto *from = producer(scope, endpoint); from != nullptr && isCanvasGraphOperation(nodeOp(*from)))
        throw std::invalid_argument("a canvas parameter takes a value edge, not a canvas node: " + where);
    return {endpoint.at("node").get<std::string>(), endpoint.at("port").get<std::string>()};
}

// One number of a node: the edge on `port` when there is one, else the
// parameters' value (component `component` of an array when given), else the
// fallback. A component edge leaves the array's number as its first value.
CanvasParameter parameter(const Scope &scope, const Json &node, const char *key, std::optional<std::size_t> arity,
                          std::size_t component, const std::string &port, std::optional<double> fallback) {
    const auto id = nodeId(node);
    const auto &parameters = objectMember(node, "parameters");
    std::optional<double> authored;
    if (const auto found = parameters.find(key); found != parameters.end()) {
        if (arity) {
            if (!found->is_array() || found->size() != *arity)
                throw std::invalid_argument(nodeOp(node) + " " + key + " must be an array of " +
                                            std::to_string(*arity) + " numbers: " + id);
            authored = number(found->at(component), nodeOp(node) + " " + key + ": " + id);
        } else {
            authored = number(*found, nodeOp(node) + " " + key + ": " + id);
        }
    }
    CanvasParameter result;
    const auto &inputs = objectMember(node, "inputs");
    if (const auto edge = inputs.find(port); edge != inputs.end()) {
        result.edge = valueSource(scope, node, port, *edge);
        result.value = authored.value_or(fallback.value_or(0.0));
        return result;
    }
    if (!authored && !fallback) throw std::invalid_argument(nodeOp(node) + " requires " + key + ": " + id);
    result.value = authored ? *authored : *fallback;
    return result;
}

void appendVector(std::vector<CanvasParameter> &out, const Scope &scope, const Json &node, const char *key,
                  std::initializer_list<const char *> components, std::optional<double> fallback = std::nullopt) {
    std::size_t index = 0;
    for (const auto *component : components) {
        out.push_back(parameter(scope, node, key, components.size(), index, std::string{key} + "." + component,
                                fallback));
        ++index;
    }
}

std::optional<std::array<double, 2>> positiveSize(const Json &parameters, const std::string &what) {
    const auto found = parameters.find("size");
    if (found == parameters.end()) return std::nullopt;
    if (!found->is_array() || found->size() != 2)
        throw std::invalid_argument(what + " size must be [width, height]");
    const std::array<double, 2> size{number(found->at(0), what + " size"), number(found->at(1), what + " size")};
    if (size[0] <= 0.0 || size[1] <= 0.0) throw std::invalid_argument(what + " size must be positive");
    return size;
}

Json layerName(const Json &layer, std::size_t index) {
    const auto name = layer.find("name");
    return name != layer.end() && name->is_string() ? *name : Json(index);
}

struct Flattening {
    double opacity = 1.0;
    std::vector<CanvasValueSource> opacity_edges;
    std::vector<std::string> stacks;
};

void appendLayer(const Scope &scope, const Json &endpoint, const Flattening &flattening,
                 std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items);

void appendPaint(const Scope &scope, const Json &paint, const Flattening &flattening, std::vector<CanvasItem> &items) {
    const auto op = nodeOp(paint);
    const auto id = nodeId(paint);
    const bool stroke = op == "shape.stroke";
    if (stroke) onlyKeys(paint, {"color", "width", "align"});
    else onlyKeys(paint, {"color"});
    const auto &inputs = objectMember(paint, "inputs");
    const auto geometry_edge = inputs.find("geometry");
    const auto *shape = geometry_edge == inputs.end() ? nullptr : producer(scope, *geometry_edge);
    if (shape == nullptr || (nodeOp(*shape) != "shape.rect" && nodeOp(*shape) != "shape.ellipse"))
        throw std::invalid_argument(op + " requires a geometry edge from shape.rect or shape.ellipse: " + id);
    CanvasItem item;
    item.shape = nodeOp(*shape) == "shape.rect" ? CanvasShape::rect : CanvasShape::ellipse;
    item.paint = stroke ? CanvasPaint::stroke : CanvasPaint::fill;
    if (item.shape == CanvasShape::rect) onlyKeys(*shape, {"size", "center", "corner_radius", "rotation"});
    else onlyKeys(*shape, {"size", "center", "rotation"});
    appendVector(item.parameters, scope, *shape, "size", {"x", "y"});
    appendVector(item.parameters, scope, *shape, "center", {"x", "y"});
    if (item.shape == CanvasShape::rect)
        item.parameters.push_back(parameter(scope, *shape, "corner_radius", std::nullopt, 0, "corner_radius", 0.0));
    item.parameters.push_back(parameter(scope, *shape, "rotation", std::nullopt, 0, "rotation", 0.0));
    appendVector(item.parameters, scope, paint, "color", {"r", "g", "b", "a"});
    if (stroke) {
        item.parameters.push_back(parameter(scope, paint, "width", std::nullopt, 0, "width", std::nullopt));
        // `align` rebuilds; it is a constant of the bake table, never an edge.
        double align = 0.0;
        if (const auto found = objectMember(paint, "parameters").find("align");
            found != objectMember(paint, "parameters").end()) {
            if (*found == "center") align = 0.0;
            else if (*found == "inner") align = 1.0;
            else if (*found == "outer") align = 2.0;
            else throw std::invalid_argument("shape.stroke align must be center, inner or outer: " + id);
        }
        item.parameters.push_back(CanvasParameter{align, std::nullopt});
    }
    item.opacity = flattening.opacity;
    item.opacity_edges = flattening.opacity_edges;
    item.nodes = {id, nodeId(*shape)};
    item.nodes.insert(item.nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
    items.push_back(std::move(item));
}

void appendStack(const Scope &scope, const Json &stack, const Flattening &flattening,
                 std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    const auto id = nodeId(stack);
    if (std::find(active.begin(), active.end(), id) != active.end()) {
        auto cycle = Json::array();
        for (auto at = std::find(active.begin(), active.end(), id); at != active.end(); ++at) cycle.push_back(*at);
        cycle.push_back(id);
        throw GraphRegistryError("canvas.stack_cycle@1", {{"node", id}, {"cycle", std::move(cycle)}});
    }
    onlyKeys(stack, {"size", "layers"});
    const auto &parameters = objectMember(stack, "parameters");
    (void)positiveSize(parameters, "canvas.stack " + id);
    const auto layers = parameters.find("layers");
    if (layers == parameters.end() || !layers->is_array())
        throw std::invalid_argument("canvas.stack requires a layers array: " + id);
    active.push_back(id);
    const auto &inputs = objectMember(stack, "inputs");
    for (std::size_t index = 0; index < layers->size(); ++index) {
        const auto &layer = layers->at(index);
        if (!layer.is_object()) throw std::invalid_argument("canvas.stack layer must be an object: " + id);
        for (const auto &[key, value] : layer.items()) {
            (void)value;
            if (key != "input" && key != "name" && key != "visible" && key != "opacity" && key != "blend")
                throw std::invalid_argument("canvas.stack layer does not take " + key + ": " + id);
        }
        const auto input = layer.find("input");
        if (input == layer.end() || !input->is_string() || input->get_ref<const std::string &>().empty())
            throw std::invalid_argument("canvas.stack layer requires an input name: " + id);
        const auto name = input->get<std::string>();
        if (const auto blend = layer.find("blend"); blend != layer.end() && *blend != "normal")
            throw GraphRegistryError("canvas.blend_unsupported@1",
                                     {{"node", id}, {"layer", layerName(layer, index)}, {"blend", *blend}});
        const auto edge = inputs.find(name);
        if (edge == inputs.end())
            throw GraphRegistryError("canvas.layer_input_missing@1",
                                     {{"node", id}, {"layer", layerName(layer, index)}, {"input", name}});
        if (const auto visible = layer.find("visible"); visible != layer.end()) {
            if (!visible->is_boolean())
                throw std::invalid_argument("canvas.stack layer visible must be a boolean: " + id);
            if (!visible->get<bool>()) continue;
        }
        Flattening inner = flattening;
        inner.stacks.insert(inner.stacks.begin(), id);
        const auto opacity_port = canvasStackOpacityPort(name);
        bool group_opacity = false;
        if (const auto opacity_edge = inputs.find(opacity_port); opacity_edge != inputs.end()) {
            inner.opacity_edges.push_back(valueSource(scope, stack, opacity_port, *opacity_edge));
            group_opacity = true;
        } else if (const auto opacity = layer.find("opacity"); opacity != layer.end()) {
            const auto value = number(*opacity, "canvas.stack layer opacity: " + id);
            inner.opacity *= value;
            group_opacity = value != 1.0;
        }
        const auto *from = producer(scope, *edge);
        if (from == nullptr)
            throw GraphRegistryError("canvas.layer_input_missing@1",
                                     {{"node", id}, {"layer", layerName(layer, index)}, {"input", name}});
        // Blend is normal only, so a nested stack's layers join this one's in
        // order. Its opacity is multiplied into each of its items rather than
        // applied to the group drawn as one picture.
        if (group_opacity && nodeOp(*from) == "canvas.stack")
            ir.diagnostics.push_back({{"code", "canvas.group_opacity_flattened"}, {"node", nodeId(*from)}});
        appendLayer(scope, *edge, inner, active, ir, items);
    }
    active.pop_back();
}

void appendLayer(const Scope &scope, const Json &endpoint, const Flattening &flattening,
                 std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    const auto *from = producer(scope, endpoint);
    if (from == nullptr) throw std::invalid_argument("canvas layer edge names no node of its scope");
    const auto op = nodeOp(*from);
    if (op == "shape.fill" || op == "shape.stroke") return appendPaint(scope, *from, flattening, items);
    if (op == "canvas.stack") return appendStack(scope, *from, flattening, active, ir, items);
    throw std::invalid_argument("a canvas layer comes from shape.fill, shape.stroke or canvas.stack, not " + op +
                                ": " + nodeId(*from));
}

CanvasOutput buildOutput(const Scope &scope, const Json &node, CanvasIr &ir) {
    const auto id = nodeId(node);
    onlyKeys(node, {"extent_scale", "size", "format", "fit"});
    const auto &parameters = objectMember(node, "parameters");
    CanvasOutput output;
    output.id = id;
    output.scope = scope.name;
    if (parameters.contains("size") && parameters.contains("extent_scale"))
        throw std::invalid_argument("canvas.output takes size or extent_scale, not both: " + id);
    if (const auto size = parameters.find("size"); size != parameters.end()) {
        if (!size->is_array() || size->size() != 2 || !size->at(0).is_number_unsigned() ||
            !size->at(1).is_number_unsigned() || size->at(0) == 0 || size->at(1) == 0)
            throw std::invalid_argument("canvas.output size must be [width, height] in pixels: " + id);
        output.image_size = std::array<std::uint32_t, 2>{size->at(0).get<std::uint32_t>(),
                                                         size->at(1).get<std::uint32_t>()};
    }
    if (const auto scale = parameters.find("extent_scale"); scale != parameters.end()) {
        output.extent_scale = number(*scale, "canvas.output extent_scale: " + id);
        if (output.extent_scale <= 0.0) throw std::invalid_argument("canvas.output extent_scale must be positive: " + id);
    }
    output.format = default_canvas_format;
    if (const auto format = parameters.find("format"); format != parameters.end()) {
        if (!format->is_string() || format->get_ref<const std::string &>().empty())
            throw std::invalid_argument("canvas.output format must be a format name: " + id);
        output.format = format->get<std::string>();
    }
    if (const auto fit = parameters.find("fit"); fit != parameters.end()) {
        if (*fit == "contain") output.fit = CanvasFit::contain;
        else if (*fit == "stretch") output.fit = CanvasFit::stretch;
        else throw std::invalid_argument("canvas.output fit must be contain or stretch: " + id);
    }
    const auto &inputs = objectMember(node, "inputs");
    const auto layer = inputs.find("layer");
    const auto *from = layer == inputs.end() ? nullptr : producer(scope, *layer);
    if (from == nullptr) throw GraphRegistryError("canvas.output_unconnected@1", {{"node", id}});
    // The stack it reads is the canvas: its size is the canvas's units.
    if (nodeOp(*from) != "canvas.stack")
        throw std::invalid_argument("canvas.output reads a canvas.stack, whose size is the canvas: " + id);
    const auto size = positiveSize(objectMember(*from, "parameters"), "canvas.stack " + nodeId(*from));
    if (!size) throw std::invalid_argument("canvas.stack read by a canvas.output requires a size: " + nodeId(*from));
    output.size = *size;
    std::vector<std::string> active;
    appendLayer(scope, *layer, Flattening{}, active, ir, output.items);
    return output;
}

// graph.loop branch bodies, by definition name.
std::set<std::string> loopBodies(const Json &document) {
    std::set<std::string> bodies;
    const auto nodes = document.find("nodes");
    if (nodes == document.end() || !nodes->is_array()) return bodies;
    for (const auto &node : *nodes) {
        if (nodeOp(node) != "graph.loop") continue;
        const auto branches = objectMember(node, "parameters").find("branches");
        if (branches == objectMember(node, "parameters").end() || !branches->is_array()) continue;
        for (const auto &branch : *branches)
            if (branch.is_object())
                if (const auto body = branch.find("body"); body != branch.end() && body->is_string())
                    bodies.insert(body->get<std::string>());
    }
    return bodies;
}

// The scopes in document order: the root, then each definition by name.
template <typename Visit> void eachScope(const Json &document, Visit visit) {
    visit(std::string{}, document);
    const auto definitions = document.find("definitions");
    if (definitions == document.end() || !definitions->is_object()) return;
    for (const auto &[name, body] : definitions->items())
        if (body.is_object()) visit(name, body);
}

std::string reservedPrefix(const std::string &output) { return output + "$canvas_"; }

void checkScope(const std::string &name, const Json &holder, const std::set<std::string> &bodies) {
    const auto nodes = holder.find("nodes");
    if (nodes == holder.end() || !nodes->is_array()) return;
    std::vector<std::string> outputs;
    for (const auto &node : *nodes) {
        if (!isCanvasGraphOperation(nodeOp(node))) continue;
        if (bodies.contains(name)) throw GraphRegistryError("canvas.in_loop_body@1", {{"node", nodeId(node)}});
        if (nodeOp(node) == "canvas.output") outputs.push_back(nodeId(node));
    }
    for (const auto &node : *nodes) {
        const auto id = nodeId(node);
        for (const auto &output : outputs)
            if (id.starts_with(reservedPrefix(output))) throw GraphRegistryError("canvas.reserved_id@1", {{"id", id}});
    }
}

// --- the tables and the shader -------------------------------------------

std::string glslNumber(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.9g", value);
    std::string text = buffer;
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

std::string fileStem(const std::string &id) {
    std::string stem;
    for (const char c : id)
        stem += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    return stem;
}

std::size_t parameterIndex(CanvasShape shape, std::size_t paint_parameter) {
    return canvasShapeParameterCount(shape) + paint_parameter;
}

const char *const parameter_layout_comment =
    "// Item table, one mat4 per item (column c is it[c]):\n"
    "//   it[0] = (kind, bake offset, source offset or -1, constant opacity)\n"
    "//   it[1] = (opacity edge offset or -1, opacity edge count, 0, 0)\n"
    "//   it[2] = (a, b, c, d)    the 2D linear map from the item's stack to the canvas, mat2 columns\n"
    "//   it[3] = (tx, ty, 0, 0)  its translation\n"
    "// Bake table: an item's parameters from its bake offset, in this order --\n"
    "//   rect: size.x size.y center.x center.y corner_radius rotation(deg)\n"
    "//   ellipse: size.x size.y center.x center.y rotation(deg)\n"
    "//   then fill: color.r color.g color.b color.a (sRGB, straight alpha)\n"
    "//   or stroke: color.r color.g color.b color.a width align(0 center, 1 inner, 2 outer)\n"
    "// At the source offset, one entry per parameter: 0 reads the bake table,\n"
    "// k reads frame table entry k - 1. At the opacity edge offset, the frame\n"
    "// table entries whose values multiply the constant opacity.\n";

std::string parameterFunctions(bool frame) {
    std::string text = R"glsl(float canvasParam(mat4 it, uint i) {
)glsl";
    if (frame)
        text += R"glsl(    if (it[0].z >= 0.0) {
        uint source = uint(pelican_load_bake(uint(it[0].z + 0.5) + i) + 0.5);
        if (source != 0u) return pelican_load_frame(source - 1u);
    }
)glsl";
    text += R"glsl(    return pelican_load_bake(uint(it[0].y + 0.5) + i);
}

float canvasOpacity(mat4 it) {
    float opacity = it[0].w;
)glsl";
    if (frame)
        text += R"glsl(    if (it[1].x >= 0.0) {
        uint first = uint(it[1].x + 0.5);
        uint count = uint(it[1].y + 0.5);
        for (uint k = 0u; k < count; ++k) opacity *= pelican_load_frame(uint(pelican_load_bake(first + k) + 0.5));
    }
)glsl";
    text += R"glsl(    return clamp(opacity, 0.0, 1.0);
}
)glsl";
    return text;
}

const char *const shader_header = R"glsl(#version 460
#extension GL_GOOGLE_include_directive : require
)glsl";

const char *const shader_includes = R"glsl(#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"
)glsl";

std::string vertexShader(const CanvasOutput &output, const std::set<std::uint32_t> &kinds, bool frame) {
    std::string text = shader_header;
    text += "// Generated by the pelican.canvas lowering for canvas.output \"" + output.id + "\". Do not edit.\n";
    text += "// One instance draws one item: 6 vertices, the item's bounding quad, no vertex buffer.\n";
    text += parameter_layout_comment;
    text += shader_includes;
    text += R"glsl(
layout(location = 0) out vec2 vLocal;
layout(location = 1) flat out uint vItem;

)glsl";
    text += "const vec2 CANVAS_SIZE = vec2(" + glslNumber(output.size[0]) + ", " + glslNumber(output.size[1]) + ");\n";
    text += "vec2 canvasTargetSize() {\n";
    if (output.image_size)
        text += "    return vec2(" + glslNumber((*output.image_size)[0]) + ", " + glslNumber((*output.image_size)[1]) +
                ");\n";
    else
        text += "    return pelicanFrame.resolution.xy * " + glslNumber(output.extent_scale) + ";\n";
    text += "}\n\n";
    text += parameterFunctions(frame);
    text += R"glsl(
// The canvas rectangle on the image: canvas units times scale, plus offset (pixels).
void canvasFit(out vec2 scale, out vec2 offset) {
    vec2 target = canvasTargetSize();
    scale = target / CANVAS_SIZE;
)glsl";
    if (output.fit == CanvasFit::contain) text += "    scale = vec2(min(scale.x, scale.y));\n";
    text += R"glsl(    offset = (target - CANVAS_SIZE * scale) * 0.5;
}

float canvasStrokeOutset(float width, float align) {
    return align > 1.5 ? abs(width) : (align > 0.5 ? 0.0 : 0.5 * abs(width));
}

void main() {
    mat4 it = pelican_load_items(uint(gl_InstanceIndex));
    uint kind = uint(it[0].x + 0.5);
    vec2 center = vec2(0.0);
    vec2 bound = vec2(0.0);
    switch (kind) {
)glsl";
    for (const auto kind : kinds) {
        const auto shape = kind < 2 ? CanvasShape::rect : CanvasShape::ellipse;
        const bool stroke = kind % 2 == 1;
        const auto rotation = shape == CanvasShape::rect ? 5 : 4;
        text += "    case " + std::to_string(kind) + "u: { // " + std::string{canvasItemKindName(kind)} + "\n";
        text += "        vec2 half_size = 0.5 * abs(vec2(canvasParam(it, 0u), canvasParam(it, 1u)));\n";
        text += "        center = vec2(canvasParam(it, 2u), canvasParam(it, 3u));\n";
        text += "        float rotation = radians(canvasParam(it, " + std::to_string(rotation) + "u));\n";
        text += "        float c = abs(cos(rotation)), s = abs(sin(rotation));\n";
        if (shape == CanvasShape::rect)
            text += "        bound = vec2(c * half_size.x + s * half_size.y, s * half_size.x + c * half_size.y);\n";
        else
            text += "        bound = vec2(length(vec2(half_size.x * c, half_size.y * s)),\n"
                    "                     length(vec2(half_size.x * s, half_size.y * c)));\n";
        if (stroke)
            text += "        bound += canvasStrokeOutset(canvasParam(it, " + std::to_string(parameterIndex(shape, 4)) +
                    "u), canvasParam(it, " + std::to_string(parameterIndex(shape, 5)) + "u));\n";
        text += "        break;\n    }\n";
    }
    text += R"glsl(    default:
        break;
    }
    vec2 scale, offset;
    canvasFit(scale, offset);
    // Two pixels of room for the antialiased edge, in canvas units.
    float margin = 2.0 / min(scale.x, scale.y);
    const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 local = corners[gl_VertexIndex] * (bound + margin);
    vec2 canvas = mat2(it[2].xy, it[2].zw) * (center + local) + it[3].xy;
    vec2 pixel = offset + canvas * scale;
    gl_Position = vec4(pixel / canvasTargetSize() * 2.0 - 1.0, 0.0, 1.0);
    vLocal = local;
    vItem = uint(gl_InstanceIndex);
}
)glsl";
    return text;
}

std::string fragmentShader(const CanvasOutput &output, const std::set<std::uint32_t> &kinds, bool frame) {
    std::string text = shader_header;
    text += "// Generated by the pelican.canvas lowering for canvas.output \"" + output.id + "\". Do not edit.\n";
    text += "// switch (kind) gives a signed distance d (canvas units, negative inside); its\n"
            "// screen-space gradient sets the antialiasing width. The image is premultiplied.\n";
    text += parameter_layout_comment;
    text += shader_includes;
    text += R"glsl(
layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in uint vItem;
layout(location = 0) out vec4 outColor;

)glsl";
    text += parameterFunctions(frame);
    text += R"glsl(
vec2 canvasRotate(vec2 p, float angle) {
    float c = cos(angle), s = sin(angle);
    return vec2(c * p.x - s * p.y, s * p.x + c * p.y);
}

float canvasRoundRect(vec2 p, vec2 half_size, float radius) {
    vec2 q = abs(p) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// Exact for a circle; near the outline otherwise.
float canvasEllipse(vec2 p, vec2 radii) {
    radii = max(radii, vec2(1e-6));
    float k0 = length(p / radii);
    float k1 = length(p / (radii * radii));
    return k1 > 1e-12 ? k0 * (k0 - 1.0) / k1 : -min(radii.x, radii.y);
}

float canvasStroke(float d, float width, float align) {
    float half_width = 0.5 * abs(width);
    float shift = align > 1.5 ? half_width : (align > 0.5 ? -half_width : 0.0);
    return abs(d - shift) - half_width;
}

vec3 canvasLinear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

void main() {
    mat4 it = pelican_load_items(vItem);
    uint kind = uint(it[0].x + 0.5);
    float d = 1e9;
    vec4 color = vec4(0.0);
    switch (kind) {
)glsl";
    for (const auto kind : kinds) {
        const auto shape = kind < 2 ? CanvasShape::rect : CanvasShape::ellipse;
        const bool stroke = kind % 2 == 1;
        const auto rotation = shape == CanvasShape::rect ? 5 : 4;
        const auto p = [&](std::size_t index) { return "canvasParam(it, " + std::to_string(index) + "u)"; };
        text += "    case " + std::to_string(kind) + "u: { // " + std::string{canvasItemKindName(kind)} + "\n";
        text += "        vec2 half_size = 0.5 * abs(vec2(" + p(0) + ", " + p(1) + "));\n";
        text += "        vec2 q = canvasRotate(vLocal, -radians(" + p(rotation) + "));\n";
        if (shape == CanvasShape::rect)
            text += "        d = canvasRoundRect(q, half_size, clamp(" + p(4) +
                    ", 0.0, min(half_size.x, half_size.y)));\n";
        else
            text += "        d = canvasEllipse(q, half_size);\n";
        if (stroke)
            text += "        d = canvasStroke(d, " + p(parameterIndex(shape, 4)) + ", " + p(parameterIndex(shape, 5)) +
                    ");\n";
        text += "        color = vec4(" + p(parameterIndex(shape, 0)) + ", " + p(parameterIndex(shape, 1)) + ", " +
                p(parameterIndex(shape, 2)) + ", " + p(parameterIndex(shape, 3)) + ");\n";
        text += "        break;\n    }\n";
    }
    text += R"glsl(    default:
        break;
    }
    float aa = max(length(vec2(dFdx(d), dFdy(d))), 1e-6);
    float coverage = clamp(0.5 - d / aa, 0.0, 1.0);
    float alpha = clamp(color.a, 0.0, 1.0) * coverage * canvasOpacity(it);
    if (alpha <= 0.0) discard;
    outColor = vec4(canvasLinear(color.rgb) * alpha, alpha);
}
)glsl";
    return text;
}

Json floatArray(const std::vector<double> &values) {
    auto result = Json::array();
    for (const auto value : values) result.push_back(value);
    return result;
}

Json colorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

Json endpoint(const std::string &node, const std::string &port) { return {{"node", node}, {"port", port}}; }

struct LoweredOutput {
    std::vector<Json> nodes;
    std::vector<CanvasGeneratedSource> sources;
    Json items = Json::array();
};

LoweredOutput lowerOutput(const CanvasOutput &output) {
    LoweredOutput lowered;
    const auto &id = output.id;
    std::vector<double> item_table, bake;
    std::vector<double> frame_initial;
    std::vector<CanvasValueSource> frame_sources;
    std::map<CanvasValueSource, std::size_t> frame_index;
    const auto frameEntry = [&](const CanvasValueSource &source, double initial) {
        const auto [found, inserted] = frame_index.emplace(source, frame_sources.size());
        if (inserted) {
            frame_sources.push_back(source);
            frame_initial.push_back(initial);
        }
        return found->second;
    };
    std::set<std::uint32_t> kinds;
    auto items = output.items;
    // An empty canvas still draws one item (fully transparent), so the item
    // table and the pass keep their shape.
    const bool empty = items.empty();
    if (empty) {
        CanvasItem none;
        none.parameters.assign(canvasShapeParameterCount(CanvasShape::rect) +
                                   canvasPaintParameterCount(CanvasPaint::fill), CanvasParameter{});
        none.opacity = 0.0;
        items.push_back(std::move(none));
    }
    for (std::size_t instance = 0; instance < items.size(); ++instance) {
        const auto &item = items[instance];
        const auto kind = canvasItemKind(item.shape, item.paint);
        kinds.insert(kind);
        const auto bake_offset = static_cast<double>(bake.size());
        for (const auto &parameter : item.parameters) bake.push_back(parameter.value);
        double source_offset = -1.0;
        if (std::any_of(item.parameters.begin(), item.parameters.end(),
                        [](const CanvasParameter &parameter) { return parameter.edge.has_value(); })) {
            source_offset = static_cast<double>(bake.size());
            for (const auto &parameter : item.parameters)
                bake.push_back(parameter.edge ? static_cast<double>(frameEntry(*parameter.edge, parameter.value) + 1)
                                              : 0.0);
        }
        double opacity_offset = -1.0;
        if (!item.opacity_edges.empty()) {
            opacity_offset = static_cast<double>(bake.size());
            for (const auto &edge : item.opacity_edges) bake.push_back(static_cast<double>(frameEntry(edge, 1.0)));
        }
        const double row[16] = {static_cast<double>(kind), bake_offset, source_offset, item.opacity,
                                opacity_offset, static_cast<double>(item.opacity_edges.size()), 0.0, 0.0,
                                1.0, 0.0, 0.0, 1.0,
                                0.0, 0.0, 0.0, 0.0};
        item_table.insert(item_table.end(), std::begin(row), std::end(row));
        if (!empty) {
            auto nodes = Json::array();
            for (const auto &node : item.nodes) nodes.push_back(node);
            lowered.items.push_back({{"output", id}, {"instance", instance},
                                     {"kind", std::string{canvasItemKindName(kind)}}, {"nodes", std::move(nodes)}});
        }
    }
    if (bake.empty()) bake.push_back(0.0);
    const bool frame = !frame_sources.empty();

    const auto vertex = vertexShader(output, kinds, frame);
    const auto fragment = fragmentShader(output, kinds, frame);
    const auto digest = picosha2::hash256_hex_string(vertex + std::string(1, '\0') + fragment);
    const auto stem = "canvas_" + fileStem(id) + "_" + digest.substr(0, 8);
    lowered.sources.push_back({stem, "vert", vertex});
    lowered.sources.push_back({stem, "frag", fragment});

    Json image_parameters{{"type", colorType()}, {"format", output.format},
                          {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
    if (output.image_size) {
        image_parameters["width"] = (*output.image_size)[0];
        image_parameters["height"] = (*output.image_size)[1];
    } else {
        image_parameters["extent_scale"] = output.extent_scale;
    }
    lowered.nodes.push_back({{"id", canvasImageNodeId(id)}, {"op", "render.image"},
                             {"parameters", std::move(image_parameters)}, {"inputs", Json::object()},
                             {"outputs", Json::array({"resource"})}});
    lowered.nodes.push_back({{"id", canvasItemsNodeId(id)}, {"op", "render.attribute"},
                             {"parameters", {{"element", "mat4"}, {"domain", "instance"}, {"count", items.size()},
                                             {"host_source", "runtime_f32"}, {"initial_f32", floatArray(item_table)}}},
                             {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
    lowered.nodes.push_back({{"id", canvasBakeNodeId(id)}, {"op", "render.buffer"},
                             {"parameters", {{"element", "float"}, {"size", bake.size() * sizeof(float)},
                                             {"host_source", "runtime_f32"}, {"initial_f32", floatArray(bake)}}},
                             {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
    if (frame) {
        lowered.nodes.push_back({{"id", canvasFrameNodeId(id)}, {"op", "render.buffer"},
                                 {"parameters", {{"element", "float"}, {"size", frame_initial.size() * sizeof(float)},
                                                 {"host_source", "runtime_f32"},
                                                 {"initial_f32", floatArray(frame_initial)}}},
                                 {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
        // One float per value edge, tiling the frame table from byte 0 (RS-15).
        for (std::size_t index = 0; index < frame_sources.size(); ++index)
            lowered.nodes.push_back({{"id", canvasFrameSinkNodeId(id, index)}, {"op", "render.scalar"},
                                     {"parameters", {{"target", canvasFrameNodeId(id)}, {"offset", index * sizeof(float)}}},
                                     {"inputs", {{"value", endpoint(frame_sources[index].node, frame_sources[index].port)}}},
                                     {"outputs", Json::array()}});
    }
    Json ports{{"items", {{"kind", "buffer"}, {"element", "mat4"}, {"domain", "instance"}, {"access", "read"}}},
               {"bake", {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}}},
               {"image", {{"kind", "image"}, {"type", colorType()}, {"access", "write"}, {"attachment", "color"},
                          {"location", 0}}}};
    Json inputs{{"items", endpoint(canvasItemsNodeId(id), "resource")},
                {"bake", endpoint(canvasBakeNodeId(id), "resource")},
                {"image", endpoint(canvasImageNodeId(id), "resource")}};
    if (frame) {
        ports["frame"] = {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}};
        inputs["frame"] = endpoint(canvasFrameNodeId(id), "resource");
    }
    // Premultiplied over: the image is premultiplied, so is every item.
    const Json premultiplied{{"color", {{"src", "one"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}}},
                             {"alpha", {{"src", "one"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}}}};
    const auto shader = "project://shaders/generated/" + stem;
    lowered.nodes.push_back(
        {{"id", id},
         {"op", "render.raster"},
         {"parameters",
          {{"shader", {{"vertex", shader}, {"fragment", shader}}},
           {"draw", {{"vertex_count", 6}, {"instance_source", "items"}}},
           {"raster_state",
            {{"topology", "triangle_list"}, {"cull", "none"}, {"depth_test", false}, {"depth_write", false},
             {"color_attachments", Json::array({{{"blend", premultiplied}, {"write_mask", "rgba"}}})}}},
           {"clear_color", Json::array({0.0, 0.0, 0.0, 0.0})},
           {"ports", std::move(ports)}}},
         {"inputs", std::move(inputs)},
         {"outputs", Json::array({"image"})}});
    return lowered;
}

// A node left in the lowered scope may read a canvas.output's `image` (the
// raster pass keeps the id and the port) and nothing else of the canvas.
void checkSurvivingEdges(const Json &nodes, const Scope &scope, const Json *outputs) {
    const auto check = [&](const Json &endpoint, const std::string &consumer) {
        const auto *from = producer(scope, endpoint);
        if (from == nullptr || !isCanvasGraphOperation(nodeOp(*from))) return;
        if (nodeOp(*from) == "canvas.output" && endpoint.value("port", std::string{}) == "image") return;
        throw std::invalid_argument("only a canvas.output's image leaves the canvas; " + consumer + " reads " +
                                    nodeId(*from));
    };
    for (const auto &node : nodes) {
        if (isCanvasGraphOperation(nodeOp(node))) continue;
        for (const auto &[port, endpoint] : objectMember(node, "inputs").items()) {
            if (endpoint.is_object()) check(endpoint, nodeId(node) + "." + port);
            else if (endpoint.is_array())
                for (const auto &entry : endpoint) check(entry, nodeId(node) + "." + port);
        }
    }
    if (outputs != nullptr && outputs->is_object())
        for (const auto &[port, endpoint] : outputs->items()) check(endpoint, "output " + port);
}
} // namespace

std::uint32_t canvasItemKind(CanvasShape shape, CanvasPaint paint) {
    return (shape == CanvasShape::rect ? 0u : 2u) + (paint == CanvasPaint::fill ? 0u : 1u);
}

std::string_view canvasItemKindName(std::uint32_t kind) {
    switch (kind) {
    case 0: return "rect_fill";
    case 1: return "rect_stroke";
    case 2: return "ellipse_fill";
    case 3: return "ellipse_stroke";
    default: throw std::invalid_argument("unknown canvas item kind: " + std::to_string(kind));
    }
}

std::size_t canvasShapeParameterCount(CanvasShape shape) { return shape == CanvasShape::rect ? 6 : 5; }
std::size_t canvasPaintParameterCount(CanvasPaint paint) { return paint == CanvasPaint::fill ? 4 : 6; }

std::string canvasImageNodeId(const std::string &output) { return reservedPrefix(output) + "image"; }
std::string canvasItemsNodeId(const std::string &output) { return reservedPrefix(output) + "items"; }
std::string canvasBakeNodeId(const std::string &output) { return reservedPrefix(output) + "bake"; }
std::string canvasFrameNodeId(const std::string &output) { return reservedPrefix(output) + "frame"; }
std::string canvasFrameSinkNodeId(const std::string &output, std::size_t index) {
    return reservedPrefix(output) + "set_" + std::to_string(index);
}

bool documentHasCanvasNodes(const nlohmann::json &document) {
    if (!document.is_object()) return false;
    bool found = false;
    eachScope(document, [&](const std::string &, const Json &holder) {
        const auto nodes = holder.find("nodes");
        if (nodes == holder.end() || !nodes->is_array()) return;
        for (const auto &node : *nodes) found = found || isCanvasGraphOperation(nodeOp(node));
    });
    return found;
}

CanvasIr buildCanvasIr(const nlohmann::json &document) {
    CanvasIr ir;
    if (!documentHasCanvasNodes(document)) return ir;
    const auto bodies = loopBodies(document);
    eachScope(document, [&](const std::string &name, const Json &holder) { checkScope(name, holder, bodies); });
    eachScope(document, [&](const std::string &name, const Json &holder) {
        const auto nodes = holder.find("nodes");
        if (nodes == holder.end() || !nodes->is_array()) return;
        const auto scope = indexScope(name, *nodes);
        for (const auto &node : *nodes)
            if (nodeOp(node) == "canvas.output") ir.outputs.push_back(buildOutput(scope, node, ir));
    });
    return ir;
}

CanvasLowering lowerCanvasNodes(const nlohmann::json &document) {
    CanvasLowering result;
    if (!documentHasCanvasNodes(document)) {
        result.document = document;
        return result;
    }
    result.ir = buildCanvasIr(document);
    result.diagnostics = result.ir.diagnostics;
    result.changed = true;
    result.document = document;
    std::map<std::pair<std::string, std::string>, const CanvasOutput *> outputs;
    for (const auto &output : result.ir.outputs) outputs.emplace(std::pair{output.scope, output.id}, &output);
    const auto lowerScope = [&](const std::string &name, Json &holder) {
        const auto nodes = holder.find("nodes");
        if (nodes == holder.end() || !nodes->is_array() ||
            std::none_of(nodes->begin(), nodes->end(),
                         [](const Json &node) { return isCanvasGraphOperation(nodeOp(node)); }))
            return;
        const auto scope = indexScope(name, *nodes);
        checkSurvivingEdges(*nodes, scope, holder.contains("outputs") ? &holder.at("outputs") : nullptr);
        auto lowered = Json::array();
        for (const auto &node : *nodes) {
            const auto op = nodeOp(node);
            if (!isCanvasGraphOperation(op)) {
                lowered.push_back(node);
                continue;
            }
            if (op != "canvas.output") continue;
            auto output = lowerOutput(*outputs.at({name, nodeId(node)}));
            for (auto &added : output.nodes) lowered.push_back(std::move(added));
            for (auto &source : output.sources) result.generated_sources.push_back(std::move(source));
            for (auto &item : output.items) {
                if (!name.empty()) item["scope"] = name;
                result.items.push_back(std::move(item));
            }
        }
        holder["nodes"] = std::move(lowered);
        // Where the editor drew the authored nodes; most of them are gone.
        holder.erase("layout");
    };
    lowerScope(std::string{}, result.document);
    if (auto definitions = result.document.find("definitions");
        definitions != result.document.end() && definitions->is_object())
        for (auto &[name, body] : definitions->items())
            if (body.is_object()) lowerScope(name, body);
    return result;
}

} // namespace Pelican
