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
};

// The context a list starts from when it is composited on its own: no
// opacity, no mask, no transform, the stacks it came from kept for the trace.
Flattening freshFlattening(const Flattening &outer) {
    Flattening fresh;
    fresh.stacks = outer.stacks;
    fresh.srgb = outer.srgb;
    fresh.output = outer.output;
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
    item.masks = flattening.masks;
    item.affine = flattening.affine;
    item.nodes = {id, nodeId(*shape)};
    item.nodes.insert(item.nodes.end(), flattening.stacks.begin(), flattening.stacks.end());
    items.push_back(std::move(item));
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
        unit.push_back(std::move(*item));
    }
    auto outer = layerFlattening(scope, stack, base, flattening, ir, true, false);
    outer.pass_through = false;
    items.push_back(groupItem(std::move(unit), flattening.srgb, nodeId(*producer(scope, *base.edge)), outer));
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
        if (const auto blend = layer.find("blend"); blend != layer.end() && *blend != "normal")
            throw GraphRegistryError("canvas.blend_unsupported@1",
                                     {{"node", id}, {"layer", layerName(layer, index)}, {"blend", *blend}});
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
        if (srgb == flattening.srgb && (group.size() <= 1 || plain)) {
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
    if (op == "canvas.backdrop" || op.starts_with("filter."))
        throw GraphRegistryError("canvas.op_unsupported@1", {{"node", nodeId(*from)}, {"op", op}});
    if (op == "canvas.stack") return appendStack(scope, *from, flattening, active, ir, items);
    if (op == "canvas.transform") return appendTransform(scope, *from, flattening, active, ir, items);
    if (flattening.pass_through)
        throw std::invalid_argument("pass_through is for a nested canvas.stack, not " + op + ": " + nodeId(*from));
    if (op == "shape.fill" || op == "shape.stroke") return appendPaint(scope, *from, flattening, items);
    if (op == "canvas.image") return appendImage(scope, *from, flattening, items);
    if (op == "canvas.from_image") return appendFromImage(scope, *from, flattening, items);
    if (op == "canvas.color") return appendColor(scope, *from, flattening, items);
    throw std::invalid_argument("a canvas layer comes from shape.fill, shape.stroke, canvas.image, "
                                "canvas.from_image, canvas.color, canvas.transform or canvas.stack, not " +
                                op + ": " + nodeId(*from));
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
    // The output's image is linear: a linear stack draws straight into it, an
    // sRGB one is composited first and decoded as it is read.
    Flattening top;
    top.pass_through = true;
    top.output = &output;
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
        // CV-2b's: they read the backdrop.
        if (nodeOp(node) == "canvas.backdrop" || nodeOp(node).starts_with("filter."))
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
};

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
    uint kind = uint(it[0].x + 0.5);
    vec2 center = vec2(0.0);
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
        if (kind == canvas_kind_color || kind == canvas_kind_group) {
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
    text += R"glsl(vec4 canvasPaint(mat4 it, vec2 p) {
    uint kind = uint(it[0].x + 0.5);
    vec2 q = canvasItemPoint(it, p);
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
        if (kind == canvas_kind_color) {
            text += "        if (canvasInside(q / CANVAS_SIZE)) {\n";
            text += "            float a = clamp(" + param(3) + ", 0.0, 1.0);\n";
            text += "            color = vec4(canvasShapeColor(vec3(" + param(0) + ", " + param(1) + ", " + param(2) +
                    ")) * a, a);\n";
            text += "        }\n        break;\n    }\n";
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
    text += R"glsl(    color *= canvasOpacity(it);
    if (color.a <= 0.0) discard;
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
    if (item.source == CanvasItemSource::image || item.source == CanvasItemSource::group) add(item.texture);
    for (const auto &mask : item.masks) add(mask.texture);
}

struct CanvasDraw {
    std::vector<std::size_t> items;
    std::vector<CanvasTexture> slots;
    // Clip layers, drawn atop what the draws before drew (their base).
    bool atop = false;
};

// Items in order, cut where clip layers start or end, and where the next item
// would take the draw past canvas_draw_image_limit images (`splits` counts
// those cuts).
std::vector<CanvasDraw> planDraws(const std::vector<CanvasItem> &items, const std::string &output,
                                  std::size_t &splits) {
    std::vector<CanvasDraw> draws(1);
    splits = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        std::vector<CanvasTexture> textures;
        itemTextures(items[index], textures);
        if (textures.size() > canvas_draw_image_limit)
            throw GraphRegistryError("canvas.draw_images_exceeded@1", {{"output", output},
                                                                        {"count", textures.size()},
                                                                        {"limit", canvas_draw_image_limit}});
        const bool atop = items[index].clip_base.has_value();
        auto merged = draws.back().slots;
        for (const auto &texture : textures)
            if (std::find(merged.begin(), merged.end(), texture) == merged.end()) merged.push_back(texture);
        const bool full = merged.size() > canvas_draw_image_limit;
        if (!draws.back().items.empty() && (full || atop != draws.back().atop)) {
            if (full) ++splits;
            draws.emplace_back();
            merged = textures;
        }
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
    const auto draws = planDraws(items, id, splits);
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
        for (const auto index : draw.items) {
            const auto instance = local.size();
            local.emplace(index, instance);
            const auto &item = items[index];
            const auto kind = canvasItemKind(item);
            shape.kinds.insert(kind);
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
            const bool sampled = item.source == CanvasItemSource::image || item.source == CanvasItemSource::group;
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
                           {"kind", std::string{canvasItemKindName(kind)}}, {"nodes", std::move(nodes)}};
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
        lowered.sources.push_back({stem, "vert", vertex, id, directory + "/" + stem + ".vert"});
        lowered.sources.push_back({stem, "frag", fragment, id, directory + "/" + stem + ".frag"});

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
    default: throw std::invalid_argument("unknown canvas item kind: " + std::to_string(kind));
    }
}

std::uint32_t canvasItemKind(const CanvasItem &item) {
    switch (item.source) {
    case CanvasItemSource::shape: return canvasItemKind(item.shape, item.paint);
    case CanvasItemSource::image: return canvas_kind_image;
    case CanvasItemSource::color: return canvas_kind_color;
    case CanvasItemSource::group: return canvas_kind_group;
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

CanvasIr buildCanvasIr(const nlohmann::json &authored) {
    CanvasIr ir;
    if (!documentHasCanvasNodes(authored)) return ir;
    const auto document = expandCanvasInstances(authored);
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

CanvasLowering lowerCanvasNodes(const nlohmann::json &document, const CanvasLoweringOptions &options) {
    CanvasLowering result;
    if (!documentHasCanvasNodes(document)) {
        result.document = document;
        return result;
    }
    result.document = expandCanvasInstances(document);
    result.ir = buildCanvasIr(result.document);
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
