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
};

// The context a list starts from when it is composited on its own: no
// opacity, no mask, no transform, the stacks it came from kept for the trace.
Flattening freshFlattening(const Flattening &outer) {
    Flattening fresh;
    fresh.stacks = outer.stacks;
    fresh.srgb = outer.srgb;
    fresh.output = outer.output;
    fresh.fonts = outer.fonts;
    return fresh;
}

// Lifts an item made under a fresh context into `flattening`'s list.
void applyFlattening(CanvasItem &item, const Flattening &flattening) {
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
    for (const auto &placed : layoutCanvasText(*opened.file, codepoints, style, id)) {
        if (opened.glyph(placed.glyph).outline.contours.empty()) continue;
        CanvasItem item;
        item.source = CanvasItemSource::glyph;
        item.paint = stroke ? CanvasPaint::stroke : CanvasPaint::fill;
        item.parameters = shared;
        item.parameters.resize(canvas_glyph_parameter_count);
        item.parameters.insert(item.parameters.end(), paint_parameters.begin(), paint_parameters.end());
        item.glyph = CanvasGlyph{font_index, placed.glyph, placed.codepoint, {placed.x, placed.y}, cell};
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
    onlyKeys(node, {"asset", "offset", "size", "channel", "outside", "sampler"});
    if (luminanceChannel(node))
        throw std::invalid_argument("canvas.image channel luminance is read as a mask, not drawn: " + nodeId(node));
    CanvasItem item;
    item.source = CanvasItemSource::image;
    // A linear stack reads the file through the sRGB decode; an sRGB stack
    // reads its bytes as they are and composites those.
    item.texture = {CanvasTexture::Origin::file, imageAsset(node), flattening.srgb, std::nullopt, {}, 0,
                    nearestSampler(node)};
    appendVector(item.parameters, scope, node, "offset", {"x", "y"}, 0.0);
    const auto size = requiredSize(node);
    item.parameters.push_back(constantParameter(size[0]));
    item.parameters.push_back(constantParameter(size[1]));
    item.parameters.push_back(constantParameter(static_cast<double>(CanvasAlpha::straight)));
    item.parameters.push_back(constantParameter(static_cast<double>(CanvasEncode::none)));
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
void appendFilter(const Scope &scope, const Json &node, const Flattening &flattening,
                  std::vector<std::string> &active, CanvasIr &ir, std::vector<CanvasItem> &items) {
    auto filter = parseCanvasFilter(node);
    const auto &inputs = objectMember(node, "inputs");
    const auto layer = inputs.find("layer");
    if (layer == inputs.end()) throw std::invalid_argument(nodeOp(node) + " requires a layer edge: " + nodeId(node));
    auto inner = flattening;
    inner.stacks.insert(inner.stacks.begin(), nodeId(node));
    auto item = singleItem(scope, *layer, inner, active, ir);
    if (!item) return;
    item->filters.push_back(std::move(filter));
    items.push_back(std::move(*item));
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
                           const Flattening &flattening, CanvasIr &ir, bool with_opacity, bool with_mask) {
    (void)ir;
    const auto id = nodeId(stack);
    const auto &inputs = objectMember(stack, "inputs");
    Flattening inner = flattening;
    inner.stacks.insert(inner.stacks.begin(), id);
    inner.pass_through = layer.pass_through;
    inner.own_opacity = false;
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
        singleItem(scope, *base.edge, layerFlattening(scope, stack, base, inner, ir, false, true), active, ir);
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
                key != "mask" && key != "clip" && key != "pass_through")
                throw std::invalid_argument("canvas.stack layer does not take " + key + ": " + id);
        }
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
    onlyKeys(stack, {"size", "layers", "blend_space"});
    const auto &parameters = objectMember(stack, "parameters");
    (void)positiveSize(parameters, "canvas.stack " + id);
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
    if (op.starts_with("filter.") && !isCanvasColorFilter(op))
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
        // The neighbourhood filters (blur, ...) are not lowered yet.
        if (nodeOp(node).starts_with("filter.") && !isCanvasColorFilter(nodeOp(node)))
            throw GraphRegistryError("canvas.op_unsupported@1", {{"node", nodeId(node)}, {"op", nodeOp(node)}});
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
            from != nullptr && isCanvasGraphOperation(nodeOp(*from)) && nodeOp(*from) != "canvas.output")
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

// Each item of an output's lists, its groups' too.
template <typename Visit> void eachOutputItem(CanvasOutput &output, Visit visit) {
    for (auto &item : output.items) visit(item);
    for (auto &group : output.groups)
        for (auto &item : group.items) visit(item);
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
};

// CV-2b: the kind's parameter count, where an item's filter numbers start.
std::size_t kindParameterCount(std::uint32_t kind) {
    switch (kind) {
    case canvas_kind_image: return 6;
    case canvas_kind_color: return 4;
    case canvas_kind_group: return 1;
    case canvas_kind_glyph_fill: return canvas_glyph_parameter_count + canvasPaintParameterCount(CanvasPaint::fill);
    case canvas_kind_glyph_stroke: return canvas_glyph_parameter_count + canvasPaintParameterCount(CanvasPaint::stroke);
    case canvas_kind_backdrop: return 0;
    default: break;
    }
    return canvasShapeParameterCount(kind < 2 ? CanvasShape::rect : CanvasShape::ellipse) +
           canvasPaintParameterCount(kind % 2 == 1 ? CanvasPaint::stroke : CanvasPaint::fill);
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
        text += "    case " + std::to_string(kind) + "u: return " + std::to_string(kindParameterCount(kind)) + "u;\n";
    text += "    default: return 0u;\n    }\n}\n\n";
    text += "vec4 canvasStyle(mat4 it, vec4 color) {\n";
    text += "    uint kind = uint(it[0].x + 0.5);\n";
    text += "    uint r = uint(it[0].y + 0.5) + canvasParamCount(kind % 256u);\n";
    text += "    vec3 c = canvasStraight(color);\n";
    text += "    switch (kind / 256u) {\n";
    for (std::size_t style = 0; style < draw.styles.size(); ++style) {
        const auto &[blend, list] = draw.styles[style];
        text += "    case " + std::to_string(style + 1) + "u: // " + std::string{canvasBlendName(blend)};
        for (const auto kind : list) text += ", " + std::string{canvasFilterOperation(kind)};
        text += "\n";
        for (const auto kind : list) {
            text += "        c = canvasFilter" + std::to_string(static_cast<int>(kind)) + "(c, r);\n";
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
    for (const auto kind : draw.kinds) {
        text += "    case " + std::to_string(kind) + "u: { // " + std::string{canvasItemKindName(kind)} + "\n";
        if (kind == canvas_kind_image) {
            text += "        vec2 size = vec2(canvasParam(it, 2u), canvasParam(it, 3u));\n";
            text += "        center = vec2(canvasParam(it, 0u), canvasParam(it, 1u)) + 0.5 * size;\n";
            text += "        bound = 0.5 * abs(size);\n";
            text += "        break;\n    }\n";
            continue;
        }
        if (kind == canvas_kind_glyph_fill || kind == canvas_kind_glyph_stroke) {
            text += "        float s = canvasParam(it, 2u);\n";
            text += "        vec2 cell_size = s * vec2(canvasParam(it, 5u), canvasParam(it, 6u));\n";
            text += "        center = vec2(canvasParam(it, 0u), canvasParam(it, 1u)) +\n";
            text += "                 s * vec2(canvasParam(it, 3u), canvasParam(it, 4u)) + 0.5 * cell_size;\n";
            text += "        bound = 0.5 * abs(cell_size);\n";
            text += "        break;\n    }\n";
            continue;
        }
        if (kind == canvas_kind_color || kind == canvas_kind_group || kind == canvas_kind_backdrop) {
            text += "        center = 0.5 * CANVAS_SIZE;\n";
            text += "        bound = 0.5 * CANVAS_SIZE;\n";
            text += "        break;\n    }\n";
            continue;
        }
        const auto shape = kind < 2 ? CanvasShape::rect : CanvasShape::ellipse;
        const bool stroke = kind % 2 == 1;
        const auto rotation = shape == CanvasShape::rect ? 5 : 4;
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
    vCanvas = canvas;
    vItem = uint(gl_InstanceIndex);
}
)glsl";
    return text;
}

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
    text += parameterFunctions(draw.frame);
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
    const auto param = [](std::size_t index) { return "canvasParam(it, " + std::to_string(index) + "u)"; };
    for (const auto kind : draw.kinds) {
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
        if (kind == canvas_kind_glyph_fill || kind == canvas_kind_glyph_stroke) {
            // The texel the point falls on in the glyph's cell, kept a half
            // texel inside it so the filter never reads the next cell.
            text += "        float s = " + param(2) + ";\n";
            text += "        vec2 cell_size = s * vec2(" + param(5) + ", " + param(6) + ");\n";
            text += "        float n = " + param(11) + ";\n";
            text += "        vec2 tex = (q - vec2(" + param(0) + ", " + param(1) + ") - s * vec2(" + param(3) + ", " +
                    param(4) + ")) / cell_size * n;\n";
            text += "        vec2 uv = vec2(" + param(7) + ", " + param(8) + ") + clamp(tex, vec2(0.5), vec2(n - 0.5)) / n * vec2(" +
                    param(9) + ", " + param(10) + ");\n";
            text += "        float d = canvasGlyphDistance(canvasSample(uint(it[3].z + 0.5), uv));\n";
            if (kind == canvas_kind_glyph_stroke)
                text += "        d = canvasGlyphStroke(d, " + param(canvas_glyph_parameter_count + 4) + " * n / abs(cell_size.x), " +
                        param(canvas_glyph_parameter_count + 5) + ");\n";
            // Texels per pixel, from the screen-space change of the texel.
            text += "        float aa = max(length(vec2(length(dFdx(tex)), length(dFdy(tex)))) * 0.70710678, 1e-6);\n";
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
        const auto shape = kind < 2 ? CanvasShape::rect : CanvasShape::ellipse;
        const bool stroke = kind % 2 == 1;
        const auto rotation = shape == CanvasShape::rect ? 5 : 4;
        text += "        vec2 half_size = 0.5 * abs(vec2(" + param(0) + ", " + param(1) + "));\n";
        text += "        vec2 local = canvasRotate(q - vec2(" + param(2) + ", " + param(3) + "), -radians(" +
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
        text += "        float a = clamp(" + param(parameterIndex(shape, 3)) + ", 0.0, 1.0) * clamp(0.5 - d / aa, 0.0, 1.0);\n";
        text += "        color = vec4(canvasShapeColor(vec3(" + param(parameterIndex(shape, 0)) + ", " +
                param(parameterIndex(shape, 1)) + ", " + param(parameterIndex(shape, 2)) + ")) * a, a);\n";
        text += "        break;\n    }\n";
    }
    text += "    default:\n        break;\n    }\n";
    if (draw.masks) text += "    color *= canvasMask(it, p);\n";
    text += "    return color;\n}\n\nvoid main() {\n";
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

Json canvasImageParameters(const CanvasOutput &output) {
    Json parameters{{"type", colorType()}, {"format", output.format},
                    {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
    if (output.image_size) {
        parameters["width"] = (*output.image_size)[0];
        parameters["height"] = (*output.image_size)[1];
    } else {
        parameters["extent_scale"] = output.extent_scale;
    }
    return parameters;
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
        for (const auto index : draw.items) {
            const auto instance = local.size();
            local.emplace(index, instance);
            const auto &item = items[index];
            auto kind = canvasItemKind(item);
            shape.kinds.insert(kind);
            // CV-2b: the item's style, numbered by first use in the draw.
            if (item.blend != CanvasBlend::normal || !item.filters.empty()) {
                std::pair<CanvasBlend, std::vector<CanvasFilterKind>> style{item.blend, {}};
                for (const auto &filter : item.filters) style.second.push_back(filter.kind);
                auto found = std::find(shape.styles.begin(), shape.styles.end(), style);
                if (found == shape.styles.end()) found = shape.styles.insert(shape.styles.end(), std::move(style));
                kind += canvas_kind_style_stride * static_cast<std::uint32_t>(found - shape.styles.begin() + 1);
            }
            const auto bake_offset = static_cast<double>(bake.size());
            for (const auto &parameter : item.parameters) bake.push_back(parameter.value);
            for (const auto &filter : item.filters) bake.insert(bake.end(), filter.constants.begin(), filter.constants.end());
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
            double mask_offset = -1.0;
            if (!item.masks.empty()) {
                shape.masks = true;
                mask_offset = static_cast<double>(bake.size());
                for (const auto &mask : item.masks) {
                    bake.push_back(slotOf(mask.texture));
                    bake.push_back(mask.luminance ? 1.0 : 0.0);
                    bake.push_back(mask.outside);
                    bake.insert(bake.end(), mask.rect.begin(), mask.rect.end());
                    bake.insert(bake.end(), mask.affine.begin(), mask.affine.end());
                }
            }
            const double clip = item.clip_base ? static_cast<double>(*item.clip_base) : -1.0;
            const bool sampled = item.source == CanvasItemSource::image || item.source == CanvasItemSource::group ||
                                 item.source == CanvasItemSource::glyph;
            const double row[16] = {static_cast<double>(kind), bake_offset, source_offset, item.opacity,
                                    opacity_offset, static_cast<double>(item.opacity_edges.size()), mask_offset,
                                    static_cast<double>(item.masks.size()),
                                    item.affine[0], item.affine[1], item.affine[2], item.affine[3],
                                    item.affine[4], item.affine[5], sampled ? slotOf(item.texture) : -1.0, clip};
            item_table.insert(item_table.end(), std::begin(row), std::end(row));
            if (!empty) {
                auto nodes = Json::array();
                for (const auto &node : item.nodes) nodes.push_back(node);
                Json entry{{"output", id}, {"instance", instance},
                           {"kind", std::string{canvasItemKindName(kind % canvas_kind_style_stride)}},
                           {"nodes", std::move(nodes)}};
                if (group) entry["group"] = *group;
                if (draws.size() > 1) entry["draw"] = draw_index;
                if (item.blend != CanvasBlend::normal) entry["blend"] = std::string{canvasBlendName(item.blend)};
                if (!item.filters.empty()) {
                    entry["filters"] = Json::array();
                    for (const auto &filter : item.filters)
                        entry["filters"].push_back({{"node", filter.node},
                                                    {"op", std::string{canvasFilterOperation(filter.kind)}}});
                }
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

LoweredOutput lowerOutput(const CanvasOutput &output, const CanvasLoweringOptions &options) {
    LoweredOutput lowered;
    const auto &id = output.id;
    // One render.image per file and decode, shared by every draw of the output.
    std::map<std::pair<std::string, bool>, std::string> assets;
    const auto texture_source = [&](const CanvasTexture &texture) -> Json {
        switch (texture.origin) {
        case CanvasTexture::Origin::graph: return endpoint(*texture.node, texture.port);
        case CanvasTexture::Origin::group: return endpoint(canvasGroupRasterNodeId(id, texture.group), "image");
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
    lowerComposite(output, output.items, false, std::nullopt, texture_source, options, lowered);
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
    default: throw std::invalid_argument("unknown canvas item kind: " + std::to_string(kind));
    }
}

bool canvasItemReadsBackdrop(const CanvasItem &item) {
    return item.blend != CanvasBlend::normal || item.source == CanvasItemSource::backdrop;
}

std::uint32_t canvasItemKind(const CanvasItem &item) {
    switch (item.source) {
    case CanvasItemSource::shape: return canvasItemKind(item.shape, item.paint);
    case CanvasItemSource::image: return canvas_kind_image;
    case CanvasItemSource::color: return canvas_kind_color;
    case CanvasItemSource::group: return canvas_kind_group;
    case CanvasItemSource::glyph:
        return item.paint == CanvasPaint::fill ? canvas_kind_glyph_fill : canvas_kind_glyph_stroke;
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
            if (nodeOp(node) == "canvas.output") {
                ir.outputs.push_back(buildOutput(scope, node, ir, fonts));
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
            if (op != "canvas.output") continue;
            auto output = lowerOutput(*outputs.at({name, nodeId(node)}), options);
            for (auto &added : output.nodes) lowered.push_back(std::move(added));
            for (auto &source : output.sources) {
                source.scope = name;
                result.generated_sources.push_back(std::move(source));
            }
            for (auto &diagnostic : output.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
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
        if (output.scope.empty()) root_outputs.insert(output.id);
    rewriteRenderingInputs(result.document, std::move(root_outputs));
    if (auto definitions = result.document.find("definitions");
        definitions != result.document.end() && definitions->is_object())
        for (auto &[name, body] : definitions->items())
            if (body.is_object()) lowerScope(name, body);
    return result;
}

} // namespace Pelican
