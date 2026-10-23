#include "canvaslowering.hpp"

#include "../graphcomposition.hpp"
#include "canvasmodule.hpp"
#include "canvastext.hpp"

#include <picosha2.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
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

using Affine = std::array<double, 6>;

// outer after inner: p -> outer(inner(p)), both as (a b c d tx ty).
Affine composeAffine(const Affine &outer, const Affine &inner) {
    return {outer[0] * inner[0] + outer[2] * inner[1], outer[1] * inner[0] + outer[3] * inner[1],
            outer[0] * inner[2] + outer[2] * inner[3], outer[1] * inner[2] + outer[3] * inner[3],
            outer[0] * inner[4] + outer[2] * inner[5] + outer[4], outer[1] * inner[4] + outer[3] * inner[5] + outer[5]};
}

struct Flattening {
    double opacity = 1.0;
    std::vector<CanvasValueSource> opacity_edges;
    std::vector<std::string> stacks;
    // CV-2a. The masks every item under here takes, the map from the stack
    // being walked to the canvas of the list being filled, and that list's
    // blend space.
    std::vector<CanvasMask> masks;
    Affine affine{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    bool srgb = false;
    // The layer entry asked for pass_through; the next stack takes it.
    bool pass_through = false;
    // The layer entry's own opacity is not 1 (for the flattened diagnostic).
    bool own_opacity = false;
    // Where groups are collected.
    CanvasOutput *output = nullptr;
    // CV-4: the fonts shape.text reads.
    CanvasFontLibrary *fonts = nullptr;
    // CV-6a. Walking for a canvas.place: the layers' depths count. `flat`
    // inside a list composited on its own, whose layers' depths are ignored.
    bool placed = false;
    bool flat = false;
    double depth = 0.0;
    std::optional<CanvasParameter> depth_edge;
    Json depth_layer;
    // Where the depth diagnostics go.
    Json *diagnostics = nullptr;
};

// The context a list starts from when it is composited on its own: no
// opacity, no mask, no transform, the stacks it came from kept for the trace.
Flattening freshFlattening(const Flattening &outer) {
    Flattening fresh;
    fresh.stacks = outer.stacks;
    fresh.srgb = outer.srgb;
    fresh.output = outer.output;
    fresh.fonts = outer.fonts;
    // CV-6a: the list is one picture, placed at the depth of the layer it is.
    fresh.placed = outer.placed;
    fresh.flat = outer.placed;
    fresh.diagnostics = outer.diagnostics;
    return fresh;
}

// CV-6a: adds `flattening`'s depth to an item's.
void addDepth(CanvasItem &item, const Flattening &flattening) {
    item.depth += flattening.depth;
    if (flattening.depth_edge) {
        if (item.depth_edge)
            throw std::invalid_argument("canvas.place adds at most one edge-driven layer depth to an item: " +
                                        flattening.depth_edge->edge->node + " and " + item.depth_edge->edge->node);
        item.depth_edge = flattening.depth_edge;
    }
    if (item.depth_layer.is_null()) item.depth_layer = flattening.depth_layer;
}

// Lifts an item made under a fresh context into `flattening`'s list.
void applyFlattening(CanvasItem &item, const Flattening &flattening) {
    addDepth(item, flattening);
    item.opacity *= flattening.opacity;
    item.opacity_edges.insert(item.opacity_edges.end(), flattening.opacity_edges.begin(),
                              flattening.opacity_edges.end());
    for (auto &mask : item.masks) mask.affine = composeAffine(flattening.affine, mask.affine);
    item.masks.insert(item.masks.end(), flattening.masks.begin(), flattening.masks.end());
    item.affine = composeAffine(flattening.affine, item.affine);
}

void appendLayer(const Scope &scope, const Json &endpoint, const Flattening &flattening,
                 std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items);

// A paint's parameters, in table order: color, then a stroke's width and align.
std::vector<CanvasParameter> paintParameters(const Scope &scope, const Json &paint, bool stroke) {
    std::vector<CanvasParameter> parameters;
    appendVector(parameters, scope, paint, "color", {"r", "g", "b", "a"});
    if (stroke) {
        parameters.push_back(parameter(scope, paint, "width", std::nullopt, 0, "width", std::nullopt));
        // `align` rebuilds; it is a constant of the bake table, never an edge.
        double align = 0.0;
        if (const auto found = objectMember(paint, "parameters").find("align");
            found != objectMember(paint, "parameters").end()) {
            if (*found == "center") align = 0.0;
            else if (*found == "inner") align = 1.0;
            else if (*found == "outer") align = 2.0;
            else throw std::invalid_argument("shape.stroke align must be center, inner or outer: " + nodeId(paint));
        }
        parameters.push_back(CanvasParameter{align, std::nullopt});
    }
    return parameters;
}

// The paint's item under `flattening`, as appendPaint makes it.
void placePaintItem(CanvasItem &item, const std::string &paint, const std::string &shape,
                    const Flattening &flattening) {
    item.opacity = flattening.opacity;
    item.opacity_edges = flattening.opacity_edges;
    item.masks = flattening.masks;
    item.affine = flattening.affine;
    item.nodes = {paint, shape};
    item.nodes.insert(item.nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
    addDepth(item, flattening);
}

void appendText(const Scope &scope, const Json &paint, const Json &text, bool stroke, const Flattening &flattening,
                std::vector<CanvasItem> &items);

void appendPaint(const Scope &scope, const Json &paint, const Flattening &flattening, std::vector<CanvasItem> &items) {
    const auto op = nodeOp(paint);
    const auto id = nodeId(paint);
    const bool stroke = op == "shape.stroke";
    if (stroke) onlyKeys(paint, {"color", "width", "align"});
    else onlyKeys(paint, {"color"});
    const auto &inputs = objectMember(paint, "inputs");
    const auto geometry_edge = inputs.find("geometry");
    const auto *shape = geometry_edge == inputs.end() ? nullptr : producer(scope, *geometry_edge);
    if (shape != nullptr && nodeOp(*shape) == "shape.text") return appendText(scope, paint, *shape, stroke, flattening, items);
    if (shape == nullptr || (nodeOp(*shape) != "shape.rect" && nodeOp(*shape) != "shape.ellipse"))
        throw std::invalid_argument(op + " requires a geometry edge from shape.rect, shape.ellipse or shape.text: " + id);
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
    for (auto &entry : paintParameters(scope, paint, stroke)) item.parameters.push_back(std::move(entry));
    placePaintItem(item, id, nodeId(*shape), flattening);
    items.push_back(std::move(item));
}

// --- CV-4: shape.text ------------------------------------------------------

// One item per character with an outline; the atlas places them later
// (bakeGlyphAtlases).
void appendText(const Scope &scope, const Json &paint, const Json &text, bool stroke, const Flattening &flattening,
                std::vector<CanvasItem> &items) {
    const auto id = nodeId(text);
    onlyKeys(text, {"font", "face", "text", "size", "origin", "align", "line_height", "tracking", "box_width",
                    "atlas_cell"});
    const auto &parameters = objectMember(text, "parameters");
    const auto what = [&](const char *key) { return std::string{"shape.text "} + key + ": " + id; };
    const auto font = parameters.find("font");
    if (font == parameters.end() || !font->is_string() || font->get_ref<const std::string &>().empty())
        throw std::invalid_argument("shape.text requires a font: " + id);
    int face = 0;
    if (const auto found = parameters.find("face"); found != parameters.end()) {
        if (!found->is_number_integer() || *found < 0 || *found > 0xFFFF)
            throw std::invalid_argument("shape.text face must be a face index: " + id);
        face = found->get<int>();
    }
    const auto string = parameters.find("text");
    if (string == parameters.end() || !string->is_string()) throw std::invalid_argument("shape.text requires a text: " + id);
    CanvasTextStyle style;
    if (const auto align = parameters.find("align"); align != parameters.end()) {
        if (*align == "left") style.align = CanvasTextAlign::left;
        else if (*align == "center") style.align = CanvasTextAlign::center;
        else if (*align == "right") style.align = CanvasTextAlign::right;
        else throw std::invalid_argument("shape.text align must be left, center or right: " + id);
    }
    if (const auto found = parameters.find("line_height"); found != parameters.end())
        style.line_height = number(*found, what("line_height"));
    if (const auto found = parameters.find("tracking"); found != parameters.end())
        style.tracking = number(*found, what("tracking"));
    if (const auto found = parameters.find("box_width"); found != parameters.end()) {
        style.box_width = number(*found, what("box_width"));
        if (style.box_width < 0.0) throw std::invalid_argument("shape.text box_width must not be negative: " + id);
    }
    std::uint32_t cell = 0;
    if (const auto found = parameters.find("atlas_cell"); found != parameters.end()) {
        if (!found->is_number_integer() || *found < 16 || *found > 256)
            throw std::invalid_argument("shape.text atlas_cell must be a whole number from 16 to 256: " + id);
        cell = found->get<std::uint32_t>();
    }
    std::vector<CanvasParameter> shared;
    appendVector(shared, scope, text, "origin", {"x", "y"}, 0.0);
    shared.push_back(parameter(scope, text, "size", std::nullopt, 0, "size", std::nullopt));
    style.size = shared.back().value;
    if (style.box_width > 0.0 && !(style.size > 0.0))
        throw std::invalid_argument("shape.text box_width wraps at the authored size, which must be positive: " + id);
    const auto paint_parameters = paintParameters(scope, paint, stroke);

    auto &fonts = *flattening.fonts;
    const auto font_index = fonts.open(font->get<std::string>(), face, id);
    auto &opened = fonts.face(font_index);
    const auto codepoints = decodeCanvasUtf8(string->get_ref<const std::string &>(), what("text"));
    const double units_per_em = opened.file->unitsPerEm();
    for (const auto &placed : layoutCanvasText(*opened.file, codepoints, style, id)) {
        const auto &outline = opened.glyph(placed.glyph);
        if (outline.outline.contours.empty()) continue;
        CanvasItem item;
        item.source = CanvasItemSource::glyph;
        item.paint = stroke ? CanvasPaint::stroke : CanvasPaint::fill;
        item.parameters = shared;
        item.parameters.resize(canvas_glyph_parameter_count);
        item.parameters.insert(item.parameters.end(), paint_parameters.begin(), paint_parameters.end());
        // CV-3a: its box in em, +y down (font units are y up).
        const std::array<double, 4> bounds{placed.x + outline.bounds[0] / units_per_em,
                                           placed.y - outline.bounds[3] / units_per_em,
                                           placed.x + outline.bounds[2] / units_per_em,
                                           placed.y - outline.bounds[1] / units_per_em};
        item.glyph = CanvasGlyph{font_index, placed.glyph, placed.codepoint, {placed.x, placed.y}, cell, bounds};
        placePaintItem(item, nodeId(paint), id, flattening);
        items.push_back(std::move(item));
    }
}

// --- CV-2a: image, color and transform layers, masks, clips, groups -------

CanvasParameter constantParameter(double value) { return CanvasParameter{value, std::nullopt}; }

std::array<double, 2> requiredSize(const Json &node) {
    const auto size = positiveSize(objectMember(node, "parameters"), nodeOp(node) + " " + nodeId(node));
    if (!size) throw std::invalid_argument(nodeOp(node) + " requires size: " + nodeId(node));
    return *size;
}

// sampler: linear (the default) or nearest.
bool nearestSampler(const Json &node) {
    const auto &parameters = objectMember(node, "parameters");
    const auto sampler = parameters.find("sampler");
    if (sampler == parameters.end() || *sampler == "linear") return false;
    if (*sampler == "nearest") return true;
    throw std::invalid_argument(nodeOp(node) + " sampler must be linear or nearest: " + nodeId(node));
}

std::string imageAsset(const Json &node) {
    const auto &parameters = objectMember(node, "parameters");
    const auto asset = parameters.find("asset");
    if (asset == parameters.end() || !asset->is_string() || asset->get_ref<const std::string &>().empty())
        throw std::invalid_argument("canvas.image requires an asset: " + nodeId(node));
    return asset->get<std::string>();
}

// channel: rgba (the default) or luminance.
bool luminanceChannel(const Json &node) {
    const auto &parameters = objectMember(node, "parameters");
    const auto channel = parameters.find("channel");
    if (channel == parameters.end() || *channel == "rgba") return false;
    if (*channel == "luminance") return true;
    throw std::invalid_argument("canvas.image channel must be rgba or luminance: " + nodeId(node));
}

void appendImage(const Scope &scope, const Json &node, const Flattening &flattening, std::vector<CanvasItem> &items) {
    onlyKeys(node, {"asset", "offset", "size", "channel", "outside", "sampler", "alpha"});
    if (luminanceChannel(node))
        throw std::invalid_argument("canvas.image channel luminance is read as a mask, not drawn: " + nodeId(node));
    // CV-5: a premultiplied file is a frame the player wrote (a static run
    // the ship policy folded): premultiplied linear colour, sRGB-encoded.
    bool premultiplied = false;
    if (const auto found = objectMember(node, "parameters").find("alpha");
        found != objectMember(node, "parameters").end()) {
        if (*found == "premultiplied") premultiplied = true;
        else if (*found != "straight")
            throw std::invalid_argument("canvas.image alpha must be straight or premultiplied: " + nodeId(node));
    }
    CanvasItem item;
    item.source = CanvasItemSource::image;
    // A linear stack reads the file through the sRGB decode; an sRGB stack
    // reads its bytes as they are and composites those. A premultiplied file
    // is always decoded (its bytes encode linear values), and an sRGB stack
    // encodes the colour back after the alpha is divided out.
    item.texture = {CanvasTexture::Origin::file, imageAsset(node), flattening.srgb && !premultiplied, std::nullopt, {},
                    0, nearestSampler(node)};
    appendVector(item.parameters, scope, node, "offset", {"x", "y"}, 0.0);
    const auto size = requiredSize(node);
    item.parameters.push_back(constantParameter(size[0]));
    item.parameters.push_back(constantParameter(size[1]));
    item.parameters.push_back(
        constantParameter(static_cast<double>(premultiplied ? CanvasAlpha::premultiplied : CanvasAlpha::straight)));
    item.parameters.push_back(constantParameter(static_cast<double>(
        premultiplied && flattening.srgb ? CanvasEncode::linear_to_srgb : CanvasEncode::none)));
    item.nodes = {nodeId(node)};
    item.nodes.insert(item.nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
    applyFlattening(item, flattening);
    items.push_back(std::move(item));
}

void appendFromImage(const Scope &scope, const Json &node, const Flattening &flattening,
                     std::vector<CanvasItem> &items) {
    onlyKeys(node, {"offset", "size", "alpha"});
    const auto id = nodeId(node);
    const auto &inputs = objectMember(node, "inputs");
    const auto edge = inputs.find("image");
    if (edge == inputs.end()) throw std::invalid_argument("canvas.from_image requires an image edge: " + id);
    // The image's producer stays in the lowered document; the raster reads it.
    const auto source = valueSource(scope, node, "image", *edge);
    auto alpha = CanvasAlpha::premultiplied;
    if (const auto found = objectMember(node, "parameters").find("alpha");
        found != objectMember(node, "parameters").end()) {
        if (*found == "premultiplied") alpha = CanvasAlpha::premultiplied;
        else if (*found == "straight") alpha = CanvasAlpha::straight;
        else if (*found == "opaque") alpha = CanvasAlpha::opaque;
        else throw std::invalid_argument("canvas.from_image alpha must be premultiplied, straight or opaque: " + id);
    }
    CanvasItem item;
    item.source = CanvasItemSource::image;
    item.texture = {CanvasTexture::Origin::graph, {}, false, source.node, source.port, 0, false};
    appendVector(item.parameters, scope, node, "offset", {"x", "y"}, 0.0);
    const auto size = requiredSize(node);
    item.parameters.push_back(constantParameter(size[0]));
    item.parameters.push_back(constantParameter(size[1]));
    item.parameters.push_back(constantParameter(static_cast<double>(alpha)));
    // A render image holds linear values.
    item.parameters.push_back(constantParameter(
        static_cast<double>(flattening.srgb ? CanvasEncode::linear_to_srgb : CanvasEncode::none)));
    item.nodes = {id};
    item.nodes.insert(item.nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
    applyFlattening(item, flattening);
    items.push_back(std::move(item));
}

void appendColor(const Scope &scope, const Json &node, const Flattening &flattening, std::vector<CanvasItem> &items) {
    onlyKeys(node, {"color"});
    CanvasItem item;
    item.source = CanvasItemSource::color;
    appendVector(item.parameters, scope, node, "color", {"r", "g", "b", "a"});
    item.nodes = {nodeId(node)};
    item.nodes.insert(item.nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
    applyFlattening(item, flattening);
    items.push_back(std::move(item));
}

// translate, then about the pivot rotate (degrees) and scale. CV-2a takes
// the numbers as authored; an edge into a transform is refused.
Affine transformAffine(const Json &node) {
    onlyKeys(node, {"translate", "rotate", "scale", "pivot"});
    const auto id = nodeId(node);
    for (const auto &[port, edge] : objectMember(node, "inputs").items()) {
        (void)edge;
        if (port != "layer")
            throw std::invalid_argument("canvas.transform takes no value edge yet (" + port + "): " + id);
    }
    const auto &parameters = objectMember(node, "parameters");
    const auto pair = [&](const char *key, double fallback) {
        const auto found = parameters.find(key);
        if (found == parameters.end()) return std::array<double, 2>{fallback, fallback};
        if (!found->is_array() || found->size() != 2)
            throw std::invalid_argument(std::string{"canvas.transform "} + key + " must be [x, y]: " + id);
        return std::array<double, 2>{number(found->at(0), "canvas.transform " + id),
                                     number(found->at(1), "canvas.transform " + id)};
    };
    const auto translate = pair("translate", 0.0);
    const auto scale = pair("scale", 1.0);
    const auto pivot = pair("pivot", 0.0);
    double rotate = 0.0;
    if (const auto found = parameters.find("rotate"); found != parameters.end())
        rotate = number(*found, "canvas.transform rotate: " + id);
    const auto radians = rotate * 3.14159265358979323846 / 180.0;
    const auto c = std::cos(radians), s = std::sin(radians);
    const Affine linear{c * scale[0], s * scale[0], -s * scale[1], c * scale[1], 0.0, 0.0};
    auto result = composeAffine(linear, Affine{1.0, 0.0, 0.0, 1.0, -pivot[0], -pivot[1]});
    result[4] += pivot[0] + translate[0];
    result[5] += pivot[1] + translate[1];
    return result;
}

void appendTransform(const Scope &scope, const Json &node, const Flattening &flattening,
                     std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    const auto affine = transformAffine(node);
    const auto &inputs = objectMember(node, "inputs");
    const auto layer = inputs.find("layer");
    if (layer == inputs.end()) throw std::invalid_argument("canvas.transform requires a layer edge: " + nodeId(node));
    Flattening inner = flattening;
    inner.affine = composeAffine(flattening.affine, affine);
    inner.stacks.insert(inner.stacks.begin(), nodeId(node));
    appendLayer(scope, *layer, inner, active, ir, items);
}

// A group item reading `items` composited into an image of their own.
CanvasItem groupItem(std::vector<CanvasItem> items, bool srgb, const std::string &node,
                     const Flattening &flattening) {
    CanvasItem item;
    item.source = CanvasItemSource::group;
    item.texture.origin = CanvasTexture::Origin::group;
    item.texture.group = flattening.output->groups.size();
    flattening.output->groups.push_back(CanvasGroup{std::move(items), srgb, node});
    const auto encode = flattening.srgb == srgb ? CanvasEncode::none
                        : srgb                  ? CanvasEncode::srgb_to_linear
                                                : CanvasEncode::linear_to_srgb;
    item.parameters.push_back(constantParameter(static_cast<double>(encode)));
    item.nodes = {node};
    item.nodes.insert(item.nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
    applyFlattening(item, flattening);
    return item;
}

// The layer as one item: itself when it draws one, else its items
// composited into a group. Empty when it draws nothing.
std::optional<CanvasItem> singleItem(const Scope &scope, const Json &endpoint, const Flattening &flattening,
                                     std::vector<std::string> &active, CanvasIr &ir) {
    auto inner = freshFlattening(flattening);
    inner.pass_through = flattening.pass_through;
    std::vector<CanvasItem> items;
    appendLayer(scope, endpoint, inner, active, ir, items);
    if (items.empty()) return std::nullopt;
    if (items.size() == 1) {
        applyFlattening(items.front(), flattening);
        return std::move(items.front());
    }
    const auto *from = producer(scope, endpoint);
    return groupItem(std::move(items), flattening.srgb, from == nullptr ? std::string{} : nodeId(*from), flattening);
}

// --- CV-2b: the backdrop and colour filters -------------------------------

void appendBackdrop(const Json &node, const Flattening &flattening, std::vector<CanvasItem> &items) {
    onlyKeys(node, {});
    if (!objectMember(node, "inputs").empty())
        throw std::invalid_argument("canvas.backdrop takes no edge: " + nodeId(node));
    CanvasItem item;
    item.source = CanvasItemSource::backdrop;
    item.nodes = {nodeId(node)};
    item.nodes.insert(item.nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
    applyFlattening(item, flattening);
    items.push_back(std::move(item));
}

// A filter works on one item's colour: its layer drawn as one (a group when
// it draws several items), the filter added after the ones it already has.
// CV-3a: its numbers may be driven by value edges (the frame table).
void appendFilter(const Scope &scope, const Json &node, const Flattening &flattening,
                  std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    auto filter = parseCanvasFilter(node);
    const auto &inputs = objectMember(node, "inputs");
    filter.edges.assign(filter.constants.size(), std::nullopt);
    for (std::size_t k = 0; k < filter.ports.size() && k < filter.edges.size(); ++k) {
        if (filter.ports[k].empty()) continue;
        if (const auto edge = inputs.find(filter.ports[k]); edge != inputs.end())
            filter.edges[k] = valueSource(scope, node, filter.ports[k], *edge);
    }
    const auto layer = inputs.find("layer");
    if (layer == inputs.end()) throw std::invalid_argument(nodeOp(node) + " requires a layer edge: " + nodeId(node));
    auto inner = flattening;
    inner.stacks.insert(inner.stacks.begin(), nodeId(node));
    auto item = singleItem(scope, *layer, inner, active, ir);
    if (!item) return;
    item->filters.push_back(std::move(filter));
    items.push_back(std::move(*item));
}

// --- CV-3a: the neighbourhood filters -----------------------------------------

// A neighbourhood filter's numbers, each of which a value edge may drive.
struct Neighbourhood {
    std::string node;
    std::string op;
    bool blur = false;
    bool shadow = false;
    std::array<CanvasParameter, 2> offset{};
    CanvasParameter radius;
    // max_radius when an edge drives the radius, else the radius.
    double radius_limit = 0.0;
    CanvasParameter spread;
    std::array<CanvasParameter, 4> color{};
    CanvasParameter opacity{1.0, std::nullopt};
};

// Photoshop's defaults (the layer styles of CS and before): a drop shadow
// black at 75 %, 5 px away at 120 degrees (the light from the upper left,
// the shadow down to the right), size 5 and no spread; an outer glow
// (255, 255, 190) at 75 %, size 5, no spread. A Gaussian Blur of 1 px.
constexpr double drop_shadow_offset[2] = {2.5, 4.330127018922193};
constexpr double outer_glow_color[3] = {1.0, 1.0, 190.0 / 255.0};

Neighbourhood parseNeighbourhood(const Scope &scope, const Json &node, Json &diagnostics) {
    Neighbourhood filter;
    filter.node = nodeId(node);
    filter.op = nodeOp(node);
    filter.blur = filter.op == "filter.blur";
    filter.shadow = filter.op == "filter.drop_shadow";
    if (filter.blur) onlyKeys(node, {"radius", "max_radius"});
    else if (filter.shadow) onlyKeys(node, {"offset", "radius", "max_radius", "spread", "color", "opacity"});
    else onlyKeys(node, {"radius", "max_radius", "spread", "color", "opacity"});
    const auto &parameters = objectMember(node, "parameters");
    const auto &inputs = objectMember(node, "inputs");
    const auto what = [&](const char *key) { return filter.op + " " + key + ": " + filter.node; };
    const auto within = [&](const CanvasParameter &value, double low, double high, const char *key) {
        if (value.edge) return;
        if (!(value.value >= low && value.value <= high))
            throw std::invalid_argument(filter.op + " " + key + " must lie in " + std::to_string(static_cast<int>(low)) +
                                        ".." + std::to_string(static_cast<int>(high)) + ": " + filter.node);
    };
    filter.radius = parameter(scope, node, "radius", std::nullopt, 0, "radius", filter.blur ? 1.0 : 5.0);
    within(filter.radius, 0.0, canvas_filter_radius_limit, "radius");
    std::optional<double> max_radius;
    if (const auto found = parameters.find("max_radius"); found != parameters.end()) {
        max_radius = number(*found, what("max_radius"));
        if (!(*max_radius > 0.0 && *max_radius <= canvas_filter_radius_limit))
            throw std::invalid_argument(filter.op + " max_radius must lie in 0..250 (and not be 0): " + filter.node);
    }
    // The passes are made for the largest radius: an edge needs one.
    if (filter.radius.edge && !max_radius)
        throw GraphRegistryError("canvas.range_unbounded@1", {{"node", filter.node}, {"parameter", "radius"}});
    filter.radius_limit = max_radius.value_or(filter.radius.value);
    if (parameters.contains("radius") && filter.radius.value > filter.radius_limit)
        diagnostics.push_back({{"code", "canvas.radius_clamped"},
                               {"node", filter.node},
                               {"radius", filter.radius.value},
                               {"max_radius", filter.radius_limit}});
    if (filter.blur) return filter;
    filter.spread = parameter(scope, node, "spread", std::nullopt, 0, "spread", 0.0);
    within(filter.spread, 0.0, 1.0, "spread");
    filter.opacity = parameter(scope, node, "opacity", std::nullopt, 0, "opacity", 0.75);
    within(filter.opacity, 0.0, 1.0, "opacity");
    const std::array<double, 4> fallback =
        filter.shadow ? std::array<double, 4>{0.0, 0.0, 0.0, 1.0}
                      : std::array<double, 4>{outer_glow_color[0], outer_glow_color[1], outer_glow_color[2], 1.0};
    const auto color = parameters.find("color");
    std::size_t arity = 4;
    if (color != parameters.end()) {
        if (!color->is_array() || (color->size() != 3 && color->size() != 4))
            throw std::invalid_argument(filter.op + " color must be [r, g, b] or [r, g, b, a]: " + filter.node);
        arity = color->size();
    }
    const char *const color_ports[4] = {"color.r", "color.g", "color.b", "color.a"};
    for (std::size_t k = 0; k < 4; ++k) {
        if (k < arity) {
            filter.color[k] = parameter(scope, node, "color", arity, k, color_ports[k], fallback[k]);
            continue;
        }
        // [r, g, b]: the alpha is 1 unless an edge drives it.
        filter.color[k] = CanvasParameter{1.0, std::nullopt};
        if (const auto edge = inputs.find(color_ports[k]); edge != inputs.end())
            filter.color[k].edge = valueSource(scope, node, color_ports[k], *edge);
    }
    if (filter.shadow)
        for (std::size_t k = 0; k < 2; ++k)
            filter.offset[k] = parameter(scope, node, "offset", 2, k, k == 0 ? "offset.x" : "offset.y",
                                         drop_shadow_offset[k]);
    return filter;
}

using Extent = std::array<double, 4>; // x0 y0 x1 y1

Extent mapExtent(const Affine &m, const Extent &e) {
    Extent result{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const auto x : {e[0], e[2]})
        for (const auto y : {e[1], e[3]}) {
            const auto px = m[0] * x + m[2] * y + m[4], py = m[1] * x + m[3] * y + m[5];
            result = {std::min(result[0], px), std::min(result[1], py), std::max(result[2], px), std::max(result[3], py)};
        }
    return result;
}

std::optional<Extent> unite(const std::optional<Extent> &a, const std::optional<Extent> &b) {
    if (!a || !b) return std::nullopt;
    return Extent{std::min((*a)[0], (*b)[0]), std::min((*a)[1], (*b)[1]), std::max((*a)[2], (*b)[2]),
                  std::max((*a)[3], (*b)[3])};
}

double strokeOutset(double width, double align) {
    return align > 1.5 ? std::abs(width) : (align > 0.5 ? 0.0 : 0.5 * std::abs(width));
}

// How far past its source a shadow or glow item's quad goes: at 4/3 of the
// radius the coverage is below 1e-4 at any spread.
constexpr double effect_reach = 4.0 / 3.0;

std::size_t kindParameterCount(std::uint32_t kind);

// Where an item draws, in the coordinates of the list it is in; nothing when
// an edge moves it (or it is a group of such).
std::optional<Extent> itemExtent(const CanvasItem &item, const CanvasOutput &output, std::size_t depth = 0) {
    if (depth > 64) return std::nullopt;
    const auto fixed = [&](std::size_t index) -> std::optional<double> {
        const auto &parameter = item.parameters.at(index);
        if (parameter.edge) return std::nullopt;
        return parameter.value;
    };
    const auto kind = canvasItemKind(item);
    const auto own = kind >= canvas_kind_effect_offset ? kind - canvas_kind_effect_offset : kind;
    std::optional<Extent> local;
    switch (item.source) {
    case CanvasItemSource::shape: {
        const auto rect = item.shape == CanvasShape::rect;
        const auto rotation = fixed(rect ? 5 : 4);
        const auto sx = fixed(0), sy = fixed(1), cx = fixed(2), cy = fixed(3);
        if (!rotation || !sx || !sy || !cx || !cy) return std::nullopt;
        const auto hx = 0.5 * std::abs(*sx), hy = 0.5 * std::abs(*sy);
        const auto radians = *rotation * 3.14159265358979323846 / 180.0;
        const auto c = std::abs(std::cos(radians)), s = std::abs(std::sin(radians));
        double bx = rect ? c * hx + s * hy : std::hypot(hx * c, hy * s);
        double by = rect ? s * hx + c * hy : std::hypot(hx * s, hy * c);
        if (item.paint == CanvasPaint::stroke) {
            const auto base = canvasShapeParameterCount(item.shape);
            const auto width = fixed(base + 4);
            if (!width) return std::nullopt;
            bx += strokeOutset(*width, item.parameters.at(base + 5).value);
            by += strokeOutset(*width, item.parameters.at(base + 5).value);
        }
        local = Extent{*cx - bx, *cy - by, *cx + bx, *cy + by};
        break;
    }
    case CanvasItemSource::glyph: {
        const auto ox = fixed(0), oy = fixed(1), size = fixed(2);
        if (!ox || !oy || !size || !item.glyph) return std::nullopt;
        const auto &b = item.glyph->bounds;
        const auto s = std::abs(*size);
        // A quarter of an em of room: the antialiased edge and a thin stroke.
        double pad = 0.25 * s;
        if (item.paint == CanvasPaint::stroke) {
            const auto width = fixed(canvas_glyph_parameter_count + 4);
            if (!width) return std::nullopt;
            pad += strokeOutset(*width, item.parameters.at(canvas_glyph_parameter_count + 5).value);
        }
        local = Extent{*ox + s * b[0] - pad, *oy + s * b[1] - pad, *ox + s * b[2] + pad, *oy + s * b[3] + pad};
        break;
    }
    case CanvasItemSource::image: {
        const auto ox = fixed(0), oy = fixed(1), w = fixed(2), h = fixed(3);
        if (!ox || !oy || !w || !h) return std::nullopt;
        local = Extent{std::min(*ox, *ox + *w), std::min(*oy, *oy + *h), std::max(*ox, *ox + *w),
                       std::max(*oy, *oy + *h)};
        break;
    }
    case CanvasItemSource::color:
    case CanvasItemSource::backdrop: local = Extent{0.0, 0.0, output.size[0], output.size[1]}; break;
    case CanvasItemSource::group: {
        const auto &group = output.groups.at(item.texture.group);
        if (group.items.empty()) return std::nullopt;
        for (const auto &inner : group.items) {
            const auto extent = itemExtent(inner, output, depth + 1);
            local = local ? unite(local, extent) : extent;
            if (!local) return std::nullopt;
        }
        break;
    }
    }
    if (!local) return std::nullopt;
    // A distance item's shadow or glow: moved by its offset, past it by its reach.
    if (kind >= canvas_kind_effect_offset) {
        const auto base = kindParameterCount(own);
        const auto ox = fixed(base), oy = fixed(base + 1);
        if (!ox || !oy) return std::nullopt;
        const auto reach = effect_reach * item.parameters.at(base + 3).value;
        local = Extent{(*local)[0] + *ox - reach, (*local)[1] + *oy - reach, (*local)[2] + *ox + reach,
                       (*local)[3] + *oy + reach};
    }
    return mapExtent(item.affine, *local);
}

// Where items draw together; nothing when one of them is moved by an edge.
std::optional<Extent> listExtent(const std::vector<CanvasItem> &items, const CanvasOutput &output) {
    std::optional<Extent> extent;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const auto one = itemExtent(items[k], output);
        if (!one) return std::nullopt;
        extent = k == 0 ? one : unite(extent, one);
    }
    return extent;
}

// The images a neighbourhood filter is drawn through (CanvasBlur, its source
// to come): the source's `extent` in the filter's space plus 3 sigma, clipped
// to what the canvas shows. Nothing when no part of it can show.
std::optional<CanvasBlur> blurShape(const std::optional<Extent> &extent, const Neighbourhood &filter,
                                    const Flattening &flattening) {
    const auto &output = *flattening.output;
    CanvasBlur blur;
    blur.sigma_scale = filter.blur ? 1.0 : 1.0 / 3.0;
    const auto sigma = blur.sigma_scale * filter.radius_limit;
    // Halved while the kernel would reach past 32 pixels.
    while (blur.levels < 8 && 3.0 * sigma / std::ldexp(1.0, static_cast<int>(blur.levels)) > 32.0) ++blur.levels;
    blur.taps = static_cast<std::uint32_t>(std::ceil(3.0 * canvasBlurLevelSigma(sigma, blur.levels) - 1e-9));
    const auto margin = std::ceil(3.0 * sigma) + 1.0;
    const auto grow = [&](Extent e) { return Extent{e[0] - margin, e[1] - margin, e[2] + margin, e[3] + margin}; };
    // What the canvas shows, in the filter's space (a shadow's source moved
    // back by its offset).
    std::optional<Extent> visible;
    const auto &m = flattening.affine;
    if (const auto det = m[0] * m[3] - m[2] * m[1]; std::abs(det) > 1e-12) {
        const Affine inverse{m[3] / det, -m[1] / det, -m[2] / det, m[0] / det,
                             (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det};
        visible = mapExtent(inverse, Extent{0.0, 0.0, output.size[0], output.size[1]});
        if (filter.shadow) {
            if (filter.offset[0].edge || filter.offset[1].edge) visible.reset();
            else
                visible = Extent{(*visible)[0] - filter.offset[0].value, (*visible)[1] - filter.offset[1].value,
                                 (*visible)[2] - filter.offset[0].value, (*visible)[3] - filter.offset[1].value};
        }
    }
    auto region = extent;
    if (region) region = grow(*region);
    if (visible) {
        const auto shown = grow(*visible);
        region = region ? Extent{std::max((*region)[0], shown[0]), std::max((*region)[1], shown[1]),
                                 std::min((*region)[2], shown[2]), std::min((*region)[3], shown[3])}
                        : shown;
    }
    if (!region) region = grow(Extent{0.0, 0.0, output.size[0], output.size[1]});
    if (!((*region)[2] > (*region)[0]) || !((*region)[3] > (*region)[1])) return std::nullopt;
    const auto step = std::ldexp(1.0, static_cast<int>(blur.levels));
    const auto x0 = std::floor((*region)[0]), y0 = std::floor((*region)[1]);
    const auto w = std::ceil((std::ceil((*region)[2]) - x0) / step) * step;
    const auto h = std::ceil((std::ceil((*region)[3]) - y0) / step) * step;
    if (w > canvas_filter_pixel_limit || h > canvas_filter_pixel_limit)
        throw GraphRegistryError("canvas.filter_size_exceeded@1",
                                 {{"node", filter.node}, {"pixels", {w, h}}, {"limit", canvas_filter_pixel_limit}});
    blur.rect = {x0, y0, w, h};
    blur.radius = filter.radius;
    blur.radius_limit = filter.radius_limit;
    blur.srgb = flattening.srgb;
    blur.node = filter.node;
    blur.op = filter.op;
    return blur;
}

// Blur `blur` of `source` into the output, its passes said in the diagnostics.
std::size_t addBlur(CanvasBlur blur, CanvasItem source, CanvasOutput &output, Json &diagnostics,
                    const std::string &why) {
    blur.source = std::move(source);
    diagnostics.push_back({{"code", "canvas.draw_split"},
                           {"output", output.id},
                           {"reason", "neighbourhood_filter"},
                           {"node", blur.node},
                           {"op", blur.op},
                           {"why", why},
                           {"passes", 2 + blur.levels},
                           {"levels", blur.levels}});
    output.blurs.push_back(std::move(blur));
    return output.blurs.size() - 1;
}

// The image of blur `index`, premultiplied, over its rect.
CanvasItem blurImageItem(const CanvasOutput &output, std::size_t index, std::vector<std::string> nodes) {
    const auto &blur = output.blurs.at(index);
    CanvasItem item;
    item.source = CanvasItemSource::image;
    item.texture = {CanvasTexture::Origin::blur, {}, false, std::nullopt, {}, index, false};
    for (const auto value : {0.0, 0.0, blur.rect[2], blur.rect[3], static_cast<double>(CanvasAlpha::premultiplied),
                             static_cast<double>(CanvasEncode::none)})
        item.parameters.push_back(constantParameter(value));
    item.affine = {1.0, 0.0, 0.0, 1.0, blur.rect[0], blur.rect[1]};
    item.nodes = std::move(nodes);
    return item;
}

// The filter's share of an effect item: its offset, radius, spread and colour
// as parameters, its opacity the item's.
void addEffect(CanvasItem &item, const Neighbourhood &filter) {
    item.effect = CanvasEffectOrigin{filter.node, filter.op};
    item.parameters.push_back(filter.shadow ? filter.offset[0] : constantParameter(0.0));
    item.parameters.push_back(filter.shadow ? filter.offset[1] : constantParameter(0.0));
    item.parameters.push_back(filter.radius);
    item.parameters.push_back(constantParameter(filter.radius_limit));
    item.parameters.push_back(filter.spread);
    for (const auto &component : filter.color) item.parameters.push_back(component);
    if (filter.opacity.edge) item.opacity_edges.push_back(*filter.opacity.edge);
    else item.opacity *= filter.opacity.value;
}

// The shadow or glow of blur `index`: its alpha, spread, in the colour.
CanvasItem effectImageItem(const CanvasOutput &output, std::size_t index, const Neighbourhood &filter,
                           std::vector<std::string> nodes) {
    const auto &blur = output.blurs.at(index);
    CanvasItem item;
    item.source = CanvasItemSource::image;
    item.texture = {CanvasTexture::Origin::blur, {}, false, std::nullopt, {}, index, false};
    item.effect = CanvasEffectOrigin{filter.node, filter.op};
    item.parameters.push_back(filter.shadow ? filter.offset[0] : constantParameter(0.0));
    item.parameters.push_back(filter.shadow ? filter.offset[1] : constantParameter(0.0));
    item.parameters.push_back(constantParameter(blur.rect[2]));
    item.parameters.push_back(constantParameter(blur.rect[3]));
    item.parameters.push_back(filter.radius);
    item.parameters.push_back(constantParameter(filter.radius_limit));
    item.parameters.push_back(filter.spread);
    for (const auto &component : filter.color) item.parameters.push_back(component);
    if (filter.opacity.edge) item.opacity_edges.push_back(*filter.opacity.edge);
    else item.opacity *= filter.opacity.value;
    item.affine = {1.0, 0.0, 0.0, 1.0, blur.rect[0], blur.rect[1]};
    item.nodes = std::move(nodes);
    return item;
}

// How far a text's atlas holds distances, in stack units, at the cell its
// characters ask for (a larger cell another text of the output asks for
// shortens it: bakeGlyphAtlases cuts the effect to that).
double glyphReach(const CanvasItem &item) {
    const auto cell = item.glyph->cell != 0                              ? item.glyph->cell
                      : item.glyph->codepoint >= canvas_glyph_cjk_first ? canvas_glyph_cell_cjk
                                                                        : canvas_glyph_cell_latin;
    return canvas_glyph_range * std::abs(item.parameters.at(2).value) / (static_cast<double>(cell) - 2.0 * canvas_glyph_range);
}

// The reason a shadow or glow of these paint items is not drawn from their
// distances, or "" when it is.
std::string distanceRefusal(const std::vector<CanvasItem> &bodies, const Neighbourhood &filter) {
    for (const auto &body : bodies) {
        if (body.source != CanvasItemSource::glyph) continue;
        const auto base = canvas_glyph_parameter_count;
        const bool stroke = body.paint == CanvasPaint::stroke;
        if (body.parameters.at(2).edge || (stroke && body.parameters.at(base + 4).edge)) return "glyph_size_edge";
        const auto outset = stroke ? strokeOutset(body.parameters.at(base + 4).value, body.parameters.at(base + 5).value)
                                   : 0.0;
        if (filter.radius_limit + outset > glyphReach(body) + 1e-9) return "glyph_range";
    }
    return {};
}

// filter.blur, filter.drop_shadow, filter.outer_glow (design 4.4). A shadow
// or glow straight on a paint of a shape or a text is drawn from the paint's
// distances in the same draw: an effect item per item it draws, all below
// them. Otherwise -- and a blur always -- the layer is one item (a group when
// it draws several), blurred through images (addBlur); a shadow or glow then
// draws that image's alpha below the layer. What the stack's layer applies to
// its layer as a whole (an opacity, a mask) makes the result one group.
void appendNeighbourhoodFilter(const Scope &scope, const Json &node, const Flattening &flattening,
                               std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    const auto id = nodeId(node);
    const auto filter = parseNeighbourhood(scope, node, ir.diagnostics);
    const auto &inputs = objectMember(node, "inputs");
    const auto layer = inputs.find("layer");
    if (layer == inputs.end()) throw std::invalid_argument(filter.op + " requires a layer edge: " + id);
    const auto *from = producer(scope, *layer);
    if (from == nullptr) throw std::invalid_argument("canvas layer edge names no node of its scope");
    // A blur of nothing is its layer.
    if (filter.blur && !filter.radius.edge && filter.radius.value <= 0.0) {
        auto inner = flattening;
        inner.stacks.insert(inner.stacks.begin(), id);
        return appendLayer(scope, *layer, inner, active, ir, items);
    }
    // The filter's own space: its layer before the stack's layer applies.
    auto inner = freshFlattening(flattening);
    inner.stacks.insert(inner.stacks.begin(), id);
    std::vector<CanvasItem> layer_items;
    std::vector<CanvasItem> result;
    std::string why = filter.blur ? "blur" : "no_distance";
    const auto op = nodeOp(*from);
    if (!filter.blur && (op == "shape.fill" || op == "shape.stroke")) {
        appendPaint(scope, *from, inner, layer_items);
        why = distanceRefusal(layer_items, filter);
        if (why.empty()) {
            for (const auto &body : layer_items) {
                auto effect = body;
                addEffect(effect, filter);
                result.push_back(std::move(effect));
            }
            for (auto &body : layer_items) result.push_back(std::move(body));
        }
    } else {
        appendLayer(scope, *layer, inner, active, ir, layer_items);
    }
    if (result.empty()) {
        if (layer_items.empty()) return;
        if (layer_items.size() == 1 && layer_items.front().source == CanvasItemSource::backdrop)
            throw GraphRegistryError("canvas.filter_backdrop_unsupported@1", {{"node", id}, {"op", filter.op}});
        // The stack's layer's map says what of the filter's space shows.
        auto shape = blurShape(listExtent(layer_items, *flattening.output), filter, flattening);
        if (!shape) {
            // None of it can show: a blur draws nothing, a shadow or glow its layer alone.
            if (filter.blur) return;
            result = std::move(layer_items);
        } else {
            auto source = layer_items.size() == 1
                              ? std::move(layer_items.front())
                              : groupItem(std::move(layer_items), inner.srgb, nodeId(*from), inner);
            auto nodes = std::vector<std::string>{id};
            nodes.insert(nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
            const auto blur = addBlur(std::move(*shape), source, *flattening.output, ir.diagnostics, why);
            if (filter.blur) {
                result.push_back(blurImageItem(*flattening.output, blur, nodes));
            } else {
                result.push_back(effectImageItem(*flattening.output, blur, filter, nodes));
                result.push_back(std::move(source));
            }
        }
    }
    const bool plain = flattening.opacity == 1.0 && flattening.opacity_edges.empty() && flattening.masks.empty();
    if (result.size() == 1 || plain) {
        for (auto &item : result) {
            applyFlattening(item, flattening);
            items.push_back(std::move(item));
        }
        return;
    }
    items.push_back(groupItem(std::move(result), flattening.srgb, id, flattening));
}

CanvasMask parseMask(const Json &image, const Flattening &flattening) {
    onlyKeys(image, {"asset", "offset", "size", "channel", "outside", "sampler"});
    const auto id = nodeId(image);
    if (!objectMember(image, "inputs").empty())
        throw std::invalid_argument("a mask's canvas.image takes no edge: " + id);
    CanvasMask mask;
    // The mask's bytes are coverage, never sRGB-decoded.
    mask.texture = {CanvasTexture::Origin::file, imageAsset(image), true, std::nullopt, {}, 0, nearestSampler(image)};
    mask.luminance = luminanceChannel(image);
    const auto &parameters = objectMember(image, "parameters");
    if (const auto outside = parameters.find("outside"); outside != parameters.end()) {
        mask.outside = number(*outside, "canvas.image outside: " + id);
        if (mask.outside < 0.0 || mask.outside > 1.0)
            throw std::invalid_argument("canvas.image outside must lie in 0..1: " + id);
    }
    double offset[2] = {0.0, 0.0};
    if (const auto found = parameters.find("offset"); found != parameters.end()) {
        if (!found->is_array() || found->size() != 2)
            throw std::invalid_argument("canvas.image offset must be [x, y]: " + id);
        offset[0] = number(found->at(0), "canvas.image offset: " + id);
        offset[1] = number(found->at(1), "canvas.image offset: " + id);
    }
    const auto size = requiredSize(image);
    mask.rect = {offset[0], offset[1], size[0], size[1]};
    mask.affine = flattening.affine;
    mask.node = id;
    return mask;
}

struct StackLayer {
    const Json *entry = nullptr;
    std::size_t index = 0;
    std::string input;
    const Json *edge = nullptr;
    bool visible = true;
    bool clip = false;
    bool pass_through = false;
    // CV-2b.
    CanvasBlend blend = CanvasBlend::normal;
};

// The context one layer's items join their list with.
Flattening layerFlattening(const Scope &scope, const Json &stack, const StackLayer &layer,
                           const Flattening &flattening, CanvasIr &ir, bool with_opacity, bool with_mask,
                           bool with_depth = true) {
    (void)ir;
    const auto id = nodeId(stack);
    const auto &inputs = objectMember(stack, "inputs");
    Flattening inner = flattening;
    inner.stacks.insert(inner.stacks.begin(), id);
    inner.pass_through = layer.pass_through;
    inner.own_opacity = false;
    // CV-6a: the layer's depth, under a canvas.place.
    if (with_depth && flattening.placed) {
        const auto depth_port = canvasStackDepthPort(layer.input);
        const auto edge = inputs.find(depth_port);
        const auto authored = layer.entry->find("depth");
        double value = 0.0;
        if (authored != layer.entry->end()) value = number(*authored, "canvas.stack layer depth: " + id);
        const Json where{{"node", id}, {"layer", layerName(*layer.entry, layer.index)}};
        if (flattening.flat) {
            if (edge != inputs.end() || value != 0.0)
                flattening.diagnostics->push_back(
                    {{"code", "canvas.depth_flattened"}, {"node", id}, {"layer", where.at("layer")}});
        } else {
            // The innermost layer an item is in names it in canvas.depth_order.
            inner.depth_layer = where;
            if (edge != inputs.end()) {
                if (inner.depth_edge)
                    throw std::invalid_argument("canvas.place adds at most one edge-driven layer depth to an item: " +
                                                id + "." + depth_port);
                inner.depth_edge = CanvasParameter{value, valueSource(scope, stack, depth_port, *edge)};
            } else {
                inner.depth += value;
            }
        }
    }
    if (with_opacity) {
        const auto opacity_port = canvasStackOpacityPort(layer.input);
        if (const auto opacity_edge = inputs.find(opacity_port); opacity_edge != inputs.end()) {
            inner.opacity_edges.push_back(valueSource(scope, stack, opacity_port, *opacity_edge));
            inner.own_opacity = true;
        } else if (const auto opacity = layer.entry->find("opacity"); opacity != layer.entry->end()) {
            const auto value = number(*opacity, "canvas.stack layer opacity: " + id);
            inner.opacity *= value;
            inner.own_opacity = value != 1.0;
        }
    }
    if (with_mask) {
        if (const auto mask = layer.entry->find("mask"); mask != layer.entry->end()) {
            const auto name = mask->get<std::string>();
            const auto edge = inputs.find(name);
            if (edge == inputs.end())
                throw GraphRegistryError("canvas.layer_input_missing@1",
                                         {{"node", id}, {"layer", layerName(*layer.entry, layer.index)}, {"input", name}});
            const auto *image = producer(scope, *edge);
            if (image == nullptr)
                throw GraphRegistryError("canvas.layer_input_missing@1",
                                         {{"node", id}, {"layer", layerName(*layer.entry, layer.index)}, {"input", name}});
            if (nodeOp(*image) != "canvas.image")
                throw GraphRegistryError("canvas.mask_unsupported@1", {{"node", id},
                                                                        {"layer", layerName(*layer.entry, layer.index)},
                                                                        {"op", nodeOp(*image)}});
            inner.masks.push_back(parseMask(*image, flattening));
        }
    }
    return inner;
}

// One unclipped layer and the clip layers right above it.
void appendClipUnit(const Scope &scope, const Json &stack, const StackLayer &base,
                    const std::vector<const StackLayer *> &clips, const Flattening &flattening,
                    std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    // A hidden base hides its clip layers too.
    if (!base.visible) return;
    std::vector<const StackLayer *> shown;
    for (const auto *clip : clips)
        if (clip->visible) shown.push_back(clip);
    // CV-2b: a layer that blends is one item, its layers composited first.
    if (shown.empty() && base.blend != CanvasBlend::normal) {
        auto item = singleItem(scope, *base.edge, layerFlattening(scope, stack, base, flattening, ir, true, true),
                               active, ir);
        if (!item) return;
        item->blend = base.blend;
        items.push_back(std::move(*item));
        return;
    }
    if (shown.empty())
        return appendLayer(scope, *base.edge, layerFlattening(scope, stack, base, flattening, ir, true, true), active,
                           ir, items);
    // A clip layer is drawn atop its base (Photoshop's, and psd-tools'
    // composite): the colours mix as if the base were opaque, the result keeps
    // the base's alpha. That reads the base alone under the clip layers, so the
    // base and its clip layers are composited into a group of their own, and
    // the base's opacity applies to the group. The base's mask stays on the
    // base: it is part of the alpha the group keeps.
    auto inner = freshFlattening(flattening);
    std::vector<CanvasItem> unit;
    const auto base_item =
        singleItem(scope, *base.edge, layerFlattening(scope, stack, base, inner, ir, false, true, false), active, ir);
    if (!base_item) return;
    unit.push_back(*base_item);
    for (const auto *clip : shown) {
        auto item = singleItem(scope, *clip->edge, layerFlattening(scope, stack, *clip, inner, ir, true, true), active, ir);
        if (!item) continue;
        item->clip_base = 0;
        item->blend = clip->blend;
        unit.push_back(std::move(*item));
    }
    auto outer = layerFlattening(scope, stack, base, flattening, ir, true, false);
    outer.pass_through = false;
    items.push_back(groupItem(std::move(unit), flattening.srgb, nodeId(*producer(scope, *base.edge)), outer));
    // The base's blend composites the base and its clip layers as one.
    items.back().blend = base.blend;
}

// A stack's layers, bottom to top, into `items` under `flattening`.
void appendStackLayers(const Scope &scope, const Json &stack, const Flattening &flattening,
                       std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    const auto id = nodeId(stack);
    const auto &layers = objectMember(stack, "parameters").at("layers");
    const auto &inputs = objectMember(stack, "inputs");
    std::vector<StackLayer> parsed;
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto &layer = layers.at(index);
        if (!layer.is_object()) throw std::invalid_argument("canvas.stack layer must be an object: " + id);
        for (const auto &[key, value] : layer.items()) {
            (void)value;
            if (key != "input" && key != "name" && key != "visible" && key != "opacity" && key != "blend" &&
                key != "mask" && key != "clip" && key != "pass_through" && key != "depth" && key != "bake")
                throw std::invalid_argument("canvas.stack layer does not take " + key + ": " + id);
        }
        // CV-5: read by the ship policy (canvasship.hpp) alone.
        if (const auto bake = layer.find("bake"); bake != layer.end() && *bake != "texture")
            throw std::invalid_argument("canvas.stack layer bake must be \"texture\": " + id);
        const auto input = layer.find("input");
        if (input == layer.end() || !input->is_string() || input->get_ref<const std::string &>().empty())
            throw std::invalid_argument("canvas.stack layer requires an input name: " + id);
        const auto name = input->get<std::string>();
        auto blend = CanvasBlend::normal;
        if (const auto found = layer.find("blend"); found != layer.end()) {
            const auto parsed = found->is_string() ? canvasBlendFromName(found->get<std::string>()) : std::nullopt;
            if (!parsed)
                throw GraphRegistryError("canvas.blend_unsupported@1",
                                         {{"node", id}, {"layer", layerName(layer, index)}, {"blend", *found}});
            blend = *parsed;
        }
        const auto edge = inputs.find(name);
        if (edge == inputs.end())
            throw GraphRegistryError("canvas.layer_input_missing@1",
                                     {{"node", id}, {"layer", layerName(layer, index)}, {"input", name}});
        StackLayer entry{&layer, index, name, &*edge};
        const auto flag = [&](const char *key) {
            const auto found = layer.find(key);
            if (found == layer.end()) return false;
            if (!found->is_boolean())
                throw std::invalid_argument(std::string{"canvas.stack layer "} + key + " must be a boolean: " + id);
            return found->get<bool>();
        };
        entry.visible = !layer.contains("visible") || flag("visible");
        entry.clip = flag("clip");
        entry.pass_through = flag("pass_through");
        entry.blend = blend;
        // A pass-through group composites its layers into the stack itself.
        if (entry.pass_through && blend != CanvasBlend::normal)
            throw std::invalid_argument("canvas.stack layer takes pass_through or a blend, not both: " + id);
        if (const auto mask = layer.find("mask");
            mask != layer.end() && (!mask->is_string() || mask->get_ref<const std::string &>().empty()))
            throw std::invalid_argument("canvas.stack layer mask must be an input name: " + id);
        if (entry.visible && producer(scope, *edge) == nullptr)
            throw GraphRegistryError("canvas.layer_input_missing@1",
                                     {{"node", id}, {"layer", layerName(layer, index)}, {"input", name}});
        parsed.push_back(entry);
    }
    for (std::size_t at = 0; at < parsed.size();) {
        if (parsed[at].clip)
            throw GraphRegistryError("canvas.clip_base_missing@1",
                                     {{"node", id}, {"layer", layerName(*parsed[at].entry, parsed[at].index)}});
        std::vector<const StackLayer *> clips;
        auto end = at + 1;
        for (; end < parsed.size() && parsed[end].clip; ++end) clips.push_back(&parsed[end]);
        appendClipUnit(scope, stack, parsed[at], clips, flattening, active, ir, items);
        at = end;
    }
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
    onlyKeys(stack, {"size", "layers", "blend_space", "bake_scale"});
    const auto &parameters = objectMember(stack, "parameters");
    (void)positiveSize(parameters, "canvas.stack " + id);
    // CV-5: read by the ship policy (canvasship.hpp) alone.
    if (const auto scale = parameters.find("bake_scale");
        scale != parameters.end() && !(number(*scale, "canvas.stack bake_scale: " + id) > 0.0))
        throw std::invalid_argument("canvas.stack bake_scale must be positive: " + id);
    const auto layers = parameters.find("layers");
    if (layers == parameters.end() || !layers->is_array())
        throw std::invalid_argument("canvas.stack requires a layers array: " + id);
    bool srgb = false;
    if (const auto space = parameters.find("blend_space"); space != parameters.end()) {
        if (*space == "srgb") srgb = true;
        else if (*space != "linear") throw std::invalid_argument("canvas.stack blend_space must be linear or srgb: " + id);
    }
    active.push_back(id);
    // Its layers prepend `id` to the trace themselves.
    Flattening inner = flattening;
    inner.pass_through = false;
    inner.own_opacity = false;
    if (flattening.pass_through && srgb == flattening.srgb) {
        // Its layers join this list in order; its opacity (and mask) is
        // multiplied into each of its items.
        if (flattening.own_opacity)
            ir.diagnostics.push_back({{"code", "canvas.group_opacity_flattened"}, {"node", id}});
        appendStackLayers(scope, stack, inner, active, ir, items);
    } else {
        // Composited into one image -- unless that is the same picture as
        // flattening it: the same blend space and at most one item, or
        // nothing (no opacity, no mask) applied to the group as a whole.
        auto fresh = freshFlattening(inner);
        fresh.srgb = srgb;
        std::vector<CanvasItem> group;
        appendStackLayers(scope, stack, fresh, active, ir, group);
        const bool plain = flattening.opacity == 1.0 && flattening.opacity_edges.empty() && flattening.masks.empty();
        // CV-2b: an item that reads its backdrop reads this stack's alone.
        const bool isolated = std::any_of(group.begin(), group.end(), canvasItemReadsBackdrop);
        if (srgb == flattening.srgb && !isolated && (group.size() <= 1 || plain)) {
            if (flattening.own_opacity && !group.empty())
                ir.diagnostics.push_back({{"code", "canvas.group_opacity_flattened"}, {"node", id}});
            const auto first = items.size();
            for (auto &item : group) {
                applyFlattening(item, flattening);
                if (item.clip_base) *item.clip_base += first;
                items.push_back(std::move(item));
            }
        } else if (!group.empty()) {
            items.push_back(groupItem(std::move(group), srgb, id, flattening));
        }
    }
    active.pop_back();
}

void appendLayer(const Scope &scope, const Json &endpoint, const Flattening &flattening,
                 std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    const auto *from = producer(scope, endpoint);
    if (from == nullptr) throw std::invalid_argument("canvas layer edge names no node of its scope");
    const auto op = nodeOp(*from);
    if (op.starts_with("filter.") && !isCanvasFilter(op))
        throw GraphRegistryError("canvas.op_unsupported@1", {{"node", nodeId(*from)}, {"op", op}});
    if (op == "canvas.stack") return appendStack(scope, *from, flattening, active, ir, items);
    if (op == "canvas.transform") return appendTransform(scope, *from, flattening, active, ir, items);
    if (flattening.pass_through)
        throw std::invalid_argument("pass_through is for a nested canvas.stack, not " + op + ": " + nodeId(*from));
    if (op == "shape.fill" || op == "shape.stroke") return appendPaint(scope, *from, flattening, items);
    if (op == "canvas.image") return appendImage(scope, *from, flattening, items);
    if (op == "canvas.from_image") return appendFromImage(scope, *from, flattening, items);
    if (op == "canvas.color") return appendColor(scope, *from, flattening, items);
    if (op == "canvas.backdrop") return appendBackdrop(*from, flattening, items);
    if (isCanvasColorFilter(op)) return appendFilter(scope, *from, flattening, active, ir, items);
    if (isCanvasNeighbourhoodFilter(op)) return appendNeighbourhoodFilter(scope, *from, flattening, active, ir, items);
    throw std::invalid_argument("a canvas layer comes from shape.fill, shape.stroke, canvas.image, "
                                "canvas.from_image, canvas.color, canvas.backdrop, filter.*, canvas.transform "
                                "or canvas.stack, not " +
                                op + ": " + nodeId(*from));
}

CanvasOutput buildOutput(const Scope &scope, const Json &node, CanvasIr &ir, CanvasFontLibrary &fonts) {
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
    // The output's image is linear: a linear stack draws straight into it, an
    // sRGB one is composited first and decoded as it is read.
    Flattening top;
    top.pass_through = true;
    top.output = &output;
    top.fonts = &fonts;
    appendLayer(scope, *layer, top, active, ir, output.items);
    return output;
}

// --- CV-6a: canvas.place ----------------------------------------------------

bool placeFlag(const Json &node, const char *key, bool fallback) {
    const auto &parameters = objectMember(node, "parameters");
    const auto found = parameters.find(key);
    if (found == parameters.end()) return fallback;
    if (!found->is_boolean())
        throw std::invalid_argument(std::string{"canvas.place "} + key + " must be a boolean: " + nodeId(node));
    return found->get<bool>();
}

// The image edge a canvas.place draws onto or tests against.
CanvasValueSource placeImageEdge(const Scope &scope, const Json &node, const std::string &port, const Json &edge) {
    if (const auto *from = producer(scope, edge); from == nullptr)
        throw std::invalid_argument("canvas.place " + port + " edge names no node of its scope: " + nodeId(node));
    return valueSource(scope, node, port, edge);
}

CanvasOutput buildPlace(const Scope &scope, const Json &node, CanvasIr &ir, CanvasFontLibrary &fonts) {
    const auto id = nodeId(node);
    onlyKeys(node, {"position", "rotation", "scale", "pixels_per_unit", "anchor", "depth_test", "depth_write",
                    "double_sided"});
    const auto &parameters = objectMember(node, "parameters");
    const auto &inputs = objectMember(node, "inputs");
    for (const auto *port : {"layer", "color"})
        if (!inputs.contains(port)) throw GraphRegistryError("canvas.place_unconnected@1", {{"node", id}, {"port", port}});
    CanvasPlacement placement;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const char *const components[3] = {"x", "y", "z"};
        placement.position[axis] = parameter(scope, node, "position", 3, axis,
                                             std::string{"position."} + components[axis], 0.0);
        placement.rotation[axis] = parameter(scope, node, "rotation", 3, axis,
                                             std::string{"rotation."} + components[axis], 0.0);
    }
    placement.scale = parameter(scope, node, "scale", std::nullopt, 0, "scale", 1.0);
    if (const auto found = parameters.find("pixels_per_unit"); found != parameters.end()) {
        placement.pixels_per_unit = number(*found, "canvas.place pixels_per_unit: " + id);
        if (!(placement.pixels_per_unit > 0.0))
            throw std::invalid_argument("canvas.place pixels_per_unit must be positive: " + id);
    }
    if (const auto found = parameters.find("anchor"); found != parameters.end()) {
        if (!found->is_array() || found->size() != 2)
            throw std::invalid_argument("canvas.place anchor must be [x, y]: " + id);
        placement.anchor = {number(found->at(0), "canvas.place anchor: " + id),
                            number(found->at(1), "canvas.place anchor: " + id)};
    }
    placement.depth_test = placeFlag(node, "depth_test", true);
    placement.depth_write = placeFlag(node, "depth_write", false);
    placement.double_sided = placeFlag(node, "double_sided", true);
    placement.color = placeImageEdge(scope, node, "color", inputs.at("color"));
    if (const auto depth = inputs.find("depth"); depth != inputs.end())
        placement.depth = placeImageEdge(scope, node, "depth", *depth);
    else if (placement.depth_write)
        throw std::invalid_argument("canvas.place depth_write writes the depth edge, which it does not have: " + id);

    const auto *from = producer(scope, inputs.at("layer"));
    if (from == nullptr) throw GraphRegistryError("canvas.place_unconnected@1", {{"node", id}, {"port", "layer"}});
    if (nodeOp(*from) != "canvas.stack")
        throw std::invalid_argument("canvas.place reads a canvas.stack, whose size is the canvas: " + id);
    const auto size = positiveSize(objectMember(*from, "parameters"), "canvas.stack " + nodeId(*from));
    if (!size) throw std::invalid_argument("canvas.stack read by a canvas.place requires a size: " + nodeId(*from));

    CanvasOutput output;
    output.id = id;
    output.scope = scope.name;
    output.size = *size;
    output.fit = CanvasFit::stretch;
    // A group is an image of one pixel per stack unit.
    output.image_size = std::array<std::uint32_t, 2>{static_cast<std::uint32_t>(std::ceil((*size)[0])),
                                                     static_cast<std::uint32_t>(std::ceil((*size)[1]))};
    output.format = default_canvas_format;
    output.placement = std::move(placement);

    auto diagnostics = Json::array();
    std::vector<std::string> active;
    Flattening top;
    top.pass_through = true;
    top.output = &output;
    top.fonts = &fonts;
    top.placed = true;
    top.diagnostics = &diagnostics;
    appendLayer(scope, inputs.at("layer"), top, active, ir, output.items);

    // CV-2b: what is below a layer drawn straight into the scene is the scene
    // and the layers at other depths, not an image a blend could read. A
    // nested stack is composited first, so its blends are drawn there.
    for (const auto &item : output.items) {
        if (!canvasItemReadsBackdrop(item)) continue;
        const auto &where = item.depth_layer;
        throw GraphRegistryError("canvas.place_backdrop_unsupported@1",
                                 {{"node", where.is_null() ? Json(id) : where.at("node")},
                                  {"layer", where.is_null() ? Json(nullptr) : where.at("layer")}});
    }

    // Back to front: by depth, then the stack's order. A layer that moves
    // before one above it in the stack is reported.
    const auto total = [](const CanvasItem &item) {
        return item.depth + (item.depth_edge ? item.depth_edge->value : 0.0);
    };
    double deepest = -std::numeric_limits<double>::infinity();
    for (const auto &item : output.items) {
        if (item.clip_base) throw std::logic_error("canvas.place: a clip layer outside its group");
        if (total(item) < deepest) {
            const auto &where = item.depth_layer;
            diagnostics.push_back({{"code", "canvas.depth_order"},
                                   {"node", where.is_null() ? Json(id) : where.at("node")},
                                   {"layer", where.is_null() ? Json(nullptr) : where.at("layer")}});
        }
        deepest = std::max(deepest, total(item));
    }
    std::stable_sort(output.items.begin(), output.items.end(),
                     [&](const CanvasItem &a, const CanvasItem &b) { return total(a) < total(b); });
    std::set<std::string> seen;
    for (auto &diagnostic : diagnostics)
        if (seen.insert(diagnostic.dump()).second) ir.diagnostics.push_back(std::move(diagnostic));
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
        // A filter the lowering does not implement.
        if (nodeOp(node).starts_with("filter.") && !isCanvasFilter(nodeOp(node)))
            throw GraphRegistryError("canvas.op_unsupported@1", {{"node", nodeId(node)}, {"op", nodeOp(node)}});
        if (!isCanvasGraphOperation(nodeOp(node))) continue;
        if (bodies.contains(name)) throw GraphRegistryError("canvas.in_loop_body@1", {{"node", nodeId(node)}});
        if (nodeOp(node) == "canvas.output" || nodeOp(node) == "canvas.place") outputs.push_back(nodeId(node));
    }
    for (const auto &node : *nodes) {
        const auto id = nodeId(node);
        for (const auto &output : outputs)
            if (id.starts_with(reservedPrefix(output))) throw GraphRegistryError("canvas.reserved_id@1", {{"id", id}});
    }
}

// --- CV-2a: canvas definitions, expanded before the walk ------------------

// A definition whose instances carry layers or geometry: an output produced
// by a canvas op (other than canvas.output, whose image is an ordinary
// image), or a graph.input feeding a layer or geometry port.
bool isCanvasDefinition(const Json &body) {
    const auto nodes = body.find("nodes");
    if (nodes == body.end() || !nodes->is_array()) return false;
    const auto scope = indexScope({}, *nodes);
    for (const auto &[port, endpoint] : objectMember(body, "outputs").items()) {
        (void)port;
        if (const auto *from = producer(scope, endpoint);
            from != nullptr && isCanvasGraphOperation(nodeOp(*from)) && nodeOp(*from) != "canvas.output" &&
            nodeOp(*from) != "canvas.place")
            return true;
    }
    for (const auto &node : *nodes) {
        if (!isCanvasGraphOperation(nodeOp(node))) continue;
        for (const auto &[port, endpoint] : objectMember(node, "inputs").items()) {
            const bool layer_port = nodeOp(node) == "canvas.stack" ? port.find('.') == std::string::npos
                                                                   : port == "layer" || port == "geometry";
            if (!layer_port) continue;
            if (const auto *from = producer(scope, endpoint); from != nullptr && nodeOp(*from) == "graph.input")
                return true;
        }
    }
    return false;
}

template <typename Rewrite> void eachEndpoint(Json &inputs, Rewrite rewrite) {
    if (!inputs.is_object()) return;
    for (auto &[port, endpoint] : inputs.items()) {
        (void)port;
        if (endpoint.is_object()) rewrite(endpoint);
        else if (endpoint.is_array())
            for (auto &entry : endpoint)
                if (entry.is_object()) rewrite(entry);
    }
}

// One pass over a scope: each instance of a canvas definition replaced by
// the definition's nodes. Returns whether anything was expanded.
bool expandScopeInstances(Json &holder, const Json &definitions, const std::set<std::string> &canvas) {
    const auto nodes = holder.find("nodes");
    if (nodes == holder.end() || !nodes->is_array()) return false;
    std::map<std::string, Json> exposed;
    auto expanded = Json::array();
    for (const auto &node : *nodes) {
        const auto definition = node.is_object() ? node.find("definition") : node.end();
        if (nodeOp(node) != "graph.instance" || definition == node.end() || !definition->is_string() ||
            !canvas.contains(definition->get<std::string>())) {
            expanded.push_back(node);
            continue;
        }
        const auto id = nodeId(node);
        const auto name = definition->get<std::string>();
        const auto &body = definitions.at(name);
        const auto &inner = body.at("nodes");
        const bool inputs = !objectMember(node, "inputs").empty() ||
                            std::any_of(inner.begin(), inner.end(),
                                        [](const Json &entry) { return nodeOp(entry) == "graph.input"; });
        if (inputs)
            throw GraphRegistryError("canvas.instance_inputs_unsupported@1", {{"node", id}, {"definition", name}});
        const auto prefix = id + "$";
        const auto rename = [&](Json &endpoint) {
            if (const auto found = endpoint.find("node"); found != endpoint.end() && found->is_string())
                *found = prefix + found->get<std::string>();
        };
        for (const auto &entry : inner) {
            auto copy = entry;
            copy["id"] = prefix + nodeId(entry);
            if (copy.contains("inputs")) eachEndpoint(copy["inputs"], rename);
            expanded.push_back(std::move(copy));
        }
        auto outputs = objectMember(body, "outputs");
        eachEndpoint(outputs, rename);
        exposed.emplace(id, std::move(outputs));
    }
    if (exposed.empty()) return false;
    // Edges that read an instance read what its definition exposes.
    const auto redirect = [&](Json &endpoint) {
        const auto node = endpoint.find("node");
        if (node == endpoint.end() || !node->is_string()) return;
        const auto found = exposed.find(node->get<std::string>());
        if (found == exposed.end()) return;
        const auto port = endpoint.value("port", std::string{});
        if (!found->second.contains(port))
            throw std::invalid_argument("unknown subgraph output: " + found->first + "." + port);
        endpoint = found->second.at(port);
    };
    std::set<std::string> ids;
    for (auto &node : expanded) {
        if (!ids.insert(nodeId(node)).second) throw GraphRegistryError("canvas.reserved_id@1", {{"id", nodeId(node)}});
        if (node.is_object() && node.contains("inputs")) eachEndpoint(node["inputs"], redirect);
    }
    if (holder.contains("outputs")) eachEndpoint(holder["outputs"], redirect);
    holder["nodes"] = std::move(expanded);
    return true;
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

// --- CV-4: the glyph atlases ----------------------------------------------

// Each item of an output's lists, its groups' too (CV-3a: and its blurs' sources).
template <typename Visit> void eachOutputItem(CanvasOutput &output, Visit visit) {
    for (auto &item : output.items) visit(item);
    for (auto &group : output.groups)
        for (auto &item : group.items) visit(item);
    for (auto &blur : output.blurs) visit(blur.source);
}

// Bakes one atlas per font face the output's glyph items read (in first-use
// order), then fills in each glyph item's cell, uv and texture, and cuts a
// constant stroke width the atlas's distances do not reach.
void bakeGlyphAtlases(CanvasOutput &output, CanvasFontLibrary &fonts, const CanvasLoweringOptions &options,
                      Json &diagnostics) {
    std::vector<std::size_t> order;
    std::map<std::size_t, std::set<int>> glyphs;
    std::map<std::size_t, std::uint32_t> cells;
    eachOutputItem(output, [&](CanvasItem &item) {
        if (!item.glyph) return;
        const auto font = item.glyph->font;
        if (!glyphs.contains(font)) order.push_back(font);
        glyphs[font].insert(item.glyph->glyph);
        const auto wanted = item.glyph->cell != 0                                 ? item.glyph->cell
                            : item.glyph->codepoint >= canvas_glyph_cjk_first ? canvas_glyph_cell_cjk
                                                                                : canvas_glyph_cell_latin;
        cells[font] = std::max(cells[font], wanted);
    });
    if (order.empty()) return;
    std::map<std::size_t, std::pair<std::size_t, CanvasBakedAtlas>> baked;
    for (const auto font : order) {
        auto &face = fonts.face(font);
        auto atlas = bakeCanvasAtlas(face, {glyphs[font].begin(), glyphs[font].end()}, cells[font]);
        const auto &png = atlas.pixels->png;
        const auto *bytes = reinterpret_cast<const unsigned char *>(png.data());
        const auto digest = picosha2::hash256_hex_string(bytes, bytes + png.size());
        const auto stem =
            "canvas_" + fileStem(output.id) + "_font" + std::to_string(output.atlases.size()) + "_" + digest.substr(0, 8);
        const auto path = options.asset_directory + (options.directory_per_stem ? "/" + stem : std::string{}) + "/" +
                          stem + ".png";
        baked.emplace(font, std::pair{output.atlases.size(), atlas});
        output.atlases.push_back({face.reference, face.index, cells[font], stem, path, "project://" + path, atlas.pixels});
    }
    // The widest stroke each stroke node may have, in stack units: the
    // distances stop `range` texels out, and the last texel is the edge's
    // antialiasing.
    std::map<std::string, std::pair<double, double>> stroke_limits; // node -> (limit, range)
    std::set<std::string> effects_cut;
    eachOutputItem(output, [&](CanvasItem &item) {
        if (!item.glyph) return;
        auto &face = fonts.face(item.glyph->font);
        const auto &[index, atlas] = baked.at(item.glyph->font);
        const auto &entry = output.atlases[index];
        const auto placement = canvasGlyphPlacement(face.glyph(item.glyph->glyph), face.file->unitsPerEm(), entry.cell);
        // Texels per em.
        const auto em = placement.scale * face.file->unitsPerEm();
        const auto [column, row] = atlas.cells.at(item.glyph->glyph);
        const double cell = entry.cell;
        const double values[9] = {item.glyph->pen[0] - placement.x0 / em,
                                  item.glyph->pen[1] - placement.y0 / em,
                                  cell / em,
                                  cell / em,
                                  column * cell / atlas.pixels->width,
                                  row * cell / atlas.pixels->height,
                                  cell / atlas.pixels->width,
                                  cell / atlas.pixels->height,
                                  cell};
        for (std::size_t k = 0; k < 9; ++k) item.parameters.at(3 + k) = CanvasParameter{values[k], std::nullopt};
        item.texture = {CanvasTexture::Origin::file, entry.asset, true, std::nullopt, {}, 0, false};
        // CV-3a: a shadow or glow reaches no further than the atlas's
        // distances (past them they would stop growing): cut to that.
        if (item.effect) {
            const auto own = canvasItemKind(item) - canvas_kind_effect_offset;
            auto &limit = item.parameters.at(kindParameterCount(own) + 3).value;
            const auto range = canvas_glyph_range * std::abs(item.parameters.at(2).value) / em;
            const bool stroke = item.paint == CanvasPaint::stroke;
            const auto base = canvas_glyph_parameter_count;
            const auto outset =
                stroke ? strokeOutset(item.parameters.at(base + 4).value, item.parameters.at(base + 5).value) : 0.0;
            const auto reach = std::max(range - outset, 0.0);
            if (limit > reach + 1e-9) {
                if (effects_cut.insert(item.effect->node).second)
                    diagnostics.push_back({{"code", "canvas.glyph_range_exceeded"}, {"node", item.effect->node},
                                           {"radius", limit}, {"range", range}, {"limit", reach}});
                limit = reach;
            }
        }
        if (item.paint != CanvasPaint::stroke) return;
        const auto size = std::abs(item.parameters.at(2).value);
        if (item.parameters.at(canvas_glyph_parameter_count + 4).edge || !(size > 0.0)) return;
        const auto align = item.parameters.at(canvas_glyph_parameter_count + 5).value;
        const auto limit = (align > 0.5 ? 1.0 : 2.0) * (canvas_glyph_range - 1.0) * size / em;
        auto [found, inserted] = stroke_limits.emplace(item.nodes.front(), std::pair{limit, canvas_glyph_range * size / em});
        if (!inserted && limit < found->second.first) found->second = {limit, canvas_glyph_range * size / em};
    });
    if (stroke_limits.empty()) return;
    std::set<std::string> reported;
    eachOutputItem(output, [&](CanvasItem &item) {
        if (!item.glyph || item.paint != CanvasPaint::stroke) return;
        const auto found = stroke_limits.find(item.nodes.front());
        if (found == stroke_limits.end()) return;
        auto &width = item.parameters.at(canvas_glyph_parameter_count + 4).value;
        const auto [limit, range] = found->second;
        if (std::abs(width) <= limit) return;
        if (reported.insert(found->first).second)
            diagnostics.push_back({{"code", "canvas.glyph_range_exceeded"}, {"node", found->first}, {"width", width},
                                   {"range", range}, {"limit", limit}});
        width = std::copysign(limit, width);
    });
}

const char *const parameter_layout_comment =
    "// Item table, one mat4 per item (column c is it[c]):\n"
    "//   it[0] = (kind, bake offset, source offset or -1, constant opacity)\n"
    "//   it[1] = (opacity edge offset or -1, opacity edge count, mask offset or -1, mask count)\n"
    "//   it[2] = (a, b, c, d)    the 2D linear map from the item's stack to the canvas, mat2 columns\n"
    "//   it[3] = (tx, ty, image slot or -1, a clip layer's base: its index in the image's items, or -1)\n"
    "// Bake table: an item's parameters from its bake offset, in this order --\n"
    "//   rect: size.x size.y center.x center.y corner_radius rotation(deg)\n"
    "//   ellipse: size.x size.y center.x center.y rotation(deg)\n"
    "//   then fill: color.r color.g color.b color.a (sRGB, straight alpha)\n"
    "//   or stroke: color.r color.g color.b color.a width align(0 center, 1 inner, 2 outer)\n"
    "//   image: offset.x offset.y size.x size.y alpha(0 straight, 1 premultiplied, 2 opaque) encode\n"
    "//   color: color.r color.g color.b color.a; group: encode (0 none, 1 to sRGB, 2 to linear)\n"
    "//   glyph: origin.x origin.y size cell.x cell.y cell.w cell.h (em) uv.x uv.y uv.w uv.h texels,\n"
    "//          then fill's or stroke's\n"
    "// At the source offset, one entry per parameter: 0 reads the bake table,\n"
    "// k reads frame table entry k - 1. At the opacity edge offset, the frame\n"
    "// table entries whose values multiply the constant opacity. At the mask\n"
    "// offset, 13 numbers per mask: slot, luminance, outside, offset.x offset.y\n"
    "// size.x size.y, then the map from the mask's stack to the canvas (a b c d tx ty).\n";

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

// What one draw's shaders have to handle.
struct DrawShape {
    std::set<std::uint32_t> kinds;
    bool frame = false;
    // The blend space of the image it draws into.
    bool srgb = false;
    std::size_t slots = 0;
    bool masks = false;
    // CV-2b: the (blend, filters) of style 1, 2, ...; whether the draw goes
    // atop (clip layers, backdrops); the slot of the backdrop's copy.
    std::vector<std::pair<CanvasBlend, std::vector<CanvasFilterKind>>> styles;
    bool atop = false;
    std::optional<std::size_t> backdrop_slot;
    // CV-6a: parameters each item carries past its kind's own (a placed
    // item's depth), before its filter numbers.
    std::size_t extra_parameters = 0;
    // CV-5, the ship policy: per kind (less its style), the parameters every
    // item of the kind in the draw holds as one constant. The shader reads
    // those as literals; the tables keep the rest, in order.
    std::map<std::uint32_t, std::vector<std::optional<double>>> constants;
};

// CV-5: a folded number as the table would hold it (a float), in GLSL.
std::string glslLiteral(double value) {
    const auto text = glslNumber(static_cast<double>(static_cast<float>(value)));
    return text.starts_with('-') ? "(" + text + ")" : text;
}

// Parameter `index` of an item of `kind` (less its style) in the shader: a
// literal when the draw folded it, else its read from the tables, numbered
// among the parameters the tables keep.
std::string parameterRead(const DrawShape &draw, std::uint32_t kind, std::size_t index) {
    const auto found = draw.constants.find(kind);
    if (found == draw.constants.end()) return "canvasParam(it, " + std::to_string(index) + "u)";
    if (found->second.at(index)) return glslLiteral(*found->second.at(index));
    const auto kept = std::count_if(found->second.begin(), found->second.begin() + static_cast<std::ptrdiff_t>(index),
                                    [](const std::optional<double> &value) { return !value.has_value(); });
    return "canvasParam(it, " + std::to_string(kept) + "u)";
}

// The parameters of `kind` the draw's tables keep.
std::size_t keptParameterCount(const DrawShape &draw, std::uint32_t kind, std::size_t count) {
    const auto found = draw.constants.find(kind);
    if (found == draw.constants.end()) return count;
    return count - static_cast<std::size_t>(std::count_if(found->second.begin(), found->second.end(),
                                                          [](const std::optional<double> &value) {
                                                              return value.has_value();
                                                          }));
}

// CV-2b: the kind's parameter count, where an item's filter numbers start.
// CV-3a: an effect kind's, the kind it is the effect of and then its own.
std::size_t kindParameterCount(std::uint32_t kind) {
    if (kind >= canvas_kind_effect_offset)
        return kindParameterCount(kind - canvas_kind_effect_offset) + canvas_effect_parameter_count;
    switch (kind) {
    case canvas_kind_image: return 6;
    case canvas_kind_color: return 4;
    case canvas_kind_group: return 1;
    case canvas_kind_glyph_fill: return canvas_glyph_parameter_count + canvasPaintParameterCount(CanvasPaint::fill);
    case canvas_kind_glyph_stroke: return canvas_glyph_parameter_count + canvasPaintParameterCount(CanvasPaint::stroke);
    case canvas_kind_backdrop: return 0;
    case canvas_kind_effect_image: return canvas_effect_image_parameter_count;
    default: break;
    }
    return canvasShapeParameterCount(kind < 2 ? CanvasShape::rect : CanvasShape::ellipse) +
           canvasPaintParameterCount(kind % 2 == 1 ? CanvasPaint::stroke : CanvasPaint::fill);
}

// CV-3a: the kind an effect kind is the effect of (itself for any other).
std::uint32_t effectSourceKind(std::uint32_t kind) {
    return kind >= canvas_kind_effect_offset ? kind - canvas_kind_effect_offset : kind;
}

bool hasEffectKinds(const DrawShape &draw) {
    return std::any_of(draw.kinds.begin(), draw.kinds.end(), [](std::uint32_t kind) {
        return kind == canvas_kind_effect_image || kind >= canvas_kind_effect_offset;
    });
}

// The line that reads an item's kind: past 256 it carries a style.
std::string kindLine(const DrawShape &draw) {
    return std::string{"    uint kind = uint(it[0].x + 0.5)"} + (draw.styles.empty() ? "" : " % 256u") + ";\n";
}

// CV-2b: the blends and filters of the draw's styles, the backdrop read, and
// canvasStyle -- an item's colour through its filters and over its backdrop.
std::string styleFunctions(const DrawShape &draw) {
    std::string text;
    if (draw.backdrop_slot) {
        text += "const uint CANVAS_BACKDROP_SLOT = " + std::to_string(*draw.backdrop_slot) + "u;\n\n";
        text += R"glsl(// What the image held under this draw's first item (its copy), premultiplied.
vec4 canvasBackdrop() {
    return canvasSample(CANVAS_BACKDROP_SLOT, gl_FragCoord.xy / canvasTargetSize());
}

)glsl";
    }
    if (draw.styles.empty() && !draw.backdrop_slot) return text;
    text += R"glsl(vec3 canvasStraight(vec4 c) {
    return c.a > 0.0 ? clamp(c.rgb / c.a, 0.0, 1.0) : vec3(0.0);
}

)glsl";
    if (draw.styles.empty()) return text;
    std::set<CanvasBlend> blends;
    std::set<CanvasFilterKind> filters;
    for (const auto &[blend, list] : draw.styles) {
        if (blend != CanvasBlend::normal) blends.insert(blend);
        filters.insert(list.begin(), list.end());
    }
    text += canvasBlendGlsl(blends);
    text += canvasFilterGlsl(filters);
    text += "uint canvasParamCount(uint kind) {\n    switch (kind) {\n";
    for (const auto kind : draw.kinds)
        text += "    case " + std::to_string(kind) + "u: return " +
                std::to_string(keptParameterCount(draw, kind, kindParameterCount(kind)) + draw.extra_parameters) +
                "u;\n";
    text += "    default: return 0u;\n    }\n}\n\n";
    text += "vec4 canvasStyle(mat4 it, vec4 color) {\n";
    text += "    uint kind = uint(it[0].x + 0.5);\n";
    // CV-3a: the filters' numbers are the item's parameters past its kind's
    // (canvasParam reads a driven one from the frame table).
    text += "    uint r = canvasParamCount(kind % 256u);\n";
    text += "    vec3 c = canvasStraight(color);\n";
    text += "    switch (kind / 256u) {\n";
    for (std::size_t style = 0; style < draw.styles.size(); ++style) {
        const auto &[blend, list] = draw.styles[style];
        text += "    case " + std::to_string(style + 1) + "u: // " + std::string{canvasBlendName(blend)};
        for (const auto kind : list) text += ", " + std::string{canvasFilterOperation(kind)};
        text += "\n";
        for (const auto kind : list) {
            text += "        c = canvasFilter" + std::to_string(static_cast<int>(kind)) + "(c, it, r);\n";
            if (canvasFilterConstantCount(kind) > 0)
                text += "        r += " + std::to_string(canvasFilterConstantCount(kind)) + "u;\n";
        }
        if (blend != CanvasBlend::normal) {
            text += "        {\n            vec4 back = canvasBackdrop();\n";
            text += "            vec3 mixed = canvasBlend" + std::to_string(static_cast<int>(blend)) +
                    "(canvasStraight(back), c);\n";
            // Atop the backdrop counts as opaque (its alpha is kept); over
            // it, Cs (1 - ab) + ab B(Cb, Cs), premultiplied.
            text += draw.atop ? "            return vec4(mixed * color.a, color.a);\n"
                              : "            return vec4(c * color.a * (1.0 - back.a) + mixed * color.a * back.a, "
                                "color.a);\n";
            text += "        }\n";
        } else {
            text += "        return vec4(c * color.a, color.a);\n";
        }
    }
    text += "    default:\n        return color;\n    }\n}\n\n";
    return text;
}

std::string generatedBanner(const CanvasOutput &output, const std::string &raster) {
    std::string text = "// Generated by the pelican.canvas lowering for canvas.output \"" + output.id + "\"";
    if (raster != output.id) text += ", draw \"" + raster + "\"";
    return text + ". Do not edit.\n";
}

// The canvas rectangle on the image: canvas units times scale, plus offset (pixels).
std::string fitFunctions(const CanvasOutput &output) {
    std::string text =
        "const vec2 CANVAS_SIZE = vec2(" + glslNumber(output.size[0]) + ", " + glslNumber(output.size[1]) + ");\n";
    text += "vec2 canvasTargetSize() {\n";
    if (output.image_size)
        text += "    return vec2(" + glslNumber((*output.image_size)[0]) + ", " + glslNumber((*output.image_size)[1]) +
                ");\n";
    else
        text += "    return pelicanFrame.resolution.xy * " + glslNumber(output.extent_scale) + ";\n";
    text += "}\n\n";
    text += R"glsl(void canvasFit(out vec2 scale, out vec2 offset) {
    vec2 target = canvasTargetSize();
    scale = target / CANVAS_SIZE;
)glsl";
    if (output.fit == CanvasFit::contain) text += "    scale = vec2(min(scale.x, scale.y));\n";
    text += R"glsl(    offset = (target - CANVAS_SIZE * scale) * 0.5;
}
)glsl";
    return text;
}

// The cases of the vertex shader's switch on an item's kind: its `center` and
// the half extent `bound` of its rectangle in its own stack.
std::string itemBoundsCases(const DrawShape &draw) {
    std::string text;
    for (const auto kind : draw.kinds) {
        const auto p = [&](std::size_t index) { return parameterRead(draw, kind, index); };
        // CV-3a: an effect kind is its source's quad, moved by the offset and
        // grown by its reach.
        const auto own = effectSourceKind(kind);
        const auto effect = [&] {
            if (own == kind) return std::string{};
            const auto e = [&](std::size_t index) { return p(kindParameterCount(own) + index); };
            return "        center += vec2(" + e(0) + ", " + e(1) + ");\n        bound += vec2(" +
                   glslNumber(effect_reach) + " * clamp(" + e(2) + ", 0.0, " + e(3) + "));\n";
        };
        text += "    case " + std::to_string(kind) + "u: { // " + std::string{canvasItemKindName(kind)} + "\n";
        if (own == canvas_kind_image || own == canvas_kind_effect_image) {
            text += "        vec2 size = vec2(" + p(2) + ", " + p(3) + ");\n";
            text += "        center = vec2(" + p(0) + ", " + p(1) + ") + 0.5 * size;\n";
            text += "        bound = 0.5 * abs(size);\n";
            text += "        break;\n    }\n";
            continue;
        }
        if (own == canvas_kind_glyph_fill || own == canvas_kind_glyph_stroke) {
            text += "        float s = " + p(2) + ";\n";
            text += "        vec2 cell_size = s * vec2(" + p(5) + ", " + p(6) + ");\n";
            text += "        center = vec2(" + p(0) + ", " + p(1) + ") +\n";
            text += "                 s * vec2(" + p(3) + ", " + p(4) + ") + 0.5 * cell_size;\n";
            text += "        bound = 0.5 * abs(cell_size);\n";
            text += effect();
            text += "        break;\n    }\n";
            continue;
        }
        if (own == canvas_kind_color || own == canvas_kind_group || own == canvas_kind_backdrop) {
            text += "        center = 0.5 * CANVAS_SIZE;\n";
            text += "        bound = 0.5 * CANVAS_SIZE;\n";
            text += "        break;\n    }\n";
            continue;
        }
        const auto shape = own < 2 ? CanvasShape::rect : CanvasShape::ellipse;
        const bool stroke = own % 2 == 1;
        const auto rotation = shape == CanvasShape::rect ? 5 : 4;
        text += "        vec2 half_size = 0.5 * abs(vec2(" + p(0) + ", " + p(1) + "));\n";
        text += "        center = vec2(" + p(2) + ", " + p(3) + ");\n";
        text += "        float rotation = radians(" + p(rotation) + ");\n";
        text += "        float c = abs(cos(rotation)), s = abs(sin(rotation));\n";
        if (shape == CanvasShape::rect)
            text += "        bound = vec2(c * half_size.x + s * half_size.y, s * half_size.x + c * half_size.y);\n";
        else
            text += "        bound = vec2(length(vec2(half_size.x * c, half_size.y * s)),\n"
                    "                     length(vec2(half_size.x * s, half_size.y * c)));\n";
        if (stroke)
            text += "        bound += canvasStrokeOutset(" + p(parameterIndex(shape, 4)) + ", " +
                    p(parameterIndex(shape, 5)) + ");\n";
        text += effect();
        text += "        break;\n    }\n";
    }
    return text;
}

std::string vertexShader(const CanvasOutput &output, const std::string &raster, const DrawShape &draw) {
    std::string text = shader_header;
    text += generatedBanner(output, raster);
    text += "// One instance draws one item: 6 vertices, the item's bounding quad, no vertex buffer.\n";
    text += parameter_layout_comment;
    text += shader_includes;
    text += R"glsl(
layout(location = 0) out vec2 vCanvas;
layout(location = 1) flat out uint vItem;

)glsl";
    text += fitFunctions(output);
    text += "\n";
    text += parameterFunctions(draw.frame);
    text += R"glsl(
float canvasStrokeOutset(float width, float align) {
    return align > 1.5 ? abs(width) : (align > 0.5 ? 0.0 : 0.5 * abs(width));
}

void main() {
    mat4 it = pelican_load_items(uint(gl_InstanceIndex));
)glsl";
    text += kindLine(draw);
    text += R"glsl(    vec2 center = vec2(0.0);
    vec2 bound = vec2(0.0);
    switch (kind) {
)glsl";
    text += itemBoundsCases(draw);
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
    vCanvas = canvas;
    vItem = uint(gl_InstanceIndex);
}
)glsl";
    return text;
}

std::string paintLibrary(const DrawShape &draw);

std::string fragmentShader(const CanvasOutput &output, const std::string &raster, const DrawShape &draw) {
    std::string text = shader_header;
    text += generatedBanner(output, raster);
    text += "// canvasPaint gives an item's premultiplied colour in the image's blend space at a\n"
            "// canvas point: a shape from its signed distance d (canvas units, negative\n"
            "// inside; its screen-space gradient sets the antialiasing width), an image\n"
            "// from its texture. Masks multiply it; a clip layer's draw blends atop.\n";
    text += parameter_layout_comment;
    text += shader_includes;
    text += R"glsl(
layout(location = 0) in vec2 vCanvas;
layout(location = 1) flat in uint vItem;
layout(location = 0) out vec4 outColor;

)glsl";
    text += fitFunctions(output);
    text += "\n";
    text += paintLibrary(draw);
    text += "void main() {\n";
    text += "    mat4 it = pelican_load_items(vItem);\n";
    text += "    vec4 color = canvasPaint(it, vCanvas);\n";
    text += "    color *= canvasOpacity(it);\n";
    if (!draw.styles.empty()) text += "    color = canvasStyle(it, color);\n";
    text += R"glsl(    if (color.a <= 0.0) discard;
    outColor = color;
}
)glsl";
    return text;
}

// What paints an item at a point: the tables' reads, the distances, the
// images, the styles and canvasPaint (the draw's fragment shader and, CV-3a,
// a blur's passes that read its layer).
std::string paintLibrary(const DrawShape &draw) {
    std::string text = parameterFunctions(draw.frame);
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

vec3 canvasEncode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

bool canvasInside(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));
}

// The point in the item's own stack.
vec2 canvasItemPoint(mat4 it, vec2 p) {
    return inverse(mat2(it[2].xy, it[2].zw)) * (p - it[3].xy);
}

)glsl";
    // A shape's or a color's sRGB colour, in the image's blend space.
    text += std::string{"vec3 canvasShapeColor(vec3 c) { return "} +
            (draw.srgb ? "clamp(c, 0.0, 1.0)" : "canvasLinear(c)") + "; }\n\n";
    if (draw.slots > 0) {
        text += "// The images this draw reads, by slot; the port t<k> is slot k.\n";
        text += "vec4 canvasSample(uint slot, vec2 uv) {\n    switch (slot) {\n";
        for (std::size_t slot = 0; slot < draw.slots; ++slot)
            text += "    case " + std::to_string(slot) + "u: return pelican_sample_lod_t" + std::to_string(slot) +
                    "(uv, 0.0);\n";
        text += "    default: return vec4(0.0);\n    }\n}\n\n";
        text += R"glsl(// A texel as the image's blend space holds it, premultiplied.
vec4 canvasTexel(vec4 t, float alpha_mode, float encode) {
    float a = alpha_mode > 1.5 ? 1.0 : clamp(t.a, 0.0, 1.0);
    vec3 rgb = t.rgb;
    if (alpha_mode > 0.5 && alpha_mode < 1.5) rgb = a > 0.0 ? rgb / a : vec3(0.0);
    if (encode > 1.5) rgb = canvasLinear(rgb);
    else if (encode > 0.5) rgb = canvasEncode(rgb);
    return vec4(rgb * a, a);
}

)glsl";
    }
    if (draw.kinds.contains(canvas_kind_glyph_fill) || draw.kinds.contains(canvas_kind_glyph_stroke)) {
        text += "const float CANVAS_GLYPH_RANGE = " + glslNumber(canvas_glyph_range) + ";\n\n";
        text += R"glsl(// A glyph atlas texel (8-bit d / (2 range) + 0.5) to a distance in texels,
// positive outside: the median clamped to A +- 0.5 near the outline, A alone
// from 2 texels out (smoothstep between 1 and 2).
float canvasGlyphDistance(vec4 t) {
    vec4 d = (t - 0.5) * (2.0 * CANVAS_GLYPH_RANGE);
    float m = max(min(d.r, d.g), min(max(d.r, d.g), d.b));
    float near = clamp(m, d.a - 0.5, d.a + 0.5);
    float x = clamp(abs(d.a) - 1.0, 0.0, 1.0);
    return mix(near, d.a, x * x * (3.0 - 2.0 * x));
}

// A glyph's stroke, in texels: no wider than the stored distances reach,
// less the texel the edge's antialiasing takes.
float canvasGlyphStroke(float d, float width, float align) {
    float limit = (align > 0.5 ? 1.0 : 2.0) * (CANVAS_GLYPH_RANGE - 1.0);
    return canvasStroke(d, min(abs(width), limit), align);
}

)glsl";
    }
    // CV-3a: the shadows' and glows' arithmetic.
    if (hasEffectKinds(draw)) text += canvasEffectGlsl();
    text += styleFunctions(draw);
    if (draw.masks)
        text += R"glsl(float canvasMask(mat4 it, vec2 p) {
    float m = 1.0;
    if (it[1].z >= 0.0) {
        uint first = uint(it[1].z + 0.5);
        uint count = uint(it[1].w + 0.5);
        for (uint k = 0u; k < count; ++k) {
            uint r = first + k * 13u;
            mat2 a = mat2(pelican_load_bake(r + 7u), pelican_load_bake(r + 8u),
                          pelican_load_bake(r + 9u), pelican_load_bake(r + 10u));
            vec2 q = inverse(a) * (p - vec2(pelican_load_bake(r + 11u), pelican_load_bake(r + 12u)));
            vec2 uv = (q - vec2(pelican_load_bake(r + 3u), pelican_load_bake(r + 4u))) /
                      vec2(pelican_load_bake(r + 5u), pelican_load_bake(r + 6u));
            float value = pelican_load_bake(r + 2u);
            if (canvasInside(uv)) {
                vec4 t = canvasSample(uint(pelican_load_bake(r) + 0.5), uv);
                value = pelican_load_bake(r + 1u) > 0.5 ? dot(t.rgb, vec3(0.2126, 0.7152, 0.0722)) : t.a;
            }
            m *= value;
        }
    }
    return m;
}

)glsl";
    text += "vec4 canvasPaint(mat4 it, vec2 p) {\n";
    text += kindLine(draw);
    text += R"glsl(    vec2 q = canvasItemPoint(it, p);
    vec4 color = vec4(0.0);
    switch (kind) {
)glsl";
    for (const auto kind : draw.kinds) {
        const auto param = [&](std::size_t index) { return parameterRead(draw, kind, index); };
        // CV-3a: an effect kind paints its source's distance, moved by the
        // offset, as the effect's coverage in the effect's colour.
        const auto own = effectSourceKind(kind);
        const bool effect = own != kind;
        const auto e = [&](std::size_t index) { return param(kindParameterCount(own) + index); };
        const std::string point = effect ? "at" : "q";
        const auto moved = [&] {
            return effect ? "        vec2 at = q - vec2(" + e(0) + ", " + e(1) + ");\n" : std::string{};
        };
        const auto effectColor = [&](const std::string &d, const std::string &aa) {
            return "        float a = canvasEffectCoverage(" + d + ", " + aa + ", clamp(" + e(2) + ", 0.0, " + e(3) +
                   "), " + e(4) + ") * clamp(" + e(8) + ", 0.0, 1.0);\n" +
                   "        color = vec4(canvasShapeColor(vec3(" + e(5) + ", " + e(6) + ", " + e(7) + ")) * a, a);\n";
        };
        text += "    case " + std::to_string(kind) + "u: { // " + std::string{canvasItemKindName(kind)} + "\n";
        if (kind == canvas_kind_image) {
            text += "        vec2 uv = (q - vec2(" + param(0) + ", " + param(1) + ")) / vec2(" + param(2) + ", " +
                    param(3) + ");\n";
            text += "        if (canvasInside(uv))\n";
            text += "            color = canvasTexel(canvasSample(uint(it[3].z + 0.5), uv), " + param(4) + ", " +
                    param(5) + ");\n";
            text += "        break;\n    }\n";
            continue;
        }
        if (kind == canvas_kind_effect_image) {
            // The blurred layer's alpha, spread, in the effect's colour.
            text += "        vec2 uv = (q - vec2(" + param(0) + ", " + param(1) + ")) / vec2(" + param(2) + ", " +
                    param(3) + ");\n";
            text += "        if (canvasInside(uv)) {\n";
            text += "            float a = canvasSpreadAlpha(canvasSample(uint(it[3].z + 0.5), uv).a, clamp(" + param(4) +
                    ", 0.0, " + param(5) + "), " + param(6) + ") * clamp(" + param(10) + ", 0.0, 1.0);\n";
            text += "            color = vec4(canvasShapeColor(vec3(" + param(7) + ", " + param(8) + ", " + param(9) +
                    ")) * a, a);\n";
            text += "        }\n        break;\n    }\n";
            continue;
        }
        if (own == canvas_kind_glyph_fill || own == canvas_kind_glyph_stroke) {
            // The texel the point falls on in the glyph's cell, kept a half
            // texel inside it so the filter never reads the next cell.
            text += "        float s = " + param(2) + ";\n";
            text += "        vec2 cell_size = s * vec2(" + param(5) + ", " + param(6) + ");\n";
            text += "        float n = " + param(11) + ";\n";
            text += moved();
            text += "        vec2 tex = (" + point + " - vec2(" + param(0) + ", " + param(1) + ") - s * vec2(" +
                    param(3) + ", " + param(4) + ")) / cell_size * n;\n";
            text += "        vec2 uv = vec2(" + param(7) + ", " + param(8) + ") + clamp(tex, vec2(0.5), vec2(n - 0.5)) / n * vec2(" +
                    param(9) + ", " + param(10) + ");\n";
            text += "        float d = canvasGlyphDistance(canvasSample(uint(it[3].z + 0.5), uv));\n";
            if (own == canvas_kind_glyph_stroke)
                text += "        d = canvasGlyphStroke(d, " + param(canvas_glyph_parameter_count + 4) + " * n / abs(cell_size.x), " +
                        param(canvas_glyph_parameter_count + 5) + ");\n";
            // Texels per pixel, from the screen-space change of the texel.
            text += "        float aa = max(length(vec2(length(dFdx(tex)), length(dFdy(tex)))) * 0.70710678, 1e-6);\n";
            if (effect) {
                // Past its cell the atlas holds nothing: the way out to the
                // cell adds to the distance there. In stack units.
                text += "        d += length(tex - clamp(tex, vec2(0.5), vec2(n - 0.5)));\n";
                text += "        float unit = abs(cell_size.x) / n;\n";
                text += effectColor("d * unit", "aa * unit");
                text += "        break;\n    }\n";
                continue;
            }
            text += "        float a = clamp(" + param(canvas_glyph_parameter_count + 3) +
                    ", 0.0, 1.0) * clamp(0.5 - d / aa, 0.0, 1.0);\n";
            text += "        color = vec4(canvasShapeColor(vec3(" + param(canvas_glyph_parameter_count) + ", " +
                    param(canvas_glyph_parameter_count + 1) + ", " + param(canvas_glyph_parameter_count + 2) +
                    ")) * a, a);\n";
            text += "        break;\n    }\n";
            continue;
        }
        if (kind == canvas_kind_color) {
            text += "        if (canvasInside(q / CANVAS_SIZE)) {\n";
            text += "            float a = clamp(" + param(3) + ", 0.0, 1.0);\n";
            text += "            color = vec4(canvasShapeColor(vec3(" + param(0) + ", " + param(1) + ", " + param(2) +
                    ")) * a, a);\n";
            text += "        }\n        break;\n    }\n";
            continue;
        }
        if (kind == canvas_kind_backdrop) {
            // Its colour, opaque: drawn atop, the alpha below stays.
            text += "        if (canvasInside(q / CANVAS_SIZE))\n";
            text += "            color = vec4(canvasStraight(canvasBackdrop()), 1.0);\n";
            text += "        break;\n    }\n";
            continue;
        }
        if (kind == canvas_kind_group) {
            text += "        vec2 scale, offset;\n        canvasFit(scale, offset);\n";
            text += "        color = canvasTexel(canvasSample(uint(it[3].z + 0.5), (offset + q * scale) / "
                    "canvasTargetSize()), 1.0, " + param(0) + ");\n";
            text += "        break;\n    }\n";
            continue;
        }
        const auto shape = own < 2 ? CanvasShape::rect : CanvasShape::ellipse;
        const bool stroke = own % 2 == 1;
        const auto rotation = shape == CanvasShape::rect ? 5 : 4;
        text += "        vec2 half_size = 0.5 * abs(vec2(" + param(0) + ", " + param(1) + "));\n";
        text += moved();
        text += "        vec2 local = canvasRotate(" + point + " - vec2(" + param(2) + ", " + param(3) + "), -radians(" +
                param(rotation) + "));\n";
        if (shape == CanvasShape::rect)
            text += "        float d = canvasRoundRect(local, half_size, clamp(" + param(4) +
                    ", 0.0, min(half_size.x, half_size.y)));\n";
        else
            text += "        float d = canvasEllipse(local, half_size);\n";
        if (stroke)
            text += "        d = canvasStroke(d, " + param(parameterIndex(shape, 4)) + ", " +
                    param(parameterIndex(shape, 5)) + ");\n";
        text += "        float aa = max(length(vec2(dFdx(d), dFdy(d))), 1e-6);\n";
        if (effect) {
            text += effectColor("d", "aa");
            text += "        break;\n    }\n";
            continue;
        }
        text += "        float a = clamp(" + param(parameterIndex(shape, 3)) + ", 0.0, 1.0) * clamp(0.5 - d / aa, 0.0, 1.0);\n";
        text += "        color = vec4(canvasShapeColor(vec3(" + param(parameterIndex(shape, 0)) + ", " +
                param(parameterIndex(shape, 1)) + ", " + param(parameterIndex(shape, 2)) + ")) * a, a);\n";
        text += "        break;\n    }\n";
    }
    text += "    default:\n        break;\n    }\n";
    if (draw.masks) text += "    color *= canvasMask(it, p);\n";
    text += "    return color;\n}\n\n";
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
    Json diagnostics = Json::array();
    // CV-5: the parameters the ship policy made literals.
    std::size_t folded_constants = 0;
};

// The images an item samples, in the order a slot is given: its own, then its masks'.
void itemTextures(const CanvasItem &item, std::vector<CanvasTexture> &into) {
    const auto add = [&](const CanvasTexture &texture) {
        if (std::find(into.begin(), into.end(), texture) == into.end()) into.push_back(texture);
    };
    if (item.source == CanvasItemSource::image || item.source == CanvasItemSource::group ||
        item.source == CanvasItemSource::glyph)
        add(item.texture);
    for (const auto &mask : item.masks) add(mask.texture);
}

struct CanvasDraw {
    std::vector<std::size_t> items;
    std::vector<CanvasTexture> slots;
    // Clip layers, drawn atop what the draws before drew (their base).
    bool atop = false;
    // CV-2b: the draw's first item reads its backdrop from copy k of the
    // image (`backdrop_prefix` back<k>), one of the draw's slots.
    std::optional<std::size_t> backdrop;
};

// Items in order, cut where clip layers start or end, and where the next item
// would take the draw past canvas_draw_image_limit images (`splits` counts
// those cuts). CV-2b: cut before each item that reads its backdrop, which
// then reads a copy of the image as the draws before it left it (the first
// item of the list has none to read: the lowering lets it blend normal).
std::vector<CanvasDraw> planDraws(const std::vector<CanvasItem> &items, const std::string &output,
                                  std::size_t &splits, const std::string &backdrop_prefix = {}) {
    std::vector<CanvasDraw> draws(1);
    splits = 0;
    std::size_t copies = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        std::vector<CanvasTexture> textures;
        const bool reads = index > 0 && canvasItemReadsBackdrop(items[index]);
        if (reads)
            textures.push_back({CanvasTexture::Origin::graph, {}, false, backdrop_prefix + "back" + std::to_string(copies),
                                "image", 0, true});
        itemTextures(items[index], textures);
        if (textures.size() > canvas_draw_image_limit)
            throw GraphRegistryError("canvas.draw_images_exceeded@1", {{"output", output},
                                                                        {"count", textures.size()},
                                                                        {"limit", canvas_draw_image_limit}});
        const bool atop = items[index].clip_base.has_value() || items[index].source == CanvasItemSource::backdrop;
        auto merged = draws.back().slots;
        for (const auto &texture : textures)
            if (std::find(merged.begin(), merged.end(), texture) == merged.end()) merged.push_back(texture);
        const bool full = merged.size() > canvas_draw_image_limit;
        if (!draws.back().items.empty() && (full || reads || atop != draws.back().atop)) {
            if (full && !reads) ++splits;
            draws.emplace_back();
            merged = textures;
        }
        if (reads) draws.back().backdrop = copies++;
        draws.back().atop = atop;
        draws.back().slots = std::move(merged);
        draws.back().items.push_back(index);
    }
    return draws;
}

// An image of a fixed size. CV-3a: with `extent_scale` 1 beside the width
// and height -- the engine's render target requires one, and draws at the
// fixed size when it has both.
Json fixedImageParameters(const std::string &format, std::uint32_t width, std::uint32_t height) {
    return {{"type", colorType()},
            {"format", format},
            {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
            {"width", width},
            {"height", height},
            {"extent_scale", 1.0}};
}

Json canvasImageParameters(const CanvasOutput &output) {
    if (output.image_size) return fixedImageParameters(output.format, (*output.image_size)[0], (*output.image_size)[1]);
    return {{"type", colorType()},
            {"format", output.format},
            {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
            {"extent_scale", output.extent_scale}};
}

// One item's share of a draw's tables. The bake table gets the parameters
// `kept` keeps (all of them but under ship) and then its filters' numbers;
// when an edge drives any, the sources follow, one per number in the same
// order (0: the bake table, k: frame table entry k - 1); then the frame
// entries of the opacity edges, then 13 numbers per mask. Returns the item's
// row of the item table (kind, offsets, the map, the image slot, `clip`).
template <typename FrameEntry, typename SlotOf, typename Kept>
std::array<double, 16> itemRecord(const CanvasItem &item, std::uint32_t kind, std::vector<double> &bake,
                                  FrameEntry frame_entry, SlotOf slot_of, Kept kept, double clip, bool &masks) {
    const auto bake_offset = static_cast<double>(bake.size());
    for (std::size_t at = 0; at < item.parameters.size(); ++at)
        if (kept(at)) bake.push_back(item.parameters[at].value);
    for (const auto &filter : item.filters) bake.insert(bake.end(), filter.constants.begin(), filter.constants.end());
    const auto filter_edge = [](const CanvasFilter &filter) {
        return std::any_of(filter.edges.begin(), filter.edges.end(),
                           [](const std::optional<CanvasValueSource> &edge) { return edge.has_value(); });
    };
    double source_offset = -1.0;
    if (std::any_of(item.parameters.begin(), item.parameters.end(),
                    [](const CanvasParameter &parameter) { return parameter.edge.has_value(); }) ||
        std::any_of(item.filters.begin(), item.filters.end(), filter_edge)) {
        source_offset = static_cast<double>(bake.size());
        for (std::size_t at = 0; at < item.parameters.size(); ++at) {
            if (!kept(at)) continue;
            const auto &parameter = item.parameters[at];
            bake.push_back(parameter.edge ? static_cast<double>(frame_entry(*parameter.edge, parameter.value) + 1) : 0.0);
        }
        // CV-3a: a filter's numbers a value edge drives.
        for (const auto &filter : item.filters)
            for (std::size_t at = 0; at < filter.constants.size(); ++at) {
                const bool driven = at < filter.edges.size() && filter.edges[at].has_value();
                bake.push_back(driven ? static_cast<double>(frame_entry(*filter.edges[at], filter.constants[at]) + 1)
                                      : 0.0);
            }
    }
    double opacity_offset = -1.0;
    if (!item.opacity_edges.empty()) {
        opacity_offset = static_cast<double>(bake.size());
        for (const auto &edge : item.opacity_edges) bake.push_back(static_cast<double>(frame_entry(edge, 1.0)));
    }
    double mask_offset = -1.0;
    if (!item.masks.empty()) {
        masks = true;
        mask_offset = static_cast<double>(bake.size());
        for (const auto &mask : item.masks) {
            bake.push_back(slot_of(mask.texture));
            bake.push_back(mask.luminance ? 1.0 : 0.0);
            bake.push_back(mask.outside);
            bake.insert(bake.end(), mask.rect.begin(), mask.rect.end());
            bake.insert(bake.end(), mask.affine.begin(), mask.affine.end());
        }
    }
    const bool sampled = item.source == CanvasItemSource::image || item.source == CanvasItemSource::group ||
                         item.source == CanvasItemSource::glyph;
    return {static_cast<double>(kind), bake_offset, source_offset, item.opacity,
            opacity_offset, static_cast<double>(item.opacity_edges.size()), mask_offset,
            static_cast<double>(item.masks.size()),
            item.affine[0], item.affine[1], item.affine[2], item.affine[3],
            item.affine[4], item.affine[5], sampled ? slot_of(item.texture) : -1.0, clip};
}

// CV-2b: the item's style -- its blend and its filters -- numbered by first
// use among the draw's, on its kind (kind + 256 * style); the kind as it is
// for neither.
std::uint32_t styledKind(const CanvasItem &item, DrawShape &shape) {
    auto kind = canvasItemKind(item);
    if (item.blend == CanvasBlend::normal && item.filters.empty()) return kind;
    std::pair<CanvasBlend, std::vector<CanvasFilterKind>> style{item.blend, {}};
    for (const auto &filter : item.filters) style.second.push_back(filter.kind);
    auto found = std::find(shape.styles.begin(), shape.styles.end(), style);
    if (found == shape.styles.end()) found = shape.styles.insert(shape.styles.end(), std::move(style));
    return kind + canvas_kind_style_stride * static_cast<std::uint32_t>(found - shape.styles.begin() + 1);
}

// The trace of one instance: which authored nodes drew it, and how.
Json itemTrace(const CanvasItem &item, const std::string &output, std::size_t instance, std::uint32_t kind) {
    auto nodes = Json::array();
    for (const auto &node : item.nodes) nodes.push_back(node);
    Json entry{{"output", output}, {"instance", instance},
               {"kind", std::string{canvasItemKindName(kind % canvas_kind_style_stride)}}, {"nodes", std::move(nodes)}};
    if (item.blend != CanvasBlend::normal) entry["blend"] = std::string{canvasBlendName(item.blend)};
    if (!item.filters.empty()) {
        entry["filters"] = Json::array();
        for (const auto &filter : item.filters)
            entry["filters"].push_back({{"node", filter.node}, {"op", std::string{canvasFilterOperation(filter.kind)}}});
    }
    // CV-3a: a shadow's or a glow's filter.
    if (item.effect) entry["effect"] = {{"node", item.effect->node}, {"op", item.effect->op}};
    return entry;
}

// CV-2b: `<copy>` writes what the draw `previous` left in its image into
// `<copy>_image`, texel for texel: one triangle over the image.
void lowerBackdropCopy(const CanvasOutput &output, const std::string &copy, const std::string &previous,
                       const CanvasLoweringOptions &options, LoweredOutput &lowered) {
    const auto &id = output.id;
    std::string vertex = shader_header;
    vertex += generatedBanner(output, copy);
    vertex += "// The backdrop copy: one triangle over the whole image.\n";
    vertex += shader_includes;
    vertex += R"glsl(
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";
    std::string fragment = shader_header;
    fragment += generatedBanner(output, copy);
    fragment += "// The backdrop copy: each pixel of the image as the draw before left it.\n";
    fragment += shader_includes;
    fragment += "\nlayout(location = 0) out vec4 outColor;\n\n";
    fragment += fitFunctions(output);
    fragment += R"glsl(
void main() {
    outColor = pelican_sample_lod_t0(gl_FragCoord.xy / canvasTargetSize(), 0.0);
}
)glsl";
    const auto digest = picosha2::hash256_hex_string(vertex + std::string(1, '\0') + fragment);
    const auto stem = "canvas_" + fileStem(copy) + "_" + digest.substr(0, 8);
    const auto directory = options.shader_directory + (options.directory_per_stem ? "/" + stem : std::string{});
    lowered.sources.push_back({stem, "vert", vertex, {}, id, directory + "/" + stem + ".vert"});
    lowered.sources.push_back({stem, "frag", fragment, {}, id, directory + "/" + stem + ".frag"});
    const auto image = copy + "_image";
    lowered.nodes.push_back({{"id", image}, {"op", "render.image"}, {"parameters", canvasImageParameters(output)},
                             {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
    const auto shader = "project://" + directory + "/" + stem;
    Json ports{{"t0", {{"kind", "image"}, {"type", colorType()}, {"access", "read"},
                       {"sampling", {{"filter", "nearest"}, {"address", "clamp_to_edge"}}}}},
               {"image", {{"kind", "image"}, {"type", colorType()}, {"access", "write"}, {"attachment", "color"},
                          {"location", 0}}}};
    Json parameters{{"shader", {{"vertex", shader}, {"fragment", shader}}},
                    {"draw", {{"vertex_count", 3}}},
                    {"raster_state",
                     {{"topology", "triangle_list"}, {"cull", "none"}, {"depth_test", false}, {"depth_write", false},
                      {"color_attachments", Json::array({{{"write_mask", "rgba"}}})}}},
                    {"ports", std::move(ports)},
                    {"clear_color", Json::array({0.0, 0.0, 0.0, 0.0})}};
    lowered.nodes.push_back({{"id", copy},
                             {"op", "render.raster"},
                             {"parameters", std::move(parameters)},
                             {"inputs", {{"t0", endpoint(previous, "image")}, {"image", endpoint(image, "resource")}}},
                             {"outputs", Json::array({"image"})}});
}

// One image of an output (the output's own, or a group's): its render.image
// and the draws that fill it, the last of which carries `raster`.
void lowerComposite(const CanvasOutput &output, const std::vector<CanvasItem> &authored, bool srgb,
                    std::optional<std::size_t> group, const std::function<Json(const CanvasTexture &)> &texture_source,
                    const CanvasLoweringOptions &options, LoweredOutput &lowered) {
    const auto &id = output.id;
    const auto prefix = group ? reservedPrefix(id) + "g" + std::to_string(*group) + "_" : reservedPrefix(id);
    const auto raster = group ? canvasGroupRasterNodeId(id, *group) : id;
    const auto image = group ? canvasGroupImageNodeId(id, *group) : canvasImageNodeId(id);
    auto items = authored;
    // CV-2b: the first item has nothing below it to read. A blend there is
    // normal; a backdrop draws nothing (and goes, unless clip layers sit on it).
    if (!items.empty() && canvasItemReadsBackdrop(items.front())) {
        auto &first = items.front();
        first.blend = CanvasBlend::normal;
        if (first.source == CanvasItemSource::backdrop) {
            const bool based = std::any_of(items.begin(), items.end(),
                                           [](const CanvasItem &item) { return item.clip_base == std::size_t{0}; });
            if (based) {
                first.source = CanvasItemSource::shape;
                first.parameters.assign(canvasShapeParameterCount(CanvasShape::rect) +
                                            canvasPaintParameterCount(CanvasPaint::fill), CanvasParameter{});
                first.opacity = 0.0;
                first.opacity_edges.clear();
                first.masks.clear();
                first.filters.clear();
            } else {
                items.erase(items.begin());
                for (auto &item : items)
                    if (item.clip_base) --*item.clip_base;
            }
        }
    }
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
    std::size_t splits = 0;
    const auto draws = planDraws(items, id, splits, prefix);
    if (const auto copies = std::count_if(draws.begin(), draws.end(),
                                          [](const CanvasDraw &draw) { return draw.backdrop.has_value(); });
        copies > 0) {
        Json diagnostic{{"code", "canvas.backdrop_copies"}, {"output", id}, {"count", copies}};
        if (group) diagnostic["group"] = *group;
        lowered.diagnostics.push_back(std::move(diagnostic));
    }
    if (splits > 0) {
        Json diagnostic{{"code", "canvas.draw_split"}, {"output", id}, {"reason", "image_limit"},
                        {"count", splits + 1}};
        if (group) diagnostic["group"] = *group;
        lowered.diagnostics.push_back(std::move(diagnostic));
    }
    lowered.nodes.push_back({{"id", image}, {"op", "render.image"}, {"parameters", canvasImageParameters(output)},
                             {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
    std::string previous;
    for (std::size_t draw_index = 0; draw_index < draws.size(); ++draw_index) {
        const auto &draw = draws[draw_index];
        const bool last = draw_index + 1 == draws.size();
        const auto tables = last ? prefix : prefix + "draw" + std::to_string(draw_index) + "_";
        const auto name = last ? raster : prefix + "draw" + std::to_string(draw_index);
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
        const auto slotOf = [&](const CanvasTexture &texture) {
            return static_cast<double>(std::find(draw.slots.begin(), draw.slots.end(), texture) - draw.slots.begin());
        };
        std::map<std::size_t, std::size_t> local;
        DrawShape shape;
        shape.srgb = srgb;
        shape.slots = draw.slots.size();
        shape.atop = draw.atop;
        if (draw.backdrop) {
            // CV-2b: the copy the draw reads its backdrop from: a raster
            // that writes the image, as the draw before left it, into one of
            // its own (a pass cannot read the image it draws into).
            const auto copy = prefix + "back" + std::to_string(*draw.backdrop);
            shape.backdrop_slot = static_cast<std::size_t>(
                slotOf(CanvasTexture{CanvasTexture::Origin::graph, {}, false, copy, "image", 0, true}));
            lowerBackdropCopy(output, copy, previous, options, lowered);
        }
        if (options.policy == CanvasLoweringPolicy::ship) {
            // CV-5: a parameter no edge drives, the same number in every item
            // of its kind, becomes a literal of the shader.
            for (const auto index : draw.items) {
                const auto &item = items[index];
                auto [found, first] = shape.constants.try_emplace(canvasItemKind(item));
                auto &constants = found->second;
                if (first) constants.resize(item.parameters.size());
                if (constants.size() != item.parameters.size())
                    throw std::logic_error("canvas lowering: items of one kind differ in their parameters");
                for (std::size_t at = 0; at < constants.size(); ++at) {
                    const auto &parameter = item.parameters[at];
                    if (first && !parameter.edge) constants[at] = parameter.value;
                    else if (parameter.edge || (constants[at] && *constants[at] != parameter.value))
                        constants[at].reset();
                }
            }
            for (const auto &[kind, constants] : shape.constants)
                lowered.folded_constants += static_cast<std::size_t>(
                    std::count_if(constants.begin(), constants.end(),
                                  [](const std::optional<double> &value) { return value.has_value(); }));
        }
        for (const auto index : draw.items) {
            const auto instance = local.size();
            local.emplace(index, instance);
            const auto &item = items[index];
            const auto own = canvasItemKind(item);
            shape.kinds.insert(own);
            // CV-5: the parameters the tables keep (all of them but under ship).
            const auto folded = shape.constants.find(own);
            const auto kept = [&](std::size_t at) {
                return folded == shape.constants.end() || !folded->second[at].has_value();
            };
            const auto kind = styledKind(item, shape);
            const double clip = item.clip_base ? static_cast<double>(*item.clip_base) : -1.0;
            const auto row = itemRecord(item, kind, bake, frameEntry, slotOf, kept, clip, shape.masks);
            item_table.insert(item_table.end(), row.begin(), row.end());
            if (!empty) {
                auto entry = itemTrace(item, id, instance, kind);
                if (group) entry["group"] = *group;
                if (draws.size() > 1) entry["draw"] = draw_index;
                lowered.items.push_back(std::move(entry));
            }
        }
        if (bake.empty()) bake.push_back(0.0);
        shape.frame = !frame_sources.empty();

        const auto vertex = vertexShader(output, name, shape);
        const auto fragment = fragmentShader(output, name, shape);
        const auto digest = picosha2::hash256_hex_string(vertex + std::string(1, '\0') + fragment);
        const auto stem = "canvas_" + fileStem(name) + "_" + digest.substr(0, 8);
        const auto directory = options.shader_directory + (options.directory_per_stem ? "/" + stem : std::string{});
        lowered.sources.push_back({stem, "vert", vertex, {}, id, directory + "/" + stem + ".vert"});
        lowered.sources.push_back({stem, "frag", fragment, {}, id, directory + "/" + stem + ".frag"});

        const auto items_id = tables + "items";
        const auto bake_id = tables + "bake";
        const auto frame_id = tables + "frame";
        lowered.nodes.push_back({{"id", items_id}, {"op", "render.attribute"},
                                 {"parameters", {{"element", "mat4"}, {"domain", "instance"}, {"count", draw.items.size()},
                                                 {"host_source", "runtime_f32"}, {"initial_f32", floatArray(item_table)}}},
                                 {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
        lowered.nodes.push_back({{"id", bake_id}, {"op", "render.buffer"},
                                 {"parameters", {{"element", "float"}, {"size", bake.size() * sizeof(float)},
                                                 {"host_source", "runtime_f32"}, {"initial_f32", floatArray(bake)}}},
                                 {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
        if (shape.frame) {
            lowered.nodes.push_back({{"id", frame_id}, {"op", "render.buffer"},
                                     {"parameters", {{"element", "float"}, {"size", frame_initial.size() * sizeof(float)},
                                                     {"host_source", "runtime_f32"},
                                                     {"initial_f32", floatArray(frame_initial)}}},
                                     {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
            // One float per value edge, tiling the frame table from byte 0 (RS-15).
            for (std::size_t index = 0; index < frame_sources.size(); ++index)
                lowered.nodes.push_back({{"id", tables + "set_" + std::to_string(index)}, {"op", "render.scalar"},
                                         {"parameters", {{"target", frame_id}, {"offset", index * sizeof(float)}}},
                                         {"inputs", {{"value", endpoint(frame_sources[index].node,
                                                                        frame_sources[index].port)}}},
                                         {"outputs", Json::array()}});
        }
        Json ports{{"items", {{"kind", "buffer"}, {"element", "mat4"}, {"domain", "instance"}, {"access", "read"}}},
                   {"bake", {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}}},
                   {"image", {{"kind", "image"}, {"type", colorType()}, {"access", "write"}, {"attachment", "color"},
                              {"location", 0}}}};
        // A split draw adds to what the one before it drew.
        Json inputs{{"items", endpoint(items_id, "resource")},
                    {"bake", endpoint(bake_id, "resource")},
                    {"image", draw_index == 0 ? endpoint(image, "resource") : endpoint(previous, "image")}};
        if (shape.frame) {
            ports["frame"] = {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}};
            inputs["frame"] = endpoint(frame_id, "resource");
        }
        for (std::size_t slot = 0; slot < draw.slots.size(); ++slot) {
            const auto port = "t" + std::to_string(slot);
            ports[port] = {{"kind", "image"}, {"type", colorType()}, {"access", "read"},
                           {"sampling", {{"filter", draw.slots[slot].nearest ? "nearest" : "linear"},
                                         {"address", "clamp_to_edge"}}}};
            inputs[port] = texture_source(draw.slots[slot]);
        }
        // Premultiplied over: the image is premultiplied, so is every item.
        // Clip layers go atop: weighted by the alpha below, which they keep.
        const Json premultiplied =
            draw.atop ? Json{{"color", {{"src", "dst_alpha"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}}},
                             {"alpha", {{"src", "zero"}, {"dst", "one"}, {"op", "add"}}}}
                      : Json{{"color", {{"src", "one"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}}},
                             {"alpha", {{"src", "one"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}}}};
        const auto shader = "project://" + directory + "/" + stem;
        Json parameters{{"shader", {{"vertex", shader}, {"fragment", shader}}},
                        {"draw", {{"vertex_count", 6}, {"instance_source", "items"}}},
                        {"raster_state",
                         {{"topology", "triangle_list"}, {"cull", "none"}, {"depth_test", false}, {"depth_write", false},
                          {"color_attachments", Json::array({{{"blend", premultiplied}, {"write_mask", "rgba"}}})}}},
                        {"ports", std::move(ports)}};
        if (draw_index == 0) parameters["clear_color"] = Json::array({0.0, 0.0, 0.0, 0.0});
        else parameters["color_load_op"] = "load";
        lowered.nodes.push_back({{"id", name},
                                 {"op", "render.raster"},
                                 {"parameters", std::move(parameters)},
                                 {"inputs", std::move(inputs)},
                                 {"outputs", Json::array({"image"})}});
        previous = name;
    }
}

// --- CV-6a: canvas.place, the stack's items drawn in 3D ---------------------

// What a canvas.place's vertex shader reads past an ordinary draw's.
struct PlaceDraw {
    // Per kind, the item parameter its depth starts at: the constant part,
    // then the edge-driven one.
    std::map<std::uint32_t, std::size_t> depth_index;
    // The placement's seven numbers in the bake table (position.xyz,
    // rotation.xyz, scale), and the sources of the ones an edge drives.
    std::size_t place_offset = 0;
    std::optional<std::size_t> place_source;
};

Json depthType() {
    return {{"semantic", "pelican.render.depth@1"}, {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
}

std::string placeVertexShader(const CanvasOutput &output, const std::string &raster, const DrawShape &draw,
                              const PlaceDraw &place) {
    const auto &placement = *output.placement;
    std::string text = shader_header;
    text += generatedBanner(output, raster);
    text += "// One instance draws one item: 6 vertices, the item's bounding quad, no vertex buffer,\n"
            "// placed in the world by canvas.place and seen by the frame's camera.\n";
    text += parameter_layout_comment;
    text += "// After an item's own parameters, its depth: a constant and an edge-driven part.\n"
            "// The placement: position.xyz rotation.xyz (degrees) scale, at PLACE_OFFSET.\n";
    text += shader_includes;
    text += R"glsl(
layout(location = 0) out vec2 vCanvas;
layout(location = 1) flat out uint vItem;

)glsl";
    text += "const vec2 CANVAS_SIZE = vec2(" + glslNumber(output.size[0]) + ", " + glslNumber(output.size[1]) + ");\n";
    text += "// The anchor in stack units, and stack units per world unit.\n";
    text += "const vec2 PLACE_ANCHOR = vec2(" + glslNumber(placement.anchor[0] * output.size[0]) + ", " +
            glslNumber(placement.anchor[1] * output.size[1]) + ");\n";
    text += "const float PLACE_UNITS = " + glslNumber(placement.pixels_per_unit) + ";\n";
    text += "const uint PLACE_OFFSET = " + std::to_string(place.place_offset) + "u;\n\n";
    text += parameterFunctions(draw.frame);
    text += "\nfloat canvasPlaceParam(uint i) {\n";
    if (draw.frame && place.place_source)
        text += "    uint source = uint(pelican_load_bake(" + std::to_string(*place.place_source) +
                "u + i) + 0.5);\n    if (source != 0u) return pelican_load_frame(source - 1u);\n";
    text += "    return pelican_load_bake(PLACE_OFFSET + i);\n}\n\n";
    text += "float canvasItemDepth(mat4 it, uint kind) {\n    switch (kind) {\n";
    for (const auto &[kind, index] : place.depth_index)
        text += "    case " + std::to_string(kind) + "u: return canvasParam(it, " + std::to_string(index) +
                "u) + canvasParam(it, " + std::to_string(index + 1) + "u);\n";
    text += "    default: return 0.0;\n    }\n}\n";
    text += R"glsl(
float canvasStrokeOutset(float width, float align) {
    return align > 1.5 ? abs(width) : (align > 0.5 ? 0.0 : 0.5 * abs(width));
}

// Degrees, X first, then Y, then Z: Rz Ry Rx.
mat3 canvasPlaceRotation(vec3 degrees) {
    vec3 r = radians(degrees);
    float cx = cos(r.x), sx = sin(r.x), cy = cos(r.y), sy = sin(r.y), cz = cos(r.z), sz = sin(r.z);
    mat3 rx = mat3(1.0, 0.0, 0.0, 0.0, cx, sx, 0.0, -sx, cx);
    mat3 ry = mat3(cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy);
    mat3 rz = mat3(cz, sz, 0.0, -sz, cz, 0.0, 0.0, 0.0, 1.0);
    return rz * ry * rx;
}

// A point of the stack (+y down) at a depth (toward the viewer) in the world.
vec3 canvasPlaceWorld(vec2 canvas, float depth, vec3 position, mat3 rotation, float scale) {
    vec3 local = vec3(canvas.x - PLACE_ANCHOR.x, PLACE_ANCHOR.y - canvas.y, depth) / PLACE_UNITS;
    return position + rotation * (scale * local);
}

void main() {
    mat4 it = pelican_load_items(uint(gl_InstanceIndex));
)glsl";
    // CV-2b: past 256 the kind carries a style (a filtered item's).
    text += kindLine(draw);
    text += R"glsl(    vec2 center = vec2(0.0);
    vec2 bound = vec2(0.0);
    switch (kind) {
)glsl";
    text += itemBoundsCases(draw);
    text += R"glsl(    default:
        break;
    }
    vec3 position = vec3(canvasPlaceParam(0u), canvasPlaceParam(1u), canvasPlaceParam(2u));
    mat3 rotation = canvasPlaceRotation(vec3(canvasPlaceParam(3u), canvasPlaceParam(4u), canvasPlaceParam(5u)));
    float scale = canvasPlaceParam(6u);
    float depth = canvasItemDepth(it, kind);
    mat2 affine = mat2(it[2].xy, it[2].zw);
    mat4 view_projection = pelicanFrame.projection * pelicanFrame.view;
    // Two pixels of room for the antialiased edge, in stack units: the
    // pixels one stack unit covers at the item's centre, the shorter way.
    vec2 middle = affine * center + it[3].xy;
    vec4 c0 = view_projection * vec4(canvasPlaceWorld(middle, depth, position, rotation, scale), 1.0);
    vec4 cx = view_projection * vec4(canvasPlaceWorld(middle + vec2(1.0, 0.0), depth, position, rotation, scale), 1.0);
    vec4 cy = view_projection * vec4(canvasPlaceWorld(middle + vec2(0.0, 1.0), depth, position, rotation, scale), 1.0);
    float margin = 0.0;
    if (c0.w > 0.0 && cx.w > 0.0 && cy.w > 0.0) {
        vec2 half_resolution = 0.5 * pelicanFrame.resolution.xy;
        vec2 p0 = c0.xy / c0.w * half_resolution;
        float pixels = min(length(cx.xy / cx.w * half_resolution - p0), length(cy.xy / cy.w * half_resolution - p0));
        margin = min(2.0 / max(pixels, 1e-4), max(CANVAS_SIZE.x, CANVAS_SIZE.y));
    }
    const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 local = corners[gl_VertexIndex] * (bound + margin);
    vec2 canvas = affine * (center + local) + it[3].xy;
    gl_Position = view_projection * vec4(canvasPlaceWorld(canvas, depth, position, rotation, scale), 1.0);
)glsl";
    if (!placement.double_sided)
        text += R"glsl(    // One side only: the one the stack's normal points to (a collapsed quad otherwise).
    vec3 normal = mat3(pelicanFrame.view) * (rotation * vec3(0.0, 0.0, scale));
    vec3 eye = (pelicanFrame.view * vec4(canvasPlaceWorld(middle, depth, position, rotation, scale), 1.0)).xyz;
    bool facing = pelicanFrame.projection[3][3] > 0.5 ? normal.z > 0.0 : dot(normal, eye) < 0.0;
    if (!facing) gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
)glsl";
    text += R"glsl(    vCanvas = canvas;
    vItem = uint(gl_InstanceIndex);
}
)glsl";
    return text;
}

// A canvas.place's items: one draw per CV-2a split onto the colour image it
// reads (depth-tested against its depth image), then with depth_write the
// same draws again writing only the depth of the pixels at least half
// covered. The last raster keeps the place's id; its `color` is the result.
void lowerPlace(const CanvasOutput &output, const std::function<Json(const CanvasTexture &)> &texture_source,
                const CanvasLoweringOptions &options, LoweredOutput &lowered) {
    const auto &id = output.id;
    const auto &placement = *output.placement;
    const auto prefix = reservedPrefix(id);
    auto items = output.items;
    // An empty stack still draws one item (fully transparent), as an output does.
    const bool empty = items.empty();
    if (empty) {
        CanvasItem none;
        none.parameters.assign(canvasShapeParameterCount(CanvasShape::rect) +
                                   canvasPaintParameterCount(CanvasPaint::fill), CanvasParameter{});
        none.opacity = 0.0;
        items.push_back(std::move(none));
    }
    std::map<std::uint32_t, std::size_t> depth_index;
    for (auto &item : items) {
        depth_index.emplace(canvasItemKind(item), item.parameters.size());
        item.parameters.push_back(constantParameter(item.depth));
        item.parameters.push_back(item.depth_edge ? *item.depth_edge : constantParameter(0.0));
    }
    std::size_t splits = 0;
    const auto draws = planDraws(items, id, splits);
    if (splits > 0)
        lowered.diagnostics.push_back(
            {{"code", "canvas.draw_split"}, {"output", id}, {"reason", "image_limit"}, {"count", splits + 1}});
    const bool depth = placement.depth.has_value();
    const std::size_t phases = placement.depth_write ? 2 : 1;
    const auto raster_count = draws.size() * phases;

    struct Tables {
        DrawShape shape;
        PlaceDraw place;
        Json ports;
        Json inputs;
    };
    std::vector<Tables> tables;
    for (std::size_t draw_index = 0; draw_index < draws.size(); ++draw_index) {
        const auto &draw = draws[draw_index];
        const bool last = draw_index + 1 == draws.size();
        const auto names = last ? prefix : prefix + "draw" + std::to_string(draw_index) + "_";
        std::vector<double> item_table, bake, frame_initial;
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
        const auto slotOf = [&](const CanvasTexture &texture) {
            return static_cast<double>(std::find(draw.slots.begin(), draw.slots.end(), texture) - draw.slots.begin());
        };
        Tables table;
        table.shape.slots = draw.slots.size();
        // The depth's two parameters sit between the kind's own and the
        // filter numbers.
        table.shape.extra_parameters = 2;
        for (std::size_t instance = 0; instance < draw.items.size(); ++instance) {
            const auto &item = items[draw.items[instance]];
            const auto own = canvasItemKind(item);
            table.shape.kinds.insert(own);
            table.place.depth_index.emplace(own, depth_index.at(own));
            // CV-2b: the item's filters, numbered by first use in the draw
            // (buildPlace refused a blend that reads the backdrop).
            const auto kind = styledKind(item, table.shape);
            const auto row = itemRecord(item, kind, bake, frameEntry, slotOf, [](std::size_t) { return true; }, -1.0,
                                        table.shape.masks);
            item_table.insert(item_table.end(), row.begin(), row.end());
            if (!empty) {
                auto entry = itemTrace(item, id, instance, kind);
                if (draws.size() > 1) entry["draw"] = draw_index;
                lowered.items.push_back(std::move(entry));
            }
        }
        // The placement, after the items.
        table.place.place_offset = bake.size();
        std::vector<const CanvasParameter *> numbers;
        for (const auto &component : placement.position) numbers.push_back(&component);
        for (const auto &component : placement.rotation) numbers.push_back(&component);
        numbers.push_back(&placement.scale);
        for (const auto *number : numbers) bake.push_back(number->value);
        if (std::any_of(numbers.begin(), numbers.end(),
                        [](const CanvasParameter *number) { return number->edge.has_value(); })) {
            table.place.place_source = bake.size();
            for (const auto *number : numbers)
                bake.push_back(number->edge ? static_cast<double>(frameEntry(*number->edge, number->value) + 1) : 0.0);
        }
        table.shape.frame = !frame_sources.empty();

        const auto items_id = names + "items";
        const auto bake_id = names + "bake";
        const auto frame_id = names + "frame";
        lowered.nodes.push_back({{"id", items_id}, {"op", "render.attribute"},
                                 {"parameters", {{"element", "mat4"}, {"domain", "instance"}, {"count", draw.items.size()},
                                                 {"host_source", "runtime_f32"}, {"initial_f32", floatArray(item_table)}}},
                                 {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
        lowered.nodes.push_back({{"id", bake_id}, {"op", "render.buffer"},
                                 {"parameters", {{"element", "float"}, {"size", bake.size() * sizeof(float)},
                                                 {"host_source", "runtime_f32"}, {"initial_f32", floatArray(bake)}}},
                                 {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
        table.ports = {{"items", {{"kind", "buffer"}, {"element", "mat4"}, {"domain", "instance"}, {"access", "read"}}},
                       {"bake", {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}}},
                       {"color", {{"kind", "image"}, {"type", colorType()}, {"access", "write"}, {"attachment", "color"},
                                  {"location", 0}}}};
        table.inputs = {{"items", endpoint(items_id, "resource")}, {"bake", endpoint(bake_id, "resource")}};
        if (table.shape.frame) {
            lowered.nodes.push_back({{"id", frame_id}, {"op", "render.buffer"},
                                     {"parameters", {{"element", "float"}, {"size", frame_initial.size() * sizeof(float)},
                                                     {"host_source", "runtime_f32"},
                                                     {"initial_f32", floatArray(frame_initial)}}},
                                     {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
            // One float per value edge, tiling the frame table from byte 0 (RS-15).
            for (std::size_t index = 0; index < frame_sources.size(); ++index)
                lowered.nodes.push_back({{"id", names + "set_" + std::to_string(index)}, {"op", "render.scalar"},
                                         {"parameters", {{"target", frame_id}, {"offset", index * sizeof(float)}}},
                                         {"inputs", {{"value", endpoint(frame_sources[index].node,
                                                                        frame_sources[index].port)}}},
                                         {"outputs", Json::array()}});
            table.ports["frame"] = {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}};
            table.inputs["frame"] = endpoint(frame_id, "resource");
        }
        if (depth)
            table.ports["depth"] = {{"kind", "image"}, {"type", depthType()}, {"access", "write"}, {"attachment", "depth"}};
        for (std::size_t slot = 0; slot < draw.slots.size(); ++slot) {
            const auto port = "t" + std::to_string(slot);
            table.ports[port] = {{"kind", "image"}, {"type", colorType()}, {"access", "read"},
                                 {"sampling", {{"filter", draw.slots[slot].nearest ? "nearest" : "linear"},
                                               {"address", "clamp_to_edge"}}}};
            table.inputs[port] = texture_source(draw.slots[slot]);
        }
        tables.push_back(std::move(table));
    }

    std::string previous;
    for (std::size_t raster = 0; raster < raster_count; ++raster) {
        const auto phase = raster / draws.size();
        const auto draw_index = raster % draws.size();
        const auto &table = tables[draw_index];
        const auto name =
            raster + 1 == raster_count ? id : prefix + (phase == 0 ? "draw" : "depth") + std::to_string(draw_index);
        const auto vertex = placeVertexShader(output, name, table.shape, table.place);
        auto fragment = fragmentShader(output, name, table.shape);
        if (phase == 1) {
            // Depth only: the pixels the item covers at least half.
            const std::string from = "    if (color.a <= 0.0) discard;\n";
            const auto at = fragment.find(from);
            if (at == std::string::npos) throw std::logic_error("canvas.place: the fragment shader has no discard");
            fragment.replace(at, from.size(), "    if (color.a < 0.5) discard;\n");
        }
        const auto digest = picosha2::hash256_hex_string(vertex + std::string(1, '\0') + fragment);
        const auto stem = "canvas_" + fileStem(name) + "_" + digest.substr(0, 8);
        const auto directory = options.shader_directory + (options.directory_per_stem ? "/" + stem : std::string{});
        lowered.sources.push_back({stem, "vert", vertex, {}, id, directory + "/" + stem + ".vert"});
        lowered.sources.push_back({stem, "frag", fragment, {}, id, directory + "/" + stem + ".frag"});

        auto inputs = table.inputs;
        inputs["color"] = raster == 0 ? endpoint(placement.color.node, placement.color.port) : endpoint(previous, "color");
        if (depth)
            inputs["depth"] =
                raster == 0 ? endpoint(placement.depth->node, placement.depth->port) : endpoint(previous, "depth");
        // Premultiplied over onto the scene's linear colour; the depth pass
        // writes no colour.
        const Json premultiplied{{"color", {{"src", "one"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}}},
                                 {"alpha", {{"src", "one"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}}}};
        const Json attachment =
            phase == 0 ? Json{{"blend", premultiplied}, {"write_mask", "rgba"}} : Json{{"write_mask", "none"}};
        Json state{{"topology", "triangle_list"}, {"cull", "none"}, {"depth_test", depth && placement.depth_test},
                   {"depth_write", phase == 1}, {"color_attachments", Json::array({attachment})}};
        if (depth) state["depth_compare"] = "less_equal";
        const auto shader = "project://" + directory + "/" + stem;
        Json parameters{{"shader", {{"vertex", shader}, {"fragment", shader}}},
                        {"draw", {{"vertex_count", 6}, {"instance_source", "items"}}},
                        {"raster_state", std::move(state)},
                        {"ports", table.ports},
                        {"color_load_op", "load"}};
        if (depth) parameters["depth_load_op"] = "load";
        auto outputs = Json::array({"color"});
        if (depth) outputs.push_back("depth");
        lowered.nodes.push_back({{"id", name},
                                 {"op", "render.raster"},
                                 {"parameters", std::move(parameters)},
                                 {"inputs", std::move(inputs)},
                                 {"outputs", std::move(outputs)}});
        previous = name;
    }
}

// --- CV-3a: a neighbourhood filter's passes ---------------------------------

const char *const fullscreen_vertex = R"glsl(
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

const char *const blur_weight_glsl = R"glsl(// A Gaussian tap's weight, before the taps are normalized.
float canvasBlurWeight(int t, float sigma) {
    return sigma > 1e-4 ? exp(-0.5 * float(t * t) / (sigma * sigma)) : (t == 0 ? 1.0 : 0.0);
}

)glsl";

const char *const blur_texel_glsl = R"glsl(// A texel of the image before this pass; nothing past its edges.
vec4 canvasTexelAt(vec2 pixel) {
    vec2 size = vec2(pelican_size_t0());
    if (any(lessThan(pixel, vec2(0.0))) || any(greaterThanEqual(pixel, size))) return vec4(0.0);
    return pelican_sample_lod_t0((pixel + 0.5) / size, 0.0);
}

)glsl";

// Blur `index` (CanvasBlur) as passes, each a triangle over an image of its
// own: with levels, `_down1` paints the source at the four pixel centres of
// each of its pixels and averages them and `_down<l>` halves the one before;
// `_across` blurs across (painting the source at each tap without levels),
// and the blur's own raster blurs down. The source is painted with the
// draw's code (paintLibrary): its tables are the blur's bake table, its row
// first.
void lowerBlur(const CanvasOutput &output, std::size_t index,
               const std::function<Json(const CanvasTexture &)> &texture_source, const CanvasLoweringOptions &options,
               LoweredOutput &lowered) {
    const auto &blur = output.blurs.at(index);
    const auto &id = output.id;
    const auto name = canvasBlurNodeId(id, index);
    std::vector<double> bake(16, 0.0);
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
    std::vector<CanvasTexture> slots;
    itemTextures(blur.source, slots);
    if (slots.size() > canvas_draw_image_limit)
        throw GraphRegistryError("canvas.draw_images_exceeded@1",
                                 {{"output", id}, {"count", slots.size()}, {"limit", canvas_draw_image_limit}});
    const auto slotOf = [&](const CanvasTexture &texture) {
        return static_cast<double>(std::find(slots.begin(), slots.end(), texture) - slots.begin());
    };
    DrawShape shape;
    shape.srgb = blur.srgb;
    shape.slots = slots.size();
    shape.kinds.insert(canvasItemKind(blur.source));
    const auto kind = styledKind(blur.source, shape);
    const auto row = itemRecord(blur.source, kind, bake, frameEntry, slotOf, [](std::size_t) { return true; }, -1.0,
                                shape.masks);
    std::copy(row.begin(), row.end(), bake.begin());
    const bool source_frame = !frame_sources.empty();
    shape.frame = source_frame;
    // The radius: a frame entry when an edge drives it, else a bake entry.
    std::string radius;
    if (blur.radius.edge) {
        radius = "pelican_load_frame(" + std::to_string(frameEntry(*blur.radius.edge, blur.radius.value)) + "u)";
    } else {
        radius = "pelican_load_bake(" + std::to_string(bake.size()) + "u)";
        bake.push_back(blur.radius.value);
    }
    const bool radius_frame = blur.radius.edge.has_value();
    const auto scale = std::ldexp(1.0, static_cast<int>(blur.levels));
    const auto taken = blur.levels == 0 ? 0.0 : (scale * scale - 1.0) / 12.0 + scale * scale / 6.0;
    const auto sigma_function =
        "// The Gaussian's sigma in this level's pixels (canvasBlurLevelSigma): the\n"
        "// radius clamped to its limit, less what halving and the read back blur.\n"
        "float canvasBlurSigma() {\n    float sigma = clamp(" + radius + ", 0.0, " + glslNumber(blur.radius_limit) +
        ") * " + glslNumber(blur.sigma_scale) + ";\n    return sqrt(max(sigma * sigma - " + glslNumber(taken) +
        ", 0.0)) / " + glslNumber(scale) + ";\n}\n\n";
    const auto taps = "const int BLUR_TAPS = " + std::to_string(blur.taps) + ";\n\n";
    const auto origin = "const vec2 BLUR_ORIGIN = vec2(" + glslNumber(blur.rect[0]) + ", " + glslNumber(blur.rect[1]) +
                        ");\n\n";
    std::string source_functions = "mat4 canvasSourceItem() {\n    return mat4(";
    for (std::size_t k = 0; k < 16; ++k)
        source_functions += (k == 0 ? "" : k % 4 == 0 ? ",\n                " : ", ") +
                            ("pelican_load_bake(" + std::to_string(k) + "u)");
    source_functions += ");\n}\n\n";
    source_functions += "// The layer at a point of the filter's space, as the draw would paint it.\n";
    source_functions += "vec4 canvasSource(mat4 it, vec2 p) {\n    vec4 color = canvasPaint(it, p) * canvasOpacity(it);\n";
    if (!shape.styles.empty()) source_functions += "    color = canvasStyle(it, color);\n";
    source_functions += "    return color;\n}\n\n";
    const auto tap_loop = [](const std::string &sample) {
        return "    float sigma = canvasBlurSigma();\n    vec4 sum = vec4(0.0);\n    float total = 0.0;\n"
               "    for (int t = -BLUR_TAPS; t <= BLUR_TAPS; ++t) {\n"
               "        float w = canvasBlurWeight(t, sigma);\n        sum += w * " +
               sample + ";\n        total += w;\n    }\n    outColor = sum / total;\n";
    };

    const auto bake_id = name + "_bake";
    const auto frame_id = name + "_frame";
    lowered.nodes.push_back({{"id", bake_id}, {"op", "render.buffer"},
                             {"parameters", {{"element", "float"}, {"size", bake.size() * sizeof(float)},
                                             {"host_source", "runtime_f32"}, {"initial_f32", floatArray(bake)}}},
                             {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
    if (!frame_sources.empty()) {
        lowered.nodes.push_back({{"id", frame_id}, {"op", "render.buffer"},
                                 {"parameters", {{"element", "float"}, {"size", frame_initial.size() * sizeof(float)},
                                                 {"host_source", "runtime_f32"},
                                                 {"initial_f32", floatArray(frame_initial)}}},
                                 {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
        for (std::size_t entry = 0; entry < frame_sources.size(); ++entry)
            lowered.nodes.push_back({{"id", name + "_set_" + std::to_string(entry)}, {"op", "render.scalar"},
                                     {"parameters", {{"target", frame_id}, {"offset", entry * sizeof(float)}}},
                                     {"inputs", {{"value", endpoint(frame_sources[entry].node,
                                                                    frame_sources[entry].port)}}},
                                     {"outputs", Json::array()}});
    }

    struct Pass {
        std::string name;
        bool source = false;     // paints the source
        bool sigma = false;      // computes the blur's sigma
        std::string body;        // main()
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    const auto w0 = static_cast<std::uint32_t>(blur.rect[2]), h0 = static_cast<std::uint32_t>(blur.rect[3]);
    std::vector<Pass> passes;
    for (std::uint32_t level = 1; level <= blur.levels; ++level) {
        Pass pass{name + "_down" + std::to_string(level), level == 1, false, {}, w0 >> level, h0 >> level};
        pass.body = level == 1
                        ? "    mat4 it = canvasSourceItem();\n"
                          "    vec2 p = BLUR_ORIGIN + floor(gl_FragCoord.xy) * 2.0;\n"
                          "    outColor = 0.25 * (canvasSource(it, p + vec2(0.5, 0.5)) + canvasSource(it, p + vec2(1.5, 0.5)) +\n"
                          "                       canvasSource(it, p + vec2(0.5, 1.5)) + canvasSource(it, p + vec2(1.5, 1.5)));\n"
                        : "    vec2 p = floor(gl_FragCoord.xy) * 2.0;\n"
                          "    outColor = 0.25 * (canvasTexelAt(p) + canvasTexelAt(p + vec2(1.0, 0.0)) +\n"
                          "                       canvasTexelAt(p + vec2(0.0, 1.0)) + canvasTexelAt(p + vec2(1.0, 1.0)));\n";
        passes.push_back(std::move(pass));
    }
    const auto wl = w0 >> blur.levels, hl = h0 >> blur.levels;
    {
        Pass across{name + "_across", blur.levels == 0, true, {}, wl, hl};
        across.body = blur.levels == 0
                          ? "    mat4 it = canvasSourceItem();\n    vec2 p = BLUR_ORIGIN + gl_FragCoord.xy;\n" +
                                tap_loop("canvasSource(it, p + vec2(float(t), 0.0))")
                          : "    vec2 p = floor(gl_FragCoord.xy);\n" + tap_loop("canvasTexelAt(p + vec2(float(t), 0.0))");
        passes.push_back(std::move(across));
        Pass down{name, false, true, {}, wl, hl};
        down.body = "    vec2 p = floor(gl_FragCoord.xy);\n" + tap_loop("canvasTexelAt(p + vec2(0.0, float(t)))");
        passes.push_back(std::move(down));
    }

    std::string previous;
    for (const auto &pass : passes) {
        std::string vertex = shader_header;
        vertex += generatedBanner(output, pass.name);
        vertex += "// One of a neighbourhood filter's passes: one triangle over its image.\n";
        vertex += shader_includes;
        vertex += fullscreen_vertex;
        std::string fragment = shader_header;
        fragment += generatedBanner(output, pass.name);
        fragment += "// A pass of " + blur.op + " \"" + blur.node + "\": " +
                    (pass.source ? std::string{"the layer painted at its pixels"} : std::string{"the image before"}) +
                    (pass.sigma ? ", blurred with " + std::to_string(2 * blur.taps + 1) + " taps" : std::string{}) +
                    ".\n";
        if (pass.source) fragment += parameter_layout_comment;
        fragment += shader_includes;
        fragment += "\nlayout(location = 0) out vec4 outColor;\n\n";
        if (pass.source) {
            fragment += fitFunctions(output);
            fragment += "\n";
            fragment += paintLibrary(shape);
            fragment += origin;
            fragment += source_functions;
        } else {
            fragment += blur_texel_glsl;
        }
        if (pass.sigma) {
            fragment += blur_weight_glsl;
            fragment += sigma_function;
            fragment += taps;
        }
        fragment += "void main() {\n" + pass.body + "}\n";
        const auto digest = picosha2::hash256_hex_string(vertex + std::string(1, '\0') + fragment);
        const auto stem = "canvas_" + fileStem(pass.name) + "_" + digest.substr(0, 8);
        const auto directory = options.shader_directory + (options.directory_per_stem ? "/" + stem : std::string{});
        lowered.sources.push_back({stem, "vert", vertex, {}, id, directory + "/" + stem + ".vert"});
        lowered.sources.push_back({stem, "frag", fragment, {}, id, directory + "/" + stem + ".frag"});

        const auto image = pass.name + "_image";
        lowered.nodes.push_back({{"id", image}, {"op", "render.image"},
                                 {"parameters", fixedImageParameters(default_canvas_format, pass.width, pass.height)},
                                 {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
        // Each pass declares only what its shaders read.
        Json ports{{"image", {{"kind", "image"}, {"type", colorType()}, {"access", "write"}, {"attachment", "color"},
                              {"location", 0}}}};
        Json inputs{{"image", endpoint(image, "resource")}};
        const bool reads_bake = pass.source || (pass.sigma && !radius_frame);
        const bool reads_frame = (pass.source && source_frame) || (pass.sigma && radius_frame);
        if (reads_bake) {
            ports["bake"] = {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}};
            inputs["bake"] = endpoint(bake_id, "resource");
        }
        if (reads_frame) {
            ports["frame"] = {{"kind", "buffer"}, {"element", "float"}, {"access", "read"}};
            inputs["frame"] = endpoint(frame_id, "resource");
        }
        if (pass.source) {
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                const auto port = "t" + std::to_string(slot);
                ports[port] = {{"kind", "image"}, {"type", colorType()}, {"access", "read"},
                               {"sampling", {{"filter", slots[slot].nearest ? "nearest" : "linear"},
                                             {"address", "clamp_to_edge"}}}};
                inputs[port] = texture_source(slots[slot]);
            }
        } else {
            ports["t0"] = {{"kind", "image"}, {"type", colorType()}, {"access", "read"},
                           {"sampling", {{"filter", "nearest"}, {"address", "clamp_to_edge"}}}};
            inputs["t0"] = endpoint(previous, "image");
        }
        const auto shader = "project://" + directory + "/" + stem;
        Json parameters{{"shader", {{"vertex", shader}, {"fragment", shader}}},
                        {"draw", {{"vertex_count", 3}}},
                        {"raster_state",
                         {{"topology", "triangle_list"}, {"cull", "none"}, {"depth_test", false},
                          {"depth_write", false}, {"color_attachments", Json::array({{{"write_mask", "rgba"}}})}}},
                        {"ports", std::move(ports)},
                        {"clear_color", Json::array({0.0, 0.0, 0.0, 0.0})}};
        lowered.nodes.push_back({{"id", pass.name},
                                 {"op", "render.raster"},
                                 {"parameters", std::move(parameters)},
                                 {"inputs", std::move(inputs)},
                                 {"outputs", Json::array({"image"})}});
        previous = pass.name;
    }
}

LoweredOutput lowerOutput(const CanvasOutput &output, const CanvasLoweringOptions &options) {
    LoweredOutput lowered;
    const auto &id = output.id;
    // One render.image per file and decode, shared by every draw of the output.
    std::map<std::pair<std::string, bool>, std::string> assets;
    const auto texture_source = [&](const CanvasTexture &texture) -> Json {
        switch (texture.origin) {
        case CanvasTexture::Origin::graph: return endpoint(*texture.node, texture.port);
        case CanvasTexture::Origin::group: return endpoint(canvasGroupRasterNodeId(id, texture.group), "image");
        case CanvasTexture::Origin::blur: return endpoint(canvasBlurNodeId(id, texture.group), "image");
        case CanvasTexture::Origin::file: break;
        }
        const auto key = std::pair{texture.asset, texture.linear};
        auto found = assets.find(key);
        if (found == assets.end()) {
            found = assets.emplace(key, canvasAssetNodeId(id, assets.size())).first;
            lowered.nodes.push_back(
                {{"id", found->second}, {"op", "render.image"},
                 {"parameters", {{"type", colorType()},
                                 {"source", {{"asset", texture.asset},
                                             {"color_space", texture.linear ? "linear" : "srgb"},
                                             {"alpha", "straight"}, {"mips", "none"}}}}},
                 {"inputs", Json::object()}, {"outputs", Json::array({"resource"})}});
        }
        return endpoint(found->second, "resource");
    };
    // CV-4: the atlases are files the caller writes; their render.image nodes
    // come with the first draw that reads them.
    for (const auto &atlas : output.atlases)
        lowered.sources.push_back({atlas.stem, "atlas", {}, atlas.pixels->png, id, atlas.path});
    for (std::size_t group = 0; group < output.groups.size(); ++group)
        lowerComposite(output, output.groups[group].items, output.groups[group].srgb, group, texture_source, options,
                       lowered);
    // CV-3a: the neighbourhood filters drawn through images.
    for (std::size_t blur = 0; blur < output.blurs.size(); ++blur)
        lowerBlur(output, blur, texture_source, options, lowered);
    if (output.placement) lowerPlace(output, texture_source, options, lowered);
    else lowerComposite(output, output.items, false, std::nullopt, texture_source, options, lowered);
    return lowered;
}

// A node left in the lowered scope may read a canvas.output's `image` (the
// raster pass keeps the id and the port) and nothing else of the canvas.
void checkSurvivingEdges(const Json &nodes, const Scope &scope, const Json *outputs) {
    const auto check = [&](const Json &endpoint, const std::string &consumer) {
        const auto *from = producer(scope, endpoint);
        if (from == nullptr || !isCanvasGraphOperation(nodeOp(*from))) return;
        if (nodeOp(*from) == "canvas.output" && endpoint.value("port", std::string{}) == "image") return;
        // CV-6a: the last raster of a canvas.place keeps its id and `color`.
        if (nodeOp(*from) == "canvas.place" && endpoint.value("port", std::string{}) == "color") return;
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

// The names the rendering section declares as targets or buffers itself.
std::set<std::string> declaredRenderingNames(const Json &rendering) {
    std::set<std::string> names;
    for (const auto *key : {"render_targets", "buffers"}) {
        const auto list = rendering.find(key);
        if (list == rendering.end() || !list->is_array()) continue;
        for (const auto &entry : *list)
            if (entry.is_object())
                if (const auto name = entry.find("name"); name != entry.end() && name->is_string())
                    names.insert(name->get<std::string>());
    }
    return names;
}

// A rendering-section pass reads a root canvas.output by the author's id; the
// image is the node the lowering named <id>$canvas_image. `@history` stays on.
void rewriteRenderingInputs(Json &document, std::set<std::string> outputs) {
    const auto rendering = document.find("rendering");
    if (rendering == document.end() || !rendering->is_object()) return;
    for (const auto &declared : declaredRenderingNames(*rendering)) outputs.erase(declared);
    const auto graphs = rendering->find("rendering_passes");
    if (outputs.empty() || graphs == rendering->end() || !graphs->is_array()) return;
    const auto rewrite = [&](Json &name) {
        if (!name.is_string()) return;
        constexpr std::string_view history = "@history";
        auto target = name.get<std::string>();
        const bool past = target.ends_with(history);
        if (past) target.resize(target.size() - history.size());
        if (outputs.contains(target)) name = canvasImageNodeId(target) + (past ? std::string{history} : std::string{});
    };
    for (auto &graph : *graphs) {
        const auto passes = graph.is_object() ? graph.find("passes") : graph.end();
        if (passes == graph.end() || !passes->is_array()) continue;
        for (auto &pass : *passes) {
            const auto input = pass.is_object() ? pass.find("input") : pass.end();
            if (input == pass.end()) continue;
            if (input->is_array())
                for (auto &entry : *input) rewrite(entry);
            else
                rewrite(*input);
        }
    }
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
    case canvas_kind_image: return "image_rgba";
    case canvas_kind_color: return "color";
    case canvas_kind_group: return "group";
    case canvas_kind_glyph_fill: return "glyph_fill";
    case canvas_kind_glyph_stroke: return "glyph_stroke";
    case canvas_kind_backdrop: return "backdrop";
    // CV-3a.
    case canvas_kind_effect_image: return "image_effect";
    case canvas_kind_effect_offset + 0: return "rect_fill_effect";
    case canvas_kind_effect_offset + 1: return "rect_stroke_effect";
    case canvas_kind_effect_offset + 2: return "ellipse_fill_effect";
    case canvas_kind_effect_offset + 3: return "ellipse_stroke_effect";
    case canvas_kind_effect_offset + canvas_kind_glyph_fill: return "glyph_fill_effect";
    case canvas_kind_effect_offset + canvas_kind_glyph_stroke: return "glyph_stroke_effect";
    default: throw std::invalid_argument("unknown canvas item kind: " + std::to_string(kind));
    }
}

bool canvasItemReadsBackdrop(const CanvasItem &item) {
    return item.blend != CanvasBlend::normal || item.source == CanvasItemSource::backdrop;
}

std::uint32_t canvasItemKind(const CanvasItem &item) {
    // CV-3a: a shadow's or a glow's.
    const auto effect = item.effect ? canvas_kind_effect_offset : 0u;
    switch (item.source) {
    case CanvasItemSource::shape: return canvasItemKind(item.shape, item.paint) + effect;
    case CanvasItemSource::image: return item.effect ? canvas_kind_effect_image : canvas_kind_image;
    case CanvasItemSource::color: return canvas_kind_color;
    case CanvasItemSource::group: return canvas_kind_group;
    case CanvasItemSource::glyph:
        return (item.paint == CanvasPaint::fill ? canvas_kind_glyph_fill : canvas_kind_glyph_stroke) + effect;
    case CanvasItemSource::backdrop: return canvas_kind_backdrop;
    }
    throw std::invalid_argument("unknown canvas item source");
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
std::string canvasGroupImageNodeId(const std::string &output, std::size_t group) {
    return reservedPrefix(output) + "g" + std::to_string(group) + "_image";
}
std::string canvasGroupRasterNodeId(const std::string &output, std::size_t group) {
    return reservedPrefix(output) + "g" + std::to_string(group);
}
std::string canvasAssetNodeId(const std::string &output, std::size_t index) {
    return reservedPrefix(output) + "asset" + std::to_string(index);
}
std::string canvasBlurNodeId(const std::string &output, std::size_t index) {
    return reservedPrefix(output) + "blur" + std::to_string(index);
}

nlohmann::json expandCanvasInstances(const nlohmann::json &document) {
    const auto definitions = document.is_object() ? document.find("definitions") : document.end();
    if (definitions == document.end() || !definitions->is_object()) return document;
    std::set<std::string> canvas;
    for (const auto &[name, body] : definitions->items())
        if (body.is_object() && isCanvasDefinition(body)) canvas.insert(name);
    if (canvas.empty()) return document;
    auto expanded = document;
    const auto &bodies = document.at("definitions");
    // A definition may instance another; each pass expands one level. More
    // passes than definitions means one instances itself.
    for (std::size_t pass = 0;; ++pass) {
        if (pass > bodies.size()) throw std::invalid_argument("canvas definitions instance each other in a cycle");
        bool changed = expandScopeInstances(expanded, bodies, canvas);
        for (auto &[name, body] : expanded.at("definitions").items())
            if (body.is_object() && !canvas.contains(name))
                changed = expandScopeInstances(body, bodies, canvas) || changed;
        if (!changed) break;
    }
    // Every instance of them is expanded; their bodies hold layers that
    // nothing lowers on their own.
    for (const auto &name : canvas) expanded.at("definitions").erase(name);
    return expanded;
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

CanvasIr buildCanvasIr(const nlohmann::json &authored, const CanvasAssetReader &read_asset,
                       const CanvasLoweringOptions &options) {
    CanvasIr ir;
    if (!documentHasCanvasNodes(authored)) return ir;
    const auto document = expandCanvasInstances(authored);
    const auto bodies = loopBodies(document);
    eachScope(document, [&](const std::string &name, const Json &holder) { checkScope(name, holder, bodies); });
    CanvasFontLibrary fonts{read_asset};
    eachScope(document, [&](const std::string &name, const Json &holder) {
        const auto nodes = holder.find("nodes");
        if (nodes == holder.end() || !nodes->is_array()) return;
        const auto scope = indexScope(name, *nodes);
        for (const auto &node : *nodes)
            if (nodeOp(node) == "canvas.output" || nodeOp(node) == "canvas.place") {
                ir.outputs.push_back(nodeOp(node) == "canvas.output" ? buildOutput(scope, node, ir, fonts)
                                                                     : buildPlace(scope, node, ir, fonts));
                bakeGlyphAtlases(ir.outputs.back(), fonts, options, ir.diagnostics);
            }
    });
    return ir;
}

CanvasLowering lowerCanvasNodes(const nlohmann::json &document, const CanvasLoweringOptions &options,
                                const CanvasAssetReader &read_asset) {
    CanvasLowering result;
    if (!documentHasCanvasNodes(document)) {
        result.document = document;
        return result;
    }
    result.document = expandCanvasInstances(document);
    result.ir = buildCanvasIr(result.document, read_asset, options);
    result.diagnostics = result.ir.diagnostics;
    result.changed = true;
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
            if (op != "canvas.output" && op != "canvas.place") continue;
            auto output = lowerOutput(*outputs.at({name, nodeId(node)}), options);
            for (auto &added : output.nodes) lowered.push_back(std::move(added));
            for (auto &source : output.sources) {
                source.scope = name;
                result.generated_sources.push_back(std::move(source));
            }
            for (auto &diagnostic : output.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
            result.folded_constants += output.folded_constants;
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
    std::set<std::string> root_outputs;
    for (const auto &output : result.ir.outputs)
        if (output.scope.empty() && !output.placement) root_outputs.insert(output.id);
    rewriteRenderingInputs(result.document, std::move(root_outputs));
    if (auto definitions = result.document.find("definitions");
        definitions != result.document.end() && definitions->is_object())
        for (auto &[name, body] : definitions->items())
            if (body.is_object()) lowerScope(name, body);
    return result;
}

} // namespace Pelican
