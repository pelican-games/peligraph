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
        const auto ir = Pelican::buildCanvasIr(job.made.document, read);
        const auto found = std::ranges::find_if(ir.outputs, [&](const Pelican::CanvasOutput &output) {
            return output.scope.empty() && output.id == job.made.output;
        });
        if (found == ir.outputs.end()) {
            failed.error = "The canvas lowering drew nothing for it";
            return failed;
        }
        const auto &output = *found;
        Pelican::CanvasReferenceImages images;
        std::string refusal;
        eachItem(output, [&](const Pelican::CanvasItem &item) {
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
        if (!refusal.empty()) {
            failed.error = refusal;
            return failed;
        }
        const auto extent = graphCanvasThumbnailExtent(job.made.size, job.target.pixels);
        const auto image = Pelican::renderCanvasReference(output, extent[0], extent[1],
                                                          graphCanvasThumbnailValues(output, job.live), images);
        return graphCanvasThumbnailBytes(image);
    } catch (const Pelican::GraphRegistryError &error) {
        failed.error = refusalText(error);
    } catch (const std::exception &error) {
        failed.error = error.what();
    }
    return failed;
}

} // namespace PelicanStudio
