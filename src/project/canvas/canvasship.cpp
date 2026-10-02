#include "canvasship.hpp"

#include "../graphcomposition.hpp"
#include "canvasmodule.hpp"

#include <picosha2.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {
using Json = nlohmann::json;

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

template <typename Visit> void eachEndpoint(const Json &inputs, Visit visit) {
    if (!inputs.is_object()) return;
    for (const auto &[port, endpoint] : inputs.items()) {
        (void)port;
        if (endpoint.is_object()) visit(endpoint);
        else if (endpoint.is_array())
            for (const auto &entry : endpoint)
                if (entry.is_object()) visit(entry);
    }
}

Json layerName(const Json &layer, std::size_t index) {
    const auto name = layer.find("name");
    return name != layer.end() && name->is_string() ? *name : Json(index);
}

std::string nameText(const Json &name) { return name.is_string() ? name.get<std::string>() : name.dump(); }

// The nodes of one scope (the root or a definition body), by id and in order.
struct Scope {
    std::string name;
    std::map<std::string, const Json *> nodes;
    std::vector<const Json *> order;

    const Json *find(const std::string &id) const {
        const auto found = nodes.find(id);
        return found == nodes.end() ? nullptr : found->second;
    }
    const Json *producer(const Json &endpoint) const {
        if (!endpoint.is_object()) return nullptr;
        const auto node = endpoint.find("node");
        return node == endpoint.end() || !node->is_string() ? nullptr : find(node->get<std::string>());
    }
    // The node a port of `node` reads, or null.
    const Json *input(const Json &node, const std::string &port) const {
        const auto &inputs = objectMember(node, "inputs");
        const auto found = inputs.find(port);
        return found == inputs.end() ? nullptr : producer(*found);
    }
};

Scope indexScope(const std::string &name, const Json &holder) {
    Scope scope{name, {}, {}};
    const auto nodes = holder.find("nodes");
    if (nodes == holder.end() || !nodes->is_array()) return scope;
    for (const auto &node : *nodes)
        if (node.is_object()) {
            scope.nodes.emplace(nodeId(node), &node);
            scope.order.push_back(&node);
        }
    return scope;
}

// The scopes in document order: the root, then each definition by name.
template <typename Visit> void eachScope(const Json &document, Visit visit) {
    visit(std::string{}, document);
    const auto definitions = document.find("definitions");
    if (definitions == document.end() || !definitions->is_object()) return;
    for (const auto &[name, body] : definitions->items())
        if (body.is_object()) visit(name, body);
}

// One entry of a stack's `layers`, as far as the cut needs it.
struct Entry {
    const Json *layer = nullptr;
    std::size_t index = 0;
    std::string input;
    std::optional<std::string> mask;
    bool visible = true;
    bool clip = false;
    bool pass_through = false;
    bool normal = true;
    bool bake = false;
};

// A base and the clip layers right above it: layers[first..last].
struct Unit {
    std::size_t first = 0;
    std::size_t last = 0;
    bool empty = false;
    bool still = false;
    bool vector = false;
    bool reads = false;
    bool bake = false;
    bool bare_image = false;
};

std::optional<bool> flag(const Json &layer, const char *key) {
    const auto found = layer.find(key);
    if (found == layer.end()) return false;
    if (!found->is_boolean()) return std::nullopt;
    return found->get<bool>();
}

// The stack's entries, or nothing when they are not well formed (the
// lowering refuses those by name).
std::optional<std::vector<Entry>> parseEntries(const Json &stack) {
    const auto &parameters = objectMember(stack, "parameters");
    const auto layers = parameters.find("layers");
    if (layers == parameters.end() || !layers->is_array()) return std::nullopt;
    std::vector<Entry> entries;
    for (std::size_t index = 0; index < layers->size(); ++index) {
        const auto &layer = layers->at(index);
        if (!layer.is_object()) return std::nullopt;
        Entry entry;
        entry.layer = &layer;
        entry.index = index;
        const auto input = layer.find("input");
        if (input == layer.end() || !input->is_string()) return std::nullopt;
        entry.input = input->get<std::string>();
        if (const auto mask = layer.find("mask"); mask != layer.end()) {
            if (!mask->is_string()) return std::nullopt;
            entry.mask = mask->get<std::string>();
        }
        const auto visible = layer.contains("visible") ? flag(layer, "visible") : std::optional<bool>{true};
        const auto clip = flag(layer, "clip");
        const auto pass_through = flag(layer, "pass_through");
        if (!visible || !clip || !pass_through) return std::nullopt;
        entry.visible = *visible;
        entry.clip = *clip;
        entry.pass_through = *pass_through;
        if (const auto blend = layer.find("blend"); blend != layer.end()) entry.normal = *blend == "normal";
        entry.bake = layer.contains("bake");
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::array<double, 2> stackSize(const Json &stack) {
    const auto &parameters = objectMember(stack, "parameters");
    const auto size = parameters.find("size");
    if (size == parameters.end() || !size->is_array() || size->size() != 2 || !size->at(0).is_number() ||
        !size->at(1).is_number())
        return {0.0, 0.0};
    return {size->at(0).get<double>(), size->at(1).get<double>()};
}

double bakeScale(const Json &stack) {
    const auto &parameters = objectMember(stack, "parameters");
    const auto scale = parameters.find("bake_scale");
    if (scale == parameters.end()) return 1.0;
    if (!scale->is_number() || !std::isfinite(scale->get<double>()) || !(scale->get<double>() > 0.0))
        throw std::invalid_argument("canvas.stack bake_scale must be positive: " + nodeId(stack));
    return scale->get<double>();
}

std::string uniqueName(std::string base, const std::function<bool(const std::string &)> &taken) {
    while (taken(base)) base += "_";
    return base;
}

class Planner {
  public:
    Planner(const Scope &scope, const CanvasAssetReader &read_asset, CanvasFlattenPlan &plan)
        : scope_(scope), read_asset_(read_asset), plan_(plan) {
        for (const auto *node : scope_.order) {
            if (nodeOp(*node) == "canvas.place") {
                eachEndpoint(objectMember(*node, "inputs"), [&](const Json &endpoint) { markPlaced(endpoint); });
                plan_.diagnostics.push_back(
                    {{"code", "canvas.flatten_skipped"}, {"reason", "placed"}, {"node", nodeId(*node)}});
            }
            // A stack under a pass-through layer is drawn into its parent's list.
            if (nodeOp(*node) != "canvas.stack") continue;
            const auto entries = parseEntries(*node);
            if (!entries) continue;
            for (const auto &entry : *entries)
                if (entry.pass_through)
                    if (const auto *nested = layerStack(scope_.input(*node, entry.input), true))
                        pass_through_.insert(nodeId(*nested));
        }
    }

    void output(const Json &node) {
        const auto *stack = scope_.input(node, "layer");
        if (stack == nullptr || nodeOp(*stack) != "canvas.stack") return;
        cut(*stack, nodeId(node), false);
    }

  private:
    const Scope &scope_;
    const CanvasAssetReader &read_asset_;
    CanvasFlattenPlan &plan_;
    std::set<std::string> placed_;
    std::set<std::string> pass_through_;
    std::set<std::string> visited_;
    std::map<std::string, bool> still_;
    std::map<std::string, bool> vector_;
    std::set<std::string> walking_;

    void markPlaced(const Json &endpoint) {
        const auto *node = scope_.producer(endpoint);
        if (node == nullptr || !isCanvasGraphOperation(nodeOp(*node)) || !placed_.insert(nodeId(*node)).second) return;
        eachEndpoint(objectMember(*node, "inputs"), [&](const Json &inner) { markPlaced(inner); });
    }

    // The stack a layer is, through canvas.transform (and with `filters`
    // colour filters too); null when it is not one.
    const Json *layerStack(const Json *node, bool filters) const {
        for (std::size_t depth = 0; node != nullptr && depth < scope_.nodes.size(); ++depth) {
            const auto op = nodeOp(*node);
            if (op == "canvas.stack") return node;
            if (op != "canvas.transform" && !(filters && op.starts_with("filter."))) return nullptr;
            node = scope_.input(*node, "layer");
        }
        return nullptr;
    }

    // Nothing under the node moves: canvas nodes all the way down, no edge
    // from outside the canvas, no image of another pass.
    bool still(const Json *node) {
        if (node == nullptr) return false;
        const auto id = nodeId(*node);
        if (const auto found = still_.find(id); found != still_.end()) return found->second;
        if (!walking_.insert(id).second) return false;
        const auto op = nodeOp(*node);
        bool result = isCanvasGraphOperation(op) && op != "canvas.from_image" && op != "canvas.place" &&
                      op != "canvas.output";
        if (result)
            eachEndpoint(objectMember(*node, "inputs"),
                         [&](const Json &endpoint) { result = result && still(scope_.producer(endpoint)); });
        walking_.erase(id);
        still_[id] = result;
        return result;
    }

    // Shapes and text alone: cheaper drawn from their distances than read
    // from an image, and sharp at any scale.
    bool vector(const Json *node) {
        if (node == nullptr) return false;
        const auto id = nodeId(*node);
        if (const auto found = vector_.find(id); found != vector_.end()) return found->second;
        if (!walking_.insert(id).second) return false;
        const auto op = nodeOp(*node);
        bool result = op.starts_with("shape.") || op == "canvas.color" || op == "canvas.transform" ||
                      op == "canvas.stack";
        if (result)
            eachEndpoint(objectMember(*node, "inputs"),
                         [&](const Json &endpoint) { result = result && vector(scope_.producer(endpoint)); });
        walking_.erase(id);
        vector_[id] = result;
        return result;
    }

    // The layer puts an item that reads its backdrop into the list it joins:
    // a canvas.backdrop, or a blend inside a pass-through group.
    bool readsBackdrop(const Json *node, bool pass_through, std::size_t depth = 0) {
        if (node == nullptr || depth > scope_.nodes.size()) return false;
        const auto op = nodeOp(*node);
        if (op == "canvas.backdrop") return true;
        if (op == "canvas.transform" || op.starts_with("filter."))
            return readsBackdrop(scope_.input(*node, "layer"), pass_through, depth + 1);
        if (op != "canvas.stack" || !pass_through) return false;
        const auto entries = parseEntries(*node);
        if (!entries) return true;
        for (const auto &entry : *entries) {
            if (!entry.visible) continue;
            if (!entry.normal || readsBackdrop(scope_.input(*node, entry.input), entry.pass_through, depth + 1))
                return true;
        }
        return false;
    }

    bool entryStill(const Json &stack, const Entry &entry) {
        const auto &inputs = objectMember(stack, "inputs");
        if (inputs.contains(canvasStackOpacityPort(entry.input))) return false;
        if (!still(scope_.input(stack, entry.input))) return false;
        return !entry.mask || still(scope_.input(stack, *entry.mask));
    }

    std::vector<Unit> units(const Json &stack, const std::vector<Entry> &entries) {
        std::vector<Unit> result;
        for (std::size_t at = 0; at < entries.size();) {
            Unit unit;
            unit.first = at;
            auto end = at + 1;
            while (end < entries.size() && entries[end].clip) ++end;
            unit.last = end - 1;
            const auto &base = entries[at];
            unit.empty = !base.visible;
            unit.still = true;
            unit.vector = true;
            bool clipped = false;
            for (auto k = at; k < end; ++k) {
                const auto &entry = entries[k];
                unit.bake = unit.bake || entry.bake;
                if (!entry.visible || unit.empty) continue;
                if (k > at) clipped = true;
                unit.still = unit.still && entryStill(stack, entry);
                unit.vector = unit.vector && !entry.mask && vector(scope_.input(stack, entry.input));
            }
            unit.reads = !base.normal || (!clipped && readsBackdrop(scope_.input(stack, base.input), base.pass_through));
            const auto *source = scope_.input(stack, base.input);
            unit.bare_image = !clipped && !base.mask && source != nullptr && nodeOp(*source) == "canvas.image";
            result.push_back(unit);
            at = end;
        }
        return result;
    }

    // Cuts the layers of a stack the output composites on its own.
    void cut(const Json &stack, const std::string &output, bool group) {
        const auto id = nodeId(stack);
        if (placed_.contains(id) || pass_through_.contains(id) || !visited_.insert(id).second) return;
        const auto size = stackSize(stack);
        if (!(size[0] > 0.0) || !(size[1] > 0.0)) return;
        const auto entries = parseEntries(stack);
        if (!entries || entries->empty() || entries->front().clip) return;
        const auto list = units(stack, *entries);
        std::vector<bool> folded(list.size(), false);
        const auto drawn = static_cast<std::size_t>(
            std::count_if(list.begin(), list.end(), [](const Unit &unit) { return !unit.empty; }));
        const bool whole =
            drawn > 0 && std::all_of(list.begin(), list.end(), [](const Unit &unit) { return unit.empty || unit.still; });
        if (whole) {
            std::vector<std::size_t> all;
            for (std::size_t k = 0; k < list.size(); ++k) all.push_back(k);
            propose(stack, output, group, *entries, list, all, "static_unit", folded);
        } else {
            std::vector<std::size_t> run;
            bool below = false;
            const auto close = [&] {
                while (!run.empty() && list[run.back()].empty) run.pop_back();
                if (!run.empty())
                    propose(stack, output, group, *entries, list, run,
                            below ? "static_above_cut" : "static_from_bottom", folded);
                run.clear();
            };
            for (std::size_t k = 0; k < list.size(); ++k) {
                const auto &unit = list[k];
                if (unit.empty) {
                    if (!run.empty()) run.push_back(k);
                    continue;
                }
                if (unit.still && !unit.reads) {
                    run.push_back(k);
                    continue;
                }
                close();
                below = true;
            }
            close();
        }
        // The stacks composited on their own inside what stays.
        for (std::size_t k = 0; k < list.size(); ++k) {
            if (folded[k] || list[k].empty) continue;
            for (auto at = list[k].first; at <= list[k].last; ++at) {
                const auto &entry = (*entries)[at];
                if (!entry.visible || entry.pass_through) continue;
                if (const auto *nested = layerStack(scope_.input(stack, entry.input), false)) cut(*nested, output, true);
            }
        }
    }

    // A run of units: proposed unless it is shapes alone (without a `bake`)
    // or a lone canvas.image.
    void propose(const Json &stack, const std::string &output, bool group, const std::vector<Entry> &entries,
                 const std::vector<Unit> &list, const std::vector<std::size_t> &run, const std::string &position,
                 std::vector<bool> &folded) {
        std::vector<std::size_t> drawn;
        for (const auto k : run)
            if (!list[k].empty) drawn.push_back(k);
        if (drawn.empty()) return;
        const bool vector_only = std::all_of(drawn.begin(), drawn.end(), [&](std::size_t k) { return list[k].vector; });
        const bool bake = std::any_of(drawn.begin(), drawn.end(), [&](std::size_t k) { return list[k].bake; });
        if (vector_only && !bake) return;
        if (drawn.size() == 1 && list[drawn.front()].bare_image) return;

        CanvasFlattenRun result;
        result.scope = scope_.name;
        result.output = output;
        result.stack = nodeId(stack);
        result.group = group;
        result.first = list[drawn.front()].first;
        result.last = list[drawn.back()].last;
        result.reason = vector_only ? "bake_texture" : position;
        result.size = stackSize(stack);
        const auto scale = bakeScale(stack);
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const auto pixels = std::ceil(result.size[axis] * scale);
            if (pixels > canvas_flatten_pixel_limit)
                throw GraphRegistryError("canvas.flatten_size_exceeded@1",
                                         {{"stack", result.stack},
                                          {"pixels", {std::ceil(result.size[0] * scale), std::ceil(result.size[1] * scale)}},
                                          {"limit", canvas_flatten_pixel_limit}});
            result.pixels[axis] = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(pixels));
        }

        // The stack with the run's layers alone, and the nodes they read.
        Json layers = Json::array();
        std::set<std::string> ports;
        const auto &inputs = objectMember(stack, "inputs");
        for (const auto k : drawn) {
            folded[k] = true;
            for (auto at = list[k].first; at <= list[k].last; ++at) {
                const auto &entry = entries[at];
                layers.push_back(*entry.layer);
                result.layers.push_back(layerName(*entry.layer, entry.index));
                for (const auto &port : {entry.input, canvasStackOpacityPort(entry.input), canvasStackDepthPort(entry.input)})
                    if (inputs.contains(port)) ports.insert(port);
                if (entry.mask && inputs.contains(*entry.mask)) ports.insert(*entry.mask);
            }
        }
        for (const auto k : run)
            if (list[k].empty) folded[k] = true;
        Json run_inputs = Json::object();
        std::set<std::string> closure;
        std::vector<const Json *> pending;
        for (const auto &port : ports) {
            run_inputs[port] = inputs.at(port);
            if (const auto *node = scope_.producer(inputs.at(port))) pending.push_back(node);
        }
        while (!pending.empty()) {
            const auto *node = pending.back();
            pending.pop_back();
            if (!closure.insert(nodeId(*node)).second) continue;
            eachEndpoint(objectMember(*node, "inputs"), [&](const Json &endpoint) {
                if (const auto *inner = scope_.producer(endpoint)) pending.push_back(inner);
            });
        }
        auto nodes = Json::array();
        for (const auto *node : scope_.order)
            if (closure.contains(nodeId(*node))) nodes.push_back(*node);
        auto copy = stack;
        copy["parameters"]["layers"] = std::move(layers);
        copy["inputs"] = std::move(run_inputs);
        nodes.push_back(std::move(copy));
        const auto taken = [&](const std::string &name) {
            return closure.contains(name) || name == result.stack ||
                   std::any_of(closure.begin(), closure.end(),
                               [&](const std::string &id) { return id.starts_with(name + "$canvas_"); });
        };
        result.bake_output = uniqueName("canvas_bake", taken);
        nodes.push_back({{"id", result.bake_output},
                         {"op", "canvas.output"},
                         // The image is the frame's size (the player's --size): a fixed
                         // `size` lowers to a render.image the engine refuses without
                         // an extent_scale beside its width and height.
                         {"parameters", {{"fit", "stretch"}}},
                         {"inputs", {{"layer", {{"node", result.stack}, {"port", "layer"}}}}},
                         {"outputs", Json::array({"image"})}});
        const Json present{{"name", "present"},
                           {"type", "fullscreen"},
                           {"input", Json::array({canvasImageNodeId(result.bake_output)})},
                           {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
                           {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
        result.document = {{"schema", "pelican.graph"},
                           {"version", 1},
                           {"definitions", Json::object()},
                           {"nodes", std::move(nodes)},
                           {"outputs", Json::object()},
                           {"rendering",
                            {{"rendering_passes",
                              Json::array({{{"name", "main_render"}, {"passes", Json::array({present})}}})}}}};
        result.assets = canvasDocumentAssets(result.document);
        std::string hashed = result.document.dump() + '\0' + std::to_string(result.pixels[0]) + "x" +
                             std::to_string(result.pixels[1]);
        for (const auto &asset : result.assets) {
            hashed += '\0' + asset + '\0';
            const auto bytes = read_asset_ ? read_asset_(asset) : nullptr;
            if (bytes == nullptr) {
                hashed += "missing";
            } else {
                const auto *begin = reinterpret_cast<const unsigned char *>(bytes->data());
                hashed += picosha2::hash256_hex_string(begin, begin + bytes->size());
            }
        }
        result.digest = picosha2::hash256_hex_string(hashed).substr(0, 16);
        plan_.runs.push_back(std::move(result));
    }
};

} // namespace

CanvasFlattenPlan planCanvasFlatten(const nlohmann::json &authored, const CanvasAssetReader &read_asset) {
    CanvasFlattenPlan plan;
    if (!documentHasCanvasNodes(authored)) return plan;
    const auto document = expandCanvasInstances(authored);
    eachScope(document, [&](const std::string &name, const Json &holder) {
        const auto scope = indexScope(name, holder);
        Planner planner{scope, read_asset, plan};
        for (const auto *node : scope.order)
            if (nodeOp(*node) == "canvas.output") planner.output(*node);
    });
    return plan;
}

std::string canvasFlattenImagePath(const CanvasFlattenRun &run) { return "canvas_baked/" + run.digest + ".png"; }

nlohmann::json applyCanvasFlatten(const nlohmann::json &authored, const CanvasFlattenPlan &plan) {
    if (plan.runs.empty()) return authored;
    auto document = expandCanvasInstances(authored);
    std::map<std::pair<std::string, std::string>, std::vector<const CanvasFlattenRun *>> stacks;
    for (const auto &run : plan.runs) stacks[{run.scope, run.stack}].push_back(&run);
    for (auto &[where, runs] : stacks) {
        auto &holder = where.first.empty() ? document : document.at("definitions").at(where.first);
        auto &nodes = holder.at("nodes");
        std::set<std::string> ids;
        for (const auto &node : nodes) ids.insert(nodeId(node));
        const auto at = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const Json &node) { return nodeId(node) == where.second; });
        if (at == nodes.end()) throw std::logic_error("canvas flatten: no stack " + where.second);
        const auto position = static_cast<std::size_t>(at - nodes.begin());
        auto stack = *at;
        std::sort(runs.begin(), runs.end(),
                  [](const CanvasFlattenRun *a, const CanvasFlattenRun *b) { return a->first < b->first; });
        for (std::size_t k = 1; k < runs.size(); ++k)
            if (runs[k]->first <= runs[k - 1]->last) throw std::logic_error("canvas flatten: runs overlap in " + where.second);
        auto &layers = stack["parameters"]["layers"];
        auto &inputs = stack["inputs"];
        const auto entryPorts = [](const Json &layer) {
            std::vector<std::string> ports;
            if (const auto input = layer.find("input"); input != layer.end() && input->is_string()) {
                ports.push_back(input->get<std::string>());
                ports.push_back(canvasStackOpacityPort(input->get<std::string>()));
                ports.push_back(canvasStackDepthPort(input->get<std::string>()));
            }
            if (const auto mask = layer.find("mask"); mask != layer.end() && mask->is_string())
                ports.push_back(mask->get<std::string>());
            return ports;
        };
        std::set<std::string> dropped;
        std::vector<Json> images(runs.size());
        for (auto k = runs.size(); k-- > 0;) {
            const auto &run = *runs[k];
            const auto image = uniqueName(where.second + "$baked" + std::to_string(k),
                                          [&](const std::string &name) { return ids.contains(name); });
            ids.insert(image);
            const auto port = uniqueName("baked" + std::to_string(k), [&](const std::string &name) {
                return inputs.contains(name) || std::any_of(layers.begin(), layers.end(), [&](const Json &layer) {
                           return layer.value("input", std::string{}) == name || layer.value("mask", std::string{}) == name;
                       });
            });
            std::string label;
            for (const auto &name : run.layers) label += (label.empty() ? "" : " + ") + nameText(name);
            images[k] = {{"id", image},
                         {"op", "canvas.image"},
                         {"parameters",
                          {{"asset", "project://" + canvasFlattenImagePath(run)},
                           {"offset", {0, 0}},
                           {"size", {run.size[0], run.size[1]}},
                           {"alpha", "premultiplied"}}},
                         {"inputs", Json::object()},
                         {"outputs", Json::array({"layer"})}};
            for (auto entry = run.first; entry <= run.last; ++entry)
                for (auto &name : entryPorts(layers.at(entry))) dropped.insert(std::move(name));
            layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(run.first),
                         layers.begin() + static_cast<std::ptrdiff_t>(run.last + 1));
            layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(run.first),
                          Json{{"input", port}, {"name", label}});
            inputs[port] = {{"node", image}, {"port", "layer"}};
        }
        // The ports only the folded layers read go; a mask another layer
        // shares stays.
        for (const auto &layer : layers)
            for (const auto &name : entryPorts(layer)) dropped.erase(name);
        for (const auto &name : dropped) inputs.erase(name);
        nodes.at(position) = std::move(stack);
        const Json added(images);
        nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(position), added.begin(), added.end());
    }
    return document;
}

std::vector<std::string> canvasDocumentAssets(const nlohmann::json &document) {
    std::set<std::string> assets;
    if (!document.is_object()) return {};
    eachScope(document, [&](const std::string &, const Json &holder) {
        const auto nodes = holder.find("nodes");
        if (nodes == holder.end() || !nodes->is_array()) return;
        for (const auto &node : *nodes) {
            const auto op = nodeOp(node);
            const char *key = op == "canvas.image" ? "asset" : op == "shape.text" ? "font" : nullptr;
            if (key == nullptr) continue;
            const auto &parameters = objectMember(node, "parameters");
            if (const auto found = parameters.find(key); found != parameters.end() && found->is_string())
                assets.insert(found->get<std::string>());
        }
    });
    return {assets.begin(), assets.end()};
}

} // namespace Pelican
