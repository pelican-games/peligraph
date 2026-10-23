#include "graphcanvasthumbnail.hpp"

#include "../../project/canvas/canvasblend.hpp"
#include "../../project/canvas/canvaslowering.hpp"
#include "../../project/canvas/canvasmodule.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/renderconfigdocument.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <cstdio>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

constexpr std::string_view project_scheme = "project://";

std::string text(const Json &node, const char *key) {
    if (!node.is_object()) return {};
    const auto found = node.find(key);
    return found != node.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

const Json *findNode(const Json &nodes, std::string_view id) {
    if (!nodes.is_array()) return nullptr;
    for (const auto &node : nodes)
        if (node.is_object() && text(node, "id") == id) return &node;
    return nullptr;
}

// The nodes an input value names: a {node, port} edge, or a list of them.
void eachEdge(const Json &value, const std::function<void(const Json &edge)> &visit) {
    if (value.is_array()) {
        for (const auto &element : value) eachEdge(element, visit);
        return;
    }
    if (value.is_object() && value.contains("node") && value.at("node").is_string()) visit(value);
}

void eachInputEdge(const Json &node, const std::function<void(const std::string &port, const Json &edge)> &visit) {
    const auto inputs = node.is_object() ? node.find("inputs") : node.end();
    if (inputs == node.end() || !inputs->is_object()) return;
    for (const auto &[port, value] : inputs->items()) eachEdge(value, [&](const Json &edge) { visit(port, edge); });
}

// A stack's `size`, when it is two positive numbers.
std::optional<std::array<double, 2>> stackSize(const Json &node) {
    if (!node.is_object()) return std::nullopt;
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object()) return std::nullopt;
    const auto size = parameters->find("size");
    if (size == parameters->end() || !size->is_array() || size->size() != 2 || !size->at(0).is_number() ||
        !size->at(1).is_number())
        return std::nullopt;
    const std::array<double, 2> result{size->at(0).get<double>(), size->at(1).get<double>()};
    if (!(result[0] > 0.0) || !(result[1] > 0.0)) return std::nullopt;
    return result;
}

// The size of the first stack with one that `id`'s layer is drawn into,
// nearest first.
std::optional<std::array<double, 2>> drawnIntoSize(const Json &nodes, const std::string &id) {
    std::map<std::string, std::vector<const Json *>> consumers;
    for (const auto &node : nodes)
        eachInputEdge(node, [&](const std::string &, const Json &edge) {
            consumers[edge.at("node").get<std::string>()].push_back(&node);
        });
    std::set<std::string> seen{id};
    std::deque<std::string> queue{id};
    while (!queue.empty()) {
        const auto at = queue.front();
        queue.pop_front();
        for (const Json *consumer : consumers[at]) {
            const auto consumer_id = text(*consumer, "id");
            if (!seen.insert(consumer_id).second) continue;
            if (text(*consumer, "op") == "canvas.stack")
                if (const auto size = stackSize(*consumer)) return size;
            queue.push_back(consumer_id);
        }
    }
    return std::nullopt;
}

// A canvas.image's own extent: offset plus size.
std::optional<std::array<double, 2>> imageExtent(const Json &node) {
    if (text(node, "op") != "canvas.image") return std::nullopt;
    const auto &parameters = node.value("parameters", Json::object());
    const auto pair = [&](const char *key, std::array<double, 2> fallback) {
        const auto found = parameters.find(key);
        if (found == parameters.end() || !found->is_array() || found->size() != 2 || !found->at(0).is_number() ||
            !found->at(1).is_number())
            return fallback;
        return std::array<double, 2>{found->at(0).get<double>(), found->at(1).get<double>()};
    };
    const auto offset = pair("offset", {0.0, 0.0});
    const auto size = pair("size", {0.0, 0.0});
    const std::array<double, 2> extent{offset[0] + size[0], offset[1] + size[1]};
    if (!(extent[0] > 0.0) || !(extent[1] > 0.0)) return std::nullopt;
    return extent;
}

std::string unusedId(const Json &nodes, const std::string &base) {
    std::string id = base;
    for (int n = 2; findNode(nodes, id) != nullptr; ++n) id = base + "_" + std::to_string(n);
    return id;
}

bool followsInputs(const std::string &op) {
    return Pelican::isCanvasGraphOperation(op) || op == "graph.instance";
}

// The document: the scope's nodes `start` reads (in the scope's order), the
// definitions their instances name, then `added`.
Json assemble(const Json &scope_nodes, const Json &definitions, const std::vector<std::string> &start,
              const std::vector<Json> &added) {
    std::set<std::string> kept;
    std::deque<std::string> queue(start.begin(), start.end());
    while (!queue.empty()) {
        const auto id = queue.front();
        queue.pop_front();
        if (kept.contains(id)) continue;
        const Json *node = findNode(scope_nodes, id);
        if (node == nullptr) continue;
        kept.insert(id);
        // A number a non-canvas node gives is its own value; what it reads
        // is not the picture's.
        if (!followsInputs(text(*node, "op"))) continue;
        eachInputEdge(*node, [&](const std::string &, const Json &edge) {
            queue.push_back(edge.at("node").get<std::string>());
        });
    }
    Json nodes = Json::array();
    std::set<std::string> named;
    std::deque<const Json *> holders;
    for (const auto &node : scope_nodes)
        if (node.is_object() && kept.contains(text(node, "id"))) {
            nodes.push_back(node);
            holders.push_back(&node);
        }
    // The definitions the instances name, and the ones those name.
    Json picked = Json::object();
    while (!holders.empty()) {
        const Json *node = holders.front();
        holders.pop_front();
        const auto definition = text(*node, "definition");
        if (definition.empty() || !named.insert(definition).second) continue;
        if (!definitions.is_object() || !definitions.contains(definition)) continue;
        const auto &body = definitions.at(definition);
        picked[definition] = body;
        if (body.is_object() && body.contains("nodes") && body.at("nodes").is_array())
            for (const auto &inner : body.at("nodes")) holders.push_back(&inner);
    }
    for (const auto &node : added) nodes.push_back(node);
    return Json{{"schema", "pelican.graph"},
                {"version", 1},
                {"definitions", std::move(picked)},
                {"nodes", std::move(nodes)},
                {"outputs", Json::object()}};
}

Json edgeTo(const std::string &node, const char *port) { return Json{{"node", node}, {"port", port}}; }

Json outputNode(const std::string &id, const std::string &stack) {
    return Json{{"id", id},
                {"op", "canvas.output"},
                {"parameters", Json::object()},
                {"inputs", {{"layer", edgeTo(stack, "layer")}}},
                {"outputs", Json::array({"image"})}};
}

// A stack of `size` whose one layer is `source`'s layer.
Json wrappingStack(const std::string &id, const std::string &source, std::array<double, 2> size) {
    return Json{{"id", id},
                {"op", "canvas.stack"},
                {"parameters",
                 {{"size", Json::array({size[0], size[1]})},
                  {"layers", Json::array({Json{{"input", "thumbnail"}}})}}},
                {"inputs", {{"thumbnail", edgeTo(source, "layer")}}},
                {"outputs", Json::array({"layer"})}};
}

bool hasLayerOutput(const Json &node) {
    const auto outputs = node.find("outputs");
    if (outputs == node.end()) return true;
    if (!outputs->is_array()) return false;
    return std::ranges::any_of(*outputs, [](const Json &port) { return port.is_string() && port == "layer"; });
}

void collectReferences(const Json &value, std::set<std::string> &into) {
    if (value.is_string()) {
        const auto &string = value.get_ref<const std::string &>();
        if (string.starts_with(project_scheme)) into.insert(string);
    } else if (value.is_array() || value.is_object()) {
        for (const auto &element : value) collectReferences(element, into);
    }
}

std::string numberText(double value) {
    std::ostringstream out;
    out.precision(4);
    out << value;
    return out.str();
}

// The textures an item and its masks sample.
void eachTexture(const Pelican::CanvasItem &item, const std::function<void(const Pelican::CanvasTexture &)> &visit) {
    using Source = Pelican::CanvasItemSource;
    if (item.source == Source::image) visit(item.texture);
    for (const auto &mask : item.masks) visit(mask.texture);
}

void eachItem(const Pelican::CanvasOutput &output, const std::function<void(const Pelican::CanvasItem &)> &visit) {
    for (const auto &item : output.items) visit(item);
    for (const auto &group : output.groups)
        for (const auto &item : group.items) visit(item);
    for (const auto &blur : output.blurs) visit(blur.source);
}

// NB-4: a box in canvas units, x0 y0 x1 y1.
struct Box {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    [[nodiscard]] bool empty() const { return !(x1 > x0) || !(y1 > y0); }
    [[nodiscard]] Box meet(const Box &other) const {
        return {std::max(x0, other.x0), std::max(y0, other.y0), std::min(x1, other.x1), std::min(y1, other.y1)};
    }
};

// The canvas box of `affine` applied to the quad of `centre` and `half`.
Box quadBox(const std::array<double, 6> &m, double cx, double cy, double hx, double hy) {
    Box box{1e300, 1e300, -1e300, -1e300};
    for (const auto [sx, sy] : {std::pair{-1.0, -1.0}, std::pair{1.0, -1.0}, std::pair{1.0, 1.0}, std::pair{-1.0, 1.0}}) {
        const double x = cx + sx * hx, y = cy + sy * hy;
        const double px = m[0] * x + m[2] * y + m[4], py = m[1] * x + m[3] * y + m[5];
        box = {std::min(box.x0, px), std::min(box.y0, py), std::max(box.x1, px), std::max(box.y1, py)};
    }
    return box;
}

constexpr double effectReach = 1.33333333; // the reference's effect_reach

// One item's box as the reference's `bounds` makes its quad, or nothing when
// it draws nothing.
std::optional<Box> itemBox(const Pelican::CanvasOutput &output, const Pelican::CanvasItem &item,
                           const Pelican::CanvasValues &values, int depth);

std::optional<Box> itemsBox(const Pelican::CanvasOutput &output, const std::vector<Pelican::CanvasItem> &items,
                            const Pelican::CanvasValues &values, int depth) {
    std::optional<Box> all;
    for (const auto &item : items) {
        const auto box = itemBox(output, item, values, depth);
        if (!box) continue;
        all = all ? Box{std::min(all->x0, box->x0), std::min(all->y0, box->y0), std::max(all->x1, box->x1),
                        std::max(all->y1, box->y1)}
                  : *box;
    }
    return all;
}

std::optional<Box> itemBox(const Pelican::CanvasOutput &output, const Pelican::CanvasItem &item,
                           const Pelican::CanvasValues &values, int depth) {
    using Source = Pelican::CanvasItemSource;
    const auto value = [&](const Pelican::CanvasParameter &parameter) {
        if (!parameter.edge) return parameter.value;
        const auto found = values.find(*parameter.edge);
        return found != values.end() ? found->second : parameter.value;
    };
    const auto p = [&](std::size_t index) {
        return index < item.parameters.size() ? value(item.parameters[index]) : 0.0;
    };
    double opacity = item.opacity;
    for (const auto &edge : item.opacity_edges) {
        const auto found = values.find(edge);
        opacity *= found != values.end() ? found->second : 1.0;
    }
    if (!(opacity > 0.0)) return std::nullopt;
    const auto kind = Pelican::canvasItemKind(item);
    const auto own = kind >= Pelican::canvas_kind_effect_offset ? kind - Pelican::canvas_kind_effect_offset : kind;
    const bool effect = own != kind && kind != Pelican::canvas_kind_effect_image;
    double cx = 0.0, cy = 0.0, hx = 0.0, hy = 0.0;
    std::optional<Box> box;
    switch (item.source) {
    case Source::shape: {
        const auto base = Pelican::canvasShapeParameterCount(item.shape);
        const double half_x = 0.5 * std::abs(p(0)), half_y = 0.5 * std::abs(p(1));
        const double angle = p(base - 1) * 3.14159265358979323846 / 180.0;
        const double c = std::abs(std::cos(angle)), s = std::abs(std::sin(angle));
        cx = p(2);
        cy = p(3);
        // The turned box (an ellipse's bound is inside it).
        hx = c * half_x + s * half_y;
        hy = s * half_x + c * half_y;
        if (item.paint == Pelican::CanvasPaint::stroke) {
            const double outset = std::abs(p(base + 4));
            hx += outset;
            hy += outset;
        }
        break;
    }
    case Source::image:
        cx = p(0) + 0.5 * p(2);
        cy = p(1) + 0.5 * p(3);
        hx = 0.5 * std::abs(p(2));
        hy = 0.5 * std::abs(p(3));
        break;
    case Source::glyph: {
        const double s = p(2);
        cx = p(0) + s * p(3) + 0.5 * s * p(5);
        cy = p(1) + s * p(4) + 0.5 * s * p(6);
        hx = 0.5 * std::abs(s * p(5));
        hy = 0.5 * std::abs(s * p(6));
        break;
    }
    case Source::group: {
        // What the nested stack's own items draw (a group reads only earlier
        // ones), over no more than the canvas.
        if (item.texture.group < output.groups.size() && depth < 64)
            box = itemsBox(output, output.groups[item.texture.group].items, values, depth + 1);
        if (!box) return std::nullopt;
        box = box->meet(quadBox(item.affine, 0.5 * output.size[0], 0.5 * output.size[1], 0.5 * output.size[0],
                                0.5 * output.size[1]));
        break;
    }
    case Source::color:
    case Source::backdrop:
        cx = hx = 0.5 * output.size[0];
        cy = hy = 0.5 * output.size[1];
        break;
    }
    if (effect && item.source != Source::group) {
        // The effect's numbers follow its source's paint (the reference's
        // paintParameterCount): moved by its offset, grown by its reach.
        const bool glyph = own == Pelican::canvas_kind_glyph_fill || own == Pelican::canvas_kind_glyph_stroke;
        const bool stroke = glyph ? own == Pelican::canvas_kind_glyph_stroke : own % 2 == 1;
        const auto e = (glyph ? Pelican::canvas_glyph_parameter_count
                              : Pelican::canvasShapeParameterCount(own < 2 ? Pelican::CanvasShape::rect
                                                                           : Pelican::CanvasShape::ellipse)) +
                       Pelican::canvasPaintParameterCount(stroke ? Pelican::CanvasPaint::stroke
                                                                 : Pelican::CanvasPaint::fill);
        const double reach = effectReach * std::clamp(p(e + 2), 0.0, std::max(p(e + 3), 0.0));
        cx += p(e);
        cy += p(e + 1);
        hx += reach;
        hy += reach;
    }
    if (!box) box = quadBox(item.affine, cx, cy, hx, hy);
    // A number that says nothing about where: the whole canvas (no cut).
    if (!std::isfinite(box->x0) || !std::isfinite(box->y0) || !std::isfinite(box->x1) || !std::isfinite(box->y1))
        return Box{-1e300, -1e300, 1e300, 1e300};
    // A mask that shows nothing past its rectangle cuts the item to it.
    for (const auto &mask : item.masks)
        if (mask.outside == 0.0)
            box = box->meet(quadBox(mask.affine, mask.rect[0] + 0.5 * mask.rect[2], mask.rect[1] + 0.5 * mask.rect[3],
                                    0.5 * std::abs(mask.rect[2]), 0.5 * std::abs(mask.rect[3])));
    if (box->empty()) return std::nullopt;
    return box;
}

std::string refusalText(const Pelican::GraphRegistryError &error) {
    const auto &detail = error.detail();
    const auto field = [&](const char *key) {
        return detail.is_object() && detail.contains(key) && detail.at(key).is_string()
                   ? detail.at(key).get<std::string>()
                   : std::string{};
    };
    if (error.code() == "canvas.font_missing@1") return "The font " + field("font") + " is not in the project";
    if (error.code() == "canvas.font_unreadable@1") return "The font " + field("font") + " cannot be read";
    if (error.code() == "canvas.glyph_missing@1") return "The font has no glyph for a character of " + field("node");
    return error.code() + " " + detail.dump();
}

} // namespace

bool graphCanvasCardOperation(std::string_view op) {
    return op == "canvas.image" || op == "canvas.color" || op == "canvas.from_image" || op == "shape.fill" ||
           op == "shape.stroke" || (op.starts_with("filter.") && Pelican::isCanvasFilter(op));
}

std::string graphCanvasFilterSummary(std::string_view op, const Json &parameters,
                                     const std::vector<std::string> &driven_ports) {
    const auto driven = [&](std::string_view port) {
        return std::ranges::any_of(driven_ports, [&](const std::string &each) {
            return each == port || each.starts_with(std::string{port} + ".");
        });
    };
    const auto number = [&](const char *key, double fallback) {
        const auto found = parameters.is_object() ? parameters.find(key) : parameters.end();
        return found != parameters.end() && found->is_number() ? found->get<double>() : fallback;
    };
    const auto say = [&](const char *word, const char *key, double fallback, const char *unit) {
        if (driven(key)) return std::string{word} + ": line";
        return std::string{word} + " " + numberText(number(key, fallback)) + unit;
    };
    if (op == "filter.blur") return say("Blur", "radius", 1.0, " px");
    if (op == "filter.drop_shadow") return say("Shadow", "radius", 5.0, "");
    if (op == "filter.outer_glow") return say("Glow", "radius", 5.0, "");
    if (op == "filter.brightness_contrast") return say("Brightness", "brightness", 0.0, "");
    if (op == "filter.hue_saturation") return say("Hue", "hue", 0.0, "\xC2\xB0");
    if (op == "filter.levels") return say("Gamma", "gamma", 1.0, "");
    if (op == "filter.gradient_map") {
        const auto stops = parameters.is_object() ? parameters.find("stops") : parameters.end();
        const auto count = stops != parameters.end() && stops->is_array() ? stops->size() : 0;
        return std::to_string(count) + (count == 1 ? " stop" : " stops");
    }
    if (op == "filter.color_overlay") {
        if (driven("color")) return "Color: line";
        const auto color = parameters.is_object() ? parameters.find("color") : parameters.end();
        if (color == parameters.end() || !color->is_array() || color->size() < 3) return {};
        char hex[8];
        const auto byte = [&](std::size_t at) {
            const double value = color->at(at).is_number() ? color->at(at).get<double>() : 0.0;
            return static_cast<unsigned>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
        };
        std::snprintf(hex, sizeof hex, "#%02X%02X%02X", byte(0), byte(1), byte(2));
        return hex;
    }
    return {};
}

GraphCanvasThumbnailDocument graphCanvasThumbnailDocument(const Json &scope_nodes, const Json &definitions,
                                                          const GraphCanvasThumbnailTarget &target) {
    GraphCanvasThumbnailDocument made;
    const Json *node = findNode(scope_nodes, target.node);
    if (node == nullptr) {
        made.error = "No node " + target.node;
        return made;
    }
    const auto op = text(*node, "op");
    const auto output = unusedId(scope_nodes, "pelican_thumbnail_output");
    const auto stack = unusedId(scope_nodes, "pelican_thumbnail_stack");
    switch (target.kind) {
    case GraphCanvasThumbnailKind::composite: {
        if (op != "canvas.stack") {
            made.error = target.node + " is not a canvas.stack";
            return made;
        }
        if (const auto size = stackSize(*node)) {
            made.size = *size;
            made.output = output;
            made.document = assemble(scope_nodes, definitions, {target.node}, {outputNode(output, target.node)});
            return made;
        }
        const auto size = drawnIntoSize(scope_nodes, target.node);
        if (!size) {
            made.error = "The stack has no size, and no stack with one draws it";
            return made;
        }
        made.size = *size;
        made.output = output;
        made.document = assemble(scope_nodes, definitions, {target.node},
                                 {wrappingStack(stack, target.node, *size), outputNode(output, stack)});
        return made;
    }
    case GraphCanvasThumbnailKind::layer: {
        if (op != "canvas.stack") {
            made.error = target.node + " is not a canvas.stack";
            return made;
        }
        const auto &parameters = node->value("parameters", Json::object());
        const auto layers = parameters.find("layers");
        const Json *entry = nullptr;
        if (layers != parameters.end() && layers->is_array())
            for (const auto &element : *layers)
                if (text(element, "input") == target.input) entry = &element;
        if (entry == nullptr) {
            made.error = "The stack has no layer " + target.input;
            return made;
        }
        auto size = stackSize(*node);
        if (!size) size = drawnIntoSize(scope_nodes, target.node);
        if (!size) {
            made.error = "The stack has no size, and no stack with one draws it";
            return made;
        }
        // The stack as it is -- its blend space, its transform -- holding
        // this one layer, shown.
        Json layer = *entry;
        layer.erase("visible");
        layer.erase("clip");
        Json copy = *node;
        copy["id"] = stack;
        copy["parameters"]["layers"] = Json::array({layer});
        copy["parameters"]["size"] = Json::array({(*size)[0], (*size)[1]});
        const auto mask = text(layer, "mask");
        Json inputs = Json::object();
        std::vector<std::string> start;
        if (const auto authored = node->find("inputs"); authored != node->end() && authored->is_object())
            for (const auto &[port, value] : authored->items()) {
                if (port != target.input && port != mask && !port.starts_with(target.input + ".")) continue;
                inputs[port] = value;
                eachEdge(value, [&](const Json &edge) { start.push_back(edge.at("node").get<std::string>()); });
            }
        copy["inputs"] = std::move(inputs);
        copy["outputs"] = Json::array({"layer"});
        made.size = *size;
        made.output = output;
        made.document = assemble(scope_nodes, definitions, start, {std::move(copy), outputNode(output, stack)});
        return made;
    }
    case GraphCanvasThumbnailKind::card: {
        if (!hasLayerOutput(*node)) {
            made.error = target.node + " hands on no layer";
            return made;
        }
        auto size = drawnIntoSize(scope_nodes, target.node);
        if (!size) size = imageExtent(*node);
        if (!size) {
            made.error = "Not on a canvas yet: the stack it is drawn into gives the picture its size";
            return made;
        }
        made.size = *size;
        made.output = output;
        made.document = assemble(scope_nodes, definitions, {target.node},
                                 {wrappingStack(stack, target.node, *size), outputNode(output, stack)});
        return made;
    }
    }
    return made;
}

std::vector<std::string> graphCanvasThumbnailReferences(const Json &document) {
    std::set<std::string> references;
    if (document.is_object()) {
        if (const auto nodes = document.find("nodes"); nodes != document.end())
            for (const auto &node : *nodes)
                if (node.is_object() && node.contains("parameters")) collectReferences(node.at("parameters"), references);
        if (const auto definitions = document.find("definitions");
            definitions != document.end() && definitions->is_object())
            for (const auto &[name, body] : definitions->items()) {
                (void)name;
                if (!body.is_object() || !body.contains("nodes")) continue;
                for (const auto &node : body.at("nodes"))
                    if (node.is_object() && node.contains("parameters"))
                        collectReferences(node.at("parameters"), references);
            }
    }
    return {references.begin(), references.end()};
}

std::filesystem::path graphCanvasProjectFile(const std::filesystem::path &project_root, std::string_view reference) {
    if (!reference.starts_with(project_scheme) || project_root.empty()) return {};
    return project_root / std::filesystem::path{std::string{reference.substr(project_scheme.size())}};
}

GraphCanvasThumbnailJob graphCanvasThumbnailJob(const Json &scope_nodes, const Json &definitions,
                                                const GraphCanvasThumbnailTarget &target,
                                                const Pelican::CanvasValues &live,
                                                const std::filesystem::path &project_root) {
    GraphCanvasThumbnailJob job;
    job.target = target;
    job.made = graphCanvasThumbnailDocument(scope_nodes, definitions, target);
    Json key{{"pixels", target.pixels}, {"kind", static_cast<int>(target.kind)}};
    if (!job.made.error.empty()) {
        key["error"] = job.made.error;
        job.digest = Pelican::renderConfigSourceDigest(key.dump());
        return job;
    }
    std::set<std::string> held;
    for (const auto &node : job.made.document.at("nodes")) held.insert(text(node, "id"));
    Json values = Json::array();
    for (const auto &[source, value] : live)
        if (held.contains(source.node)) {
            job.live.emplace(source, value);
            values.push_back(Json::array({source.node, source.port, value}));
        }
    Json files = Json::array();
    for (const auto &reference : graphCanvasThumbnailReferences(job.made.document)) {
        GraphCanvasThumbnailFile file;
        file.reference = reference;
        const auto path = graphCanvasProjectFile(project_root, reference);
        std::error_code error;
        if (!path.empty() && std::filesystem::is_regular_file(path, error)) {
            file.exists = true;
            file.size = std::filesystem::file_size(path, error);
            const auto time = std::filesystem::last_write_time(path, error);
            if (!error) file.modified = static_cast<std::int64_t>(time.time_since_epoch().count());
        }
        files.push_back(Json::array({file.reference, file.exists, file.size, file.modified}));
        job.files.push_back(std::move(file));
    }
    key["document"] = job.made.document;
    key["output"] = job.made.output;
    key["values"] = std::move(values);
    key["files"] = std::move(files);
    // The project root only matters through the files it holds.
    job.digest = Pelican::renderConfigSourceDigest(key.dump());
    return job;
}

std::array<std::uint32_t, 2> graphCanvasThumbnailExtent(std::array<double, 2> size, std::uint32_t pixels) {
    if (!(size[0] > 0.0) || !(size[1] > 0.0) || pixels == 0) return {pixels, pixels};
    const double scale = static_cast<double>(pixels) / std::max(size[0], size[1]);
    const auto side = [&](double length) {
        return std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(length * scale)), 1, pixels);
    };
    return {side(size[0]), side(size[1])};
}

Pelican::CanvasValues graphCanvasThumbnailValues(const Pelican::CanvasOutput &output,
                                                 const Pelican::CanvasValues &live) {
    Pelican::CanvasValues values;
    const auto take = [&](const Pelican::CanvasValueSource &source, double rest) {
        if (values.contains(source)) return;
        const auto found = live.find(source);
        values.emplace(source, found != live.end() ? found->second : rest);
    };
    const auto parameter = [&](const Pelican::CanvasParameter &number) {
        if (number.edge) take(*number.edge, number.value);
    };
    eachItem(output, [&](const Pelican::CanvasItem &item) {
        for (const auto &number : item.parameters) parameter(number);
        for (const auto &edge : item.opacity_edges) take(edge, 1.0);
        for (const auto &filter : item.filters)
            for (std::size_t at = 0; at < filter.edges.size(); ++at)
                if (filter.edges[at])
                    take(*filter.edges[at], at < filter.constants.size() ? filter.constants[at] : 0.0);
        if (item.depth_edge) parameter(*item.depth_edge);
    });
    for (const auto &blur : output.blurs) parameter(blur.radius);
    return values;
}

std::optional<std::array<double, 4>> graphCanvasDrawnBounds(const Pelican::CanvasOutput &output,
                                                            const Pelican::CanvasValues &values) {
    const auto box = itemsBox(output, output.items, values, 0);
    if (!box) return std::nullopt;
    return std::array<double, 4>{box->x0, box->y0, box->x1 - box->x0, box->y1 - box->y0};
}

std::optional<std::array<double, 4>> graphCanvasThumbnailCrop(const std::array<double, 4> &drawn,
                                                             std::array<double, 2> size) {
    const Box canvas{0.0, 0.0, size[0], size[1]};
    if (canvas.empty()) return std::nullopt;
    const Box seen = Box{drawn[0], drawn[1], drawn[0] + drawn[2], drawn[1] + drawn[3]}.meet(canvas);
    if (seen.empty()) return std::nullopt;
    // A margin of 4 % of the long side all round (at the picture's size, more
    // than the antialiasing's pixel past an edge).
    const double margin = 0.04 * std::max(seen.x1 - seen.x0, seen.y1 - seen.y0);
    const Box cut = Box{seen.x0 - margin, seen.y0 - margin, seen.x1 + margin, seen.y1 + margin}.meet(canvas);
    if (cut.empty() || (cut.x0 <= 0.0 && cut.y0 <= 0.0 && cut.x1 >= size[0] && cut.y1 >= size[1]))
        return std::nullopt;
    return std::array<double, 4>{cut.x0, cut.y0, cut.x1 - cut.x0, cut.y1 - cut.y0};
}

Json graphCanvasCroppedDocument(const Json &document, const std::string &output, const std::array<double, 4> &crop) {
    Json cropped = document;
    auto &nodes = cropped.at("nodes");
    Json *out = nullptr;
    for (auto &node : nodes)
        if (text(node, "id") == output) out = &node;
    if (out == nullptr || !out->contains("inputs") || !out->at("inputs").contains("layer")) return cropped;
    const Json read = out->at("inputs").at("layer");
    std::string space;
    if (const Json *stack = findNode(nodes, read.value("node", std::string{})); stack != nullptr)
        space = text(stack->value("parameters", Json::object()), "blend_space");
    const auto move = unusedId(nodes, "pelican_thumbnail_crop_move");
    const auto cut = unusedId(nodes, "pelican_thumbnail_crop");
    Json parameters{{"size", Json::array({crop[2], crop[3]})}, {"layers", Json::array({Json{{"input", "thumbnail"}}})}};
    if (!space.empty()) parameters["blend_space"] = space;
    (*out)["inputs"]["layer"] = edgeTo(cut, "layer");
    nodes.push_back(Json{{"id", move},
                         {"op", "canvas.transform"},
                         {"parameters", {{"translate", Json::array({-crop[0], -crop[1]})}}},
                         {"inputs", {{"layer", read}}},
                         {"outputs", Json::array({"layer"})}});
    nodes.push_back(Json{{"id", cut},
                         {"op", "canvas.stack"},
                         {"parameters", std::move(parameters)},
                         {"inputs", {{"thumbnail", edgeTo(move, "layer")}}},
                         {"outputs", Json::array({"layer"})}});
    return cropped;
}

GraphCanvasThumbnailPixels graphCanvasThumbnailBytes(const Pelican::CanvasImage &image) {
    GraphCanvasThumbnailPixels pixels;
    pixels.width = image.width;
    pixels.height = image.height;
    pixels.rgba.resize(std::size_t{image.width} * image.height * 4);
    const auto encode = [](double c) {
        c = std::clamp(c, 0.0, 1.0);
        return c > 0.0031308 ? 1.055 * std::pow(c, 1.0 / 2.4) - 0.055 : c * 12.92;
    };
    const auto byte = [](double c) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
    };
    for (std::size_t at = 0; at < std::size_t{image.width} * image.height; ++at) {
        const double alpha = std::clamp(static_cast<double>(image.pixels[at * 4 + 3]), 0.0, 1.0);
        for (std::size_t c = 0; c < 3; ++c) {
            const double premultiplied = image.pixels[at * 4 + c];
            pixels.rgba[at * 4 + c] = alpha > 0.0 ? byte(encode(premultiplied / alpha)) : 0;
        }
        pixels.rgba[at * 4 + 3] = byte(alpha);
    }
    return pixels;
}

GraphCanvasThumbnailPixels renderGraphCanvasThumbnail(const GraphCanvasThumbnailJob &job,
                                                      const std::filesystem::path &project_root,
                                                      const GraphCanvasImageDecoder &decode) {
    GraphCanvasThumbnailPixels failed;
    if (!job.made.error.empty()) {
        failed.error = job.made.error;
        return failed;
    }
    try {
        const Pelican::CanvasAssetReader read = [&](std::string_view reference)
            -> std::shared_ptr<const std::vector<std::byte>> {
            const auto path = graphCanvasProjectFile(project_root, reference);
            if (path.empty()) return nullptr;
            std::ifstream in(path, std::ios::binary);
            if (!in) return nullptr;
            const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            const auto *begin = reinterpret_cast<const std::byte *>(bytes.data());
            return std::make_shared<const std::vector<std::byte>>(begin, begin + bytes.size());
        };
        Pelican::CanvasReferenceImages images;
        // The document's output lowered, with the pictures its items sample
        // decoded (once for both drawings), or why it cannot be drawn.
        const auto lower = [&](const Json &document, Pelican::CanvasIr &ir) -> std::string {
            ir = Pelican::buildCanvasIr(document, read);
            const auto found = std::ranges::find_if(ir.outputs, [&](const Pelican::CanvasOutput &output) {
                return output.scope.empty() && output.id == job.made.output;
            });
            if (found == ir.outputs.end()) return "The canvas lowering drew nothing for it";
            std::string refusal;
            eachItem(*found, [&](const Pelican::CanvasItem &item) {
                eachTexture(item, [&](const Pelican::CanvasTexture &texture) {
                    if (!refusal.empty()) return;
                    using Origin = Pelican::CanvasTexture::Origin;
                    if (texture.origin == Origin::graph) {
                        refusal = "It draws a rendered image (" + texture.node.value_or(std::string{}) + "." +
                                  texture.port + "), which is only seen while the engine draws";
                        return;
                    }
                    if (texture.origin != Origin::file || images.contains(texture.asset)) return;
                    const auto path = graphCanvasProjectFile(project_root, texture.asset);
                    std::optional<Pelican::CanvasReferenceImage> image;
                    if (!path.empty() && decode) image = decode(path);
                    if (!image || image->width == 0 || image->height == 0 ||
                        image->rgba.size() != std::size_t{image->width} * image->height * 4) {
                        refusal = "The picture " + texture.asset + " cannot be read";
                        return;
                    }
                    images.emplace(texture.asset, std::move(*image));
                });
            });
            if (!refusal.empty()) return refusal;
            // Kept first in the list, so the caller finds it there.
            if (found != ir.outputs.begin()) std::iter_swap(found, ir.outputs.begin());
            return {};
        };
        Pelican::CanvasIr ir;
        if (auto refusal = lower(job.made.document, ir); !refusal.empty()) {
            failed.error = std::move(refusal);
            return failed;
        }
        // NB-4: a layer's or a card's picture is cut to where it draws, and
        // that part alone is drawn at the picture's size.
        std::optional<std::array<double, 4>> crop;
        if (job.target.kind != GraphCanvasThumbnailKind::composite) {
            const auto &output = ir.outputs.front();
            if (const auto drawn = graphCanvasDrawnBounds(output, graphCanvasThumbnailValues(output, job.live)))
                crop = graphCanvasThumbnailCrop(*drawn, job.made.size);
        }
        if (crop) {
            Pelican::CanvasIr cropped;
            if (auto refusal = lower(graphCanvasCroppedDocument(job.made.document, job.made.output, *crop), cropped);
                !refusal.empty()) {
                failed.error = std::move(refusal);
                return failed;
            }
            ir = std::move(cropped);
        }
        const auto &output = ir.outputs.front();
        const auto extent =
            graphCanvasThumbnailExtent(crop ? std::array<double, 2>{(*crop)[2], (*crop)[3]} : job.made.size,
                                       job.target.pixels);
        const auto image = Pelican::renderCanvasReference(output, extent[0], extent[1],
                                                          graphCanvasThumbnailValues(output, job.live), images);
        auto pixels = graphCanvasThumbnailBytes(image);
        pixels.crop = crop;
        return pixels;
    } catch (const Pelican::GraphRegistryError &error) {
        failed.error = refusalText(error);
    } catch (const std::exception &error) {
        failed.error = error.what();
    }
    return failed;
}

} // namespace PelicanStudio
