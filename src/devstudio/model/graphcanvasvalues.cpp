// UX-8: a curve onto a shader's value, the curve's keys, a playback's
// fields, a pass's named buffer ports, render.scalar's packed values, and
// the sound nodes' placing -- the parts of GraphCanvasModel GB-4 had to write
// by hand.

#include "graphcanvasmodel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <tuple>
#include <utility>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

std::string textOf(const Json &value, const char *key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

std::string tokenOf(std::string_view scope) {
    return scope.empty() ? std::string{"root"} : std::string{scope};
}

// The value a curve has at `time`, as the engine samples it (linear between
// keys, the step's own value, the ends held). A cubic segment is read as
// linear here: the new key only has to land somewhere reasonable.
double sampleCurve(const std::vector<GraphCanvasCurveKey> &keys, double time) {
    if (keys.empty()) return 0.0;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (std::size_t index = 1; index < keys.size(); ++index) {
        const auto &a = keys[index - 1];
        const auto &b = keys[index];
        if (time > b.time) continue;
        if (a.interpolation == "step") return a.value;
        const double width = b.time - a.time;
        const double t = width > 0.0 ? (time - a.time) / width : 0.0;
        return a.value + (b.value - a.value) * t;
    }
    return keys.back().value;
}

bool isBufferOp(std::string_view op) {
    return op == "render.buffer" || op == "render.attribute";
}

std::size_t scalarComponents(const Json &parameters) {
    const auto value = parameters.is_object() ? parameters.find("value") : parameters.end();
    if (!parameters.is_object() || value == parameters.end() || !value->is_array()) return 1;
    return value->size();
}

std::int64_t scalarOffset(const Json &parameters) {
    const auto offset = parameters.is_object() ? parameters.find("offset") : parameters.end();
    if (!parameters.is_object() || offset == parameters.end() || !offset->is_number_integer())
        return 0;
    return offset->get<std::int64_t>();
}
} // namespace

// --- Decision 4: a value.curve's keys.

const std::vector<std::string> &GraphCanvasModel::curveInterpolations() {
    static const std::vector<std::string> interpolations{"linear", "step", "cubic"};
    return interpolations;
}

std::vector<GraphCanvasCurveKey> GraphCanvasModel::curveKeys(std::string_view scope,
                                                             std::string_view node) const {
    std::vector<GraphCanvasCurveKey> keys;
    const auto *found = findNode(scope, node);
    if (found == nullptr || textOf(*found, "op") != "value.curve") return keys;
    const auto parameters = found->value("parameters", Json::object());
    const auto authored = parameters.is_object() ? parameters.value("keys", Json{}) : Json{};
    if (!authored.is_array()) return keys;
    for (const auto &entry : authored) {
        if (!entry.is_object()) return {};
        const auto time = entry.find("time");
        const auto value = entry.find("value");
        if (time == entry.end() || !time->is_number() || value == entry.end() ||
            !value->is_number())
            return {};
        GraphCanvasCurveKey key;
        key.time = time->get<double>();
        key.value = value->get<double>();
        const auto interpolation = textOf(entry, "interpolation");
        if (!interpolation.empty()) key.interpolation = interpolation;
        for (const auto &[name, member] : entry.items())
            if (name != "time" && name != "value" && name != "interpolation")
                key.rest[name] = member;
        keys.push_back(std::move(key));
    }
    return keys;
}

std::string GraphCanvasModel::curveKeysRefusal(const std::vector<GraphCanvasCurveKey> &keys) {
    if (keys.empty()) return "A curve needs at least one key.";
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto &key = keys[index];
        if (!std::isfinite(key.time) || !std::isfinite(key.value))
            return "Key " + std::to_string(index + 1) + " is not a number.";
        if (std::ranges::find(curveInterpolations(), key.interpolation) ==
            curveInterpolations().end())
            return "Key " + std::to_string(index + 1) + " has an interpolation the engine does not "
                   "take (" + key.interpolation + ").";
        if (index > 0 && !(keys[index - 1].time < key.time))
            return "The keys' times have to rise: key " + std::to_string(index + 1) +
                   " is not later than key " + std::to_string(index) + ".";
    }
    return {};
}

Json GraphCanvasModel::curveKeysJson(const std::vector<GraphCanvasCurveKey> &keys) {
    Json result = Json::array();
    for (const auto &key : keys) {
        Json entry{{"time", key.time}, {"value", key.value}};
        // The engine's default stays unwritten.
        if (key.interpolation != "linear") entry["interpolation"] = key.interpolation;
        if (key.rest.is_object())
            for (const auto &[name, member] : key.rest.items()) entry[name] = member;
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<GraphCanvasCurveKey>
GraphCanvasModel::curveKeysWithMoved(std::vector<GraphCanvasCurveKey> keys, std::size_t index,
                                     double time, double value) {
    if (index >= keys.size() || !std::isfinite(time) || !std::isfinite(value)) return keys;
    // Just short of a neighbour: the times have to stay strictly rising.
    constexpr double gap = 1.0e-3;
    if (index > 0) time = std::max(time, keys[index - 1].time + gap);
    if (index + 1 < keys.size()) time = std::min(time, keys[index + 1].time - gap);
    keys[index].time = time;
    keys[index].value = value;
    return keys;
}

std::vector<GraphCanvasCurveKey>
GraphCanvasModel::curveKeysWithAdded(std::vector<GraphCanvasCurveKey> keys, std::size_t index) {
    GraphCanvasCurveKey added;
    if (keys.empty()) {
        keys.push_back(added);
        return keys;
    }
    index = std::min(index, keys.size() - 1);
    const double time = index + 1 < keys.size() ? (keys[index].time + keys[index + 1].time) / 2.0
                                                : keys[index].time + 1.0;
    added.time = time;
    added.value = sampleCurve(keys, time);
    added.interpolation = keys[index].interpolation;
    keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(added));
    return keys;
}

bool GraphCanvasModel::setCurveKeys(std::string_view scope, const std::string &node,
                                    const std::vector<GraphCanvasCurveKey> &keys) {
    if (!canEdit()) return false;
    const auto *found = findNode(scope, node);
    if (found == nullptr || textOf(*found, "op") != "value.curve") return false;
    if (auto refusal = curveKeysRefusal(keys); !refusal.empty()) {
        edit_notice_ = {InspectorNoticeKind::Error, std::move(refusal)};
        return false;
    }
    auto parameters = nodeParameters(scope, node);
    if (!parameters.is_object()) parameters = Json::object();
    parameters["keys"] = curveKeysJson(keys);
    return setNodeParameters(scope, node, parameters);
}

// --- Decision 5: value.playback.

const std::vector<std::string> &GraphCanvasModel::playbackWraps() {
    static const std::vector<std::string> wraps{"once", "repeat", "ping_pong"};
    return wraps;
}

const std::vector<std::pair<std::string, double>> &GraphCanvasModel::playbackNumbers() {
    // valuegraph's playback keys and CurvePlaybackSettings' defaults.
    static const std::vector<std::pair<std::string, double>> numbers{
        {"duration", 1.0}, {"source_begin", 0.0}, {"source_end", 1.0},
        {"speed", 1.0},    {"delay", 0.0}};
    return numbers;
}

bool GraphCanvasModel::isPlayback(std::string_view op) noexcept {
    return op == "value.playback";
}

double GraphCanvasModel::playbackNumber(std::string_view scope, std::string_view node,
                                        std::string_view key) const {
    double fallback = 0.0;
    for (const auto &[name, value] : playbackNumbers())
        if (name == key) fallback = value;
    const auto parameters = nodeParameters(scope, node);
    if (!parameters.is_object()) return fallback;
    const auto found = parameters.find(std::string{key});
    return found != parameters.end() && found->is_number() ? found->get<double>() : fallback;
}

bool GraphCanvasModel::playbackDriven(std::string_view scope, std::string_view node,
                                      std::string_view key) const {
    const auto *found = findNode(scope, node);
    if (found == nullptr) return false;
    const auto inputs = found->value("inputs", Json::object());
    return inputs.is_object() && inputs.contains(std::string{key}) &&
           inputs.at(std::string{key}).is_object();
}

bool GraphCanvasModel::setPlaybackField(std::string_view scope, const std::string &node,
                                        const std::string &key, const Json &value) {
    const auto *found = findNode(scope, node);
    if (found == nullptr || !isPlayback(textOf(*found, "op"))) return false;
    if (key == "wrap") {
        if (!value.is_string() ||
            std::ranges::find(playbackWraps(), value.get<std::string>()) == playbackWraps().end())
            return false;
    } else {
        const bool known = std::ranges::any_of(playbackNumbers(),
                                               [&](const auto &entry) { return entry.first == key; });
        if (!known || !value.is_number() || !std::isfinite(value.get<double>())) return false;
        if (key == "duration" && !(value.get<double>() > 0.0)) {
            edit_notice_ = {InspectorNoticeKind::Error, "A playback's duration has to be above 0."};
            return false;
        }
        if (playbackDriven(scope, node, key)) return false;
    }
    auto parameters = nodeParameters(scope, node);
    if (!parameters.is_object()) parameters = Json::object();
    parameters[key] = value;
    return setNodeParameters(scope, node, parameters);
}

// --- Decision 2: a pass's named buffer ports.

std::vector<std::string> GraphCanvasModel::bufferPorts(std::string_view scope,
                                                       std::string_view node) const {
    std::vector<std::string> ports;
    const auto parameters = nodeParameters(scope, node);
    const auto declared = parameters.is_object() ? parameters.value("ports", Json::object())
                                                 : Json::object();
    if (!declared.is_object()) return ports;
    for (const auto &[name, port] : declared.items())
        if (port.is_object() && textOf(port, "kind") == "buffer" && !port.contains("binding"))
            ports.push_back(name);
    std::ranges::sort(ports);
    return ports;
}

std::string GraphCanvasModel::bufferElementOf(std::string_view scope, std::string_view node) const {
    const auto *found = findNode(scope, node);
    if (found == nullptr && !scope.empty()) found = findNode({}, node);
    if (found == nullptr || !isBufferOp(textOf(*found, "op"))) return {};
    const auto parameters = found->value("parameters", Json::object());
    return parameters.is_object() ? textOf(parameters, "element") : std::string{};
}

std::string GraphCanvasModel::bufferPortName(std::string_view declaration) {
    // The port header's variable is pelican_resource_<port> (the engine
    // checks the name at the port's binding): its port is what follows.
    if (declaration.starts_with(kResourcePortVariablePrefix))
        declaration.remove_prefix(kResourcePortVariablePrefix.size());
    std::string name;
    for (const char c : declaration) {
        const auto u = static_cast<unsigned char>(c);
        name.push_back(std::isalnum(u) || c == '_' ? c : '_');
    }
    while (!name.empty() && name.front() == '_') name.erase(name.begin());
    while (!name.empty() && name.back() == '_') name.pop_back();
    if (name.empty()) return "buffer";
    if (std::isdigit(static_cast<unsigned char>(name.front()))) name.insert(0, "b_");
    return name;
}

Json GraphCanvasModel::bufferPortOps(std::string_view scope, const std::string &node,
                                     const std::string &port, const std::string &source_node,
                                     const std::string &source_port, std::string *refusal) const {
    const auto refuse = [&](std::string line) {
        if (refusal != nullptr) *refusal = std::move(line);
        return Json::array();
    };
    const auto *target = findNode(scope, node);
    if (target == nullptr) return Json::array();
    const auto op = textOf(*target, "op");
    if (op != "render.fullscreen" && op != "render.raster") return Json::array();
    if (port.empty() || source_node.empty() || source_port.empty()) return Json::array();
    const auto element = bufferElementOf(scope, source_node);
    if (element.empty())
        return refuse(source_node + " hands on no buffer: a buffer port reads a render.buffer "
                                    "or a render.attribute.");
    auto parameters = nodeParameters(scope, node);
    if (!parameters.is_object()) parameters = Json::object();
    auto &ports = parameters["ports"];
    if (!ports.is_object()) ports = Json::object();
    if (const auto existing = ports.find(port); existing != ports.end()) {
        if (!existing->is_object() || textOf(*existing, "kind") != "buffer")
            return refuse(node + " already has a port named " + port + " that is no buffer.");
    }
    const auto token = tokenOf(scope);
    const Json source{{"node", source_node}, {"port", source_port}};
    Json ops = Json::array();
    Json declaration{{"kind", "buffer"}, {"access", "read"}, {"element", element}};
    if (ports.value(port, Json{}) != declaration) {
        // Whatever else the port said (a hand-written binding) goes: a
        // pass's buffer input is read by its position after the images.
        ports[port] = std::move(declaration);
        ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", node},
                           {"parameters", std::move(parameters)}});
    }
    const auto inputs = target->value("inputs", Json::object());
    if (!inputs.is_object() || inputs.value(port, Json{}) != source)
        ops.push_back(Json{{"op", "set_input"}, {"scope", token}, {"node", node}, {"port", port},
                           {"source", source}});
    return ops;
}

bool GraphCanvasModel::addBufferPort(std::string_view scope, const std::string &node,
                                     const std::string &port, const std::string &source_node,
                                     const std::string &source_port) {
    if (!canEdit()) return false;
    std::string refusal;
    auto ops = bufferPortOps(scope, node, port, source_node, source_port, &refusal);
    if (ops.empty()) {
        if (!refusal.empty()) edit_notice_ = {InspectorNoticeKind::Error, std::move(refusal)};
        return false;
    }
    return submitOps(std::move(ops));
}

std::vector<std::string> GraphCanvasModel::bufferSources(std::string_view scope) const {
    std::vector<std::string> result;
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return result;
    for (const auto &node : *nodes)
        if (node.is_object() && isBufferOp(textOf(node, "op"))) result.push_back(textOf(node, "id"));
    return result;
}

// --- Decision 7: the voices in a scope.

std::vector<std::string> GraphCanvasModel::voiceChoices(std::string_view scope) const {
    std::vector<std::string> result;
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return result;
    for (const auto &node : *nodes)
        if (node.is_object() && isAudioPlayback(textOf(node, "op")))
            result.push_back(textOf(node, "id"));
    return result;
}

// --- Decision 6: what keeps the Timeline from reading.

std::string GraphCanvasModel::timelineReason() const {
    if (!connected_) return "No engine is running.";
    if (timeline_node_.empty()) return "Pick a node to follow over time.";
    if (!currentScope().empty() && values_instance_.empty())
        return "A definition has no values of its own: pick the instance to read them through in "
               "Values from.";
    if (!live_) return "Turn Live on to read the values while the engine runs.";
    if (node_values_received_ && !node_values_published_)
        return "The engine has not published values for this graph yet.";
    if (node_values_received_ && node_values_published_ &&
        nodeOutputs(currentScope(), timeline_node_) == nullptr)
        return timeline_node_ + " publishes no value (a sink or a render node is not read).";
    return {};
}

// --- Decision 10: a definition with nothing in it.

Json GraphCanvasModel::newDefinitionOps(const std::string &name) const {
    if (name.empty() || definitions_.contains(name) || !definitionNameRefusal({}, name).empty())
        return Json::array();
    return Json::array({Json{{"op", "add_definition"},
                             {"name", name},
                             {"definition", Json{{"nodes", Json::array()},
                                                 {"outputs", Json::object()}}}}});
}

bool GraphCanvasModel::addDefinition(const std::string &name) {
    if (!canEdit()) return false;
    if (auto refusal = definitionNameRefusal({}, name); !refusal.empty()) {
        edit_notice_ = {InspectorNoticeKind::Error, std::move(refusal)};
        return false;
    }
    auto ops = newDefinitionOps(name);
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

// --- Decision 12 (b): render.scalar.

std::vector<double> GraphCanvasModel::scalarValue(std::string_view scope,
                                                  std::string_view node) const {
    const auto parameters = nodeParameters(scope, node);
    const auto value = parameters.is_object() ? parameters.find("value") : parameters.end();
    if (!parameters.is_object() || value == parameters.end() || !value->is_array())
        return {0.0};
    std::vector<double> result;
    for (const auto &component : *value)
        result.push_back(component.is_number() ? component.get<double>() : 0.0);
    return result;
}

std::vector<std::pair<std::string, std::int64_t>>
GraphCanvasModel::scalarLayout(std::string_view scope, const std::string &target,
                               const std::map<std::string, std::size_t> &components) const {
    struct Sink {
        std::string id;
        std::int64_t offset = 0;
        std::size_t components = 1;
    };
    std::vector<Sink> sinks;
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr || target.empty()) return {};
    for (const auto &node : *nodes) {
        if (!node.is_object() || textOf(node, "op") != "render.scalar") continue;
        const auto parameters = node.value("parameters", Json::object());
        if (!parameters.is_object() || textOf(parameters, "target") != target) continue;
        Sink sink{textOf(node, "id"), scalarOffset(parameters), scalarComponents(parameters)};
        if (const auto given = components.find(sink.id); given != components.end())
            sink.components = given->second;
        sinks.push_back(std::move(sink));
    }
    // The order they stand in now (offset, then id), packed from byte 0.
    std::ranges::stable_sort(sinks, [](const Sink &a, const Sink &b) {
        return std::tie(a.offset, a.id) < std::tie(b.offset, b.id);
    });
    std::vector<std::pair<std::string, std::int64_t>> layout;
    std::int64_t offset = 0;
    for (const auto &sink : sinks) {
        layout.emplace_back(sink.id, offset);
        offset += static_cast<std::int64_t>(sink.components * sizeof(float));
    }
    return layout;
}

namespace {
// The parameters a render.scalar is left with: one component on the edge
// (no `value`), else the numbers; an offset of 0 left out.
Json scalarParameters(Json parameters, const std::vector<double> *value, std::int64_t offset) {
    if (!parameters.is_object()) parameters = Json::object();
    if (value != nullptr) {
        if (value->size() <= 1) parameters.erase("value");
        else parameters["value"] = *value;
    }
    if (offset == 0) parameters.erase("offset");
    else parameters["offset"] = offset;
    return parameters;
}
} // namespace

bool GraphCanvasModel::setScalarValue(std::string_view scope, const std::string &node,
                                      const std::vector<double> &value) {
    if (!canEdit()) return false;
    const auto *found = findNode(scope, node);
    if (found == nullptr || textOf(*found, "op") != "render.scalar") return false;
    if (value.empty() || value.size() > 4) return false;
    const auto token = tokenOf(scope);
    const auto parameters = nodeParameters(scope, node);
    const auto target = parameters.is_object() ? textOf(parameters, "target") : std::string{};
    const auto layout = scalarLayout(scope, target, {{node, value.size()}});
    Json ops = Json::array();
    auto removed_inputs = Json::object();
    for (const auto &[id, offset] : layout) {
        const auto before = nodeParameters(scope, id);
        const auto after = scalarParameters(before, id == node ? &value : nullptr, offset);
        if (after != before)
            ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", id},
                               {"parameters", after}});
    }
    if (layout.empty()) {
        const auto after = scalarParameters(parameters, &value, scalarOffset(parameters));
        if (after != parameters)
            ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", node},
                               {"parameters", after}});
    }
    // Numbers written on the node take the place of the edge: the engine
    // reads no `value` edge beside them.
    if (value.size() > 1) {
        const auto inputs = found->value("inputs", Json::object());
        if (inputs.is_object() && inputs.contains("value"))
            ops.push_back(Json{{"op", "set_input"}, {"scope", token}, {"node", node},
                               {"port", "value"}, {"source", nullptr}});
    }
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

bool GraphCanvasModel::setScalarTarget(std::string_view scope, const std::string &node,
                                       const std::string &target) {
    if (!canEdit() || target.empty()) return false;
    const auto *found = findNode(scope, node);
    if (found == nullptr || textOf(*found, "op") != "render.scalar") return false;
    const auto parameters = nodeParameters(scope, node);
    const auto old_target = parameters.is_object() ? textOf(parameters, "target") : std::string{};
    if (old_target == target) return false;
    const auto token = tokenOf(scope);
    Json ops = Json::array();
    const auto repack = [&](const std::string &buffer, std::int64_t *end) {
        std::int64_t packed = 0;
        for (const auto &[id, offset] : scalarLayout(scope, buffer)) {
            (void)offset;
            if (id == node) continue;
            const auto before = nodeParameters(scope, id);
            const auto after = scalarParameters(before, nullptr, packed);
            packed += static_cast<std::int64_t>(scalarComponents(before) * sizeof(float));
            if (after != before)
                ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", id},
                                   {"parameters", after}});
        }
        if (end != nullptr) *end = packed;
    };
    // The node joins the new buffer after the sinks already there; the ones
    // left behind close the gap.
    std::int64_t end = 0;
    repack(target, &end);
    auto moved = scalarParameters(parameters, nullptr, end);
    moved["target"] = target;
    ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", node},
                       {"parameters", moved}});
    if (!old_target.empty()) repack(old_target, nullptr);
    return submitOps(std::move(ops));
}

std::vector<std::string> GraphCanvasModel::scalarTargets(std::string_view scope) const {
    auto result = bufferChoices(scope);
    // A feature's runtime buffer (bloom.intensity) is no node of the graph:
    // the frame plan lists it among its buffers.
    std::vector<std::string> planned{plan_buffers_.begin(), plan_buffers_.end()};
    std::ranges::sort(planned);
    for (const auto &buffer : planned)
        if (std::ranges::find(result, buffer) == result.end() && buffer.find('@') == std::string::npos)
            result.push_back(buffer);
    return result;
}

// --- Decision 13 (a) / UX-12a: the catalog's required parameters and its table.

std::vector<GraphCanvasModel::RequiredParameter>
GraphCanvasModel::requiredParametersFor(std::string_view op) const {
    std::vector<RequiredParameter> result;
    // What the palette seeds on its own is not asked for: a curve's keys,
    // a voice's sound, a UI sink's node, a render node's parameters (UX-3's
    // row beside the op), a draw step's.
    if (op == "value.curve" || op == "audio.playback" || op.starts_with("ui.") ||
        (op.starts_with("render.") && op != "render.scalar") || op.starts_with("draw."))
        return result;
    const auto inputs = operationInputPorts(op);
    for (const auto &entry : operationParameters(op)) {
        if (!entry.is_object() || !entry.value("required", false)) continue;
        RequiredParameter required;
        required.name = textOf(entry, "name");
        if (required.name.empty()) continue;
        required.kind = textOf(entry, "kind");
        required.schema = entry;
        if (const auto alternatives = entry.find("alternatives");
            alternatives != entry.end() && alternatives->is_array())
            for (const auto &name : *alternatives) {
                if (!name.is_string()) continue;
                required.alternatives.push_back(name.get<std::string>());
                if (std::ranges::find(inputs, name.get<std::string>()) != inputs.end())
                    required.edge_alternatives.push_back(name.get<std::string>());
            }
        // What Studio knows the string names.
        if (op == "render.scalar" && required.name == "target") required.kind = "buffer";
        else if (op == "value.input" && required.name == "name") required.kind = "input";
        result.push_back(std::move(required));
    }
    return result;
}

Json GraphCanvasModel::operationParameters(std::string_view op) const {
    const auto *contract = operation(op);
    if (contract == nullptr) return Json::array();
    const auto found = contract->find("parameters");
    if (found == contract->end() || !found->is_array()) return Json::array();
    return *found;
}

bool GraphCanvasModel::operationParametersComplete(std::string_view op) const {
    const auto *contract = operation(op);
    if (contract == nullptr) return false;
    const auto found = contract->find("parameters_complete");
    return found != contract->end() && found->is_boolean() && found->get<bool>();
}

std::optional<Json> GraphCanvasModel::parameterSchema(std::string_view op,
                                                      std::string_view name) const {
    const auto *contract = operation(op);
    if (contract == nullptr) return std::nullopt;
    const auto found = contract->find("parameters");
    if (found == contract->end() || !found->is_array()) return std::nullopt;
    for (const auto &entry : *found)
        if (entry.is_object() && textOf(entry, "name") == name) return entry;
    return std::nullopt;
}

std::vector<std::string> GraphCanvasModel::operationInputPorts(std::string_view op) const {
    std::vector<std::string> ports;
    const auto *contract = operation(op);
    if (contract == nullptr) return ports;
    const auto found = contract->find("inputs");
    if (found == contract->end() || !found->is_array()) return ports;
    for (const auto &input : *found)
        if (input.is_object() && !textOf(input, "port").empty()) ports.push_back(textOf(input, "port"));
    return ports;
}

std::vector<std::string> GraphCanvasModel::valueInputNamesFor() const {
    const auto *contract = operation("value.input");
    const auto listed = contract != nullptr ? contract->find("input_names") : Json::const_iterator{};
    if (contract == nullptr || listed == contract->end() || !listed->is_array())
        return valueInputNames();
    std::vector<std::string> names;
    for (const auto &entry : *listed) {
        const auto name = entry.is_object() ? textOf(entry, "name")
                          : entry.is_string() ? entry.get<std::string>()
                                              : std::string{};
        if (!name.empty()) names.push_back(name);
    }
    // The scene clock first, as the table Studio kept did: it is the one Play,
    // Pause and the Timeline move, which is what a curve is placed against.
    std::ranges::stable_partition(names, [](const std::string &name) { return name == "scene_time"; });
    return names.empty() ? valueInputNames() : names;
}

// --- Decision 9: sounds copied into the project.

GraphCanvasAudioImport importProjectAudio(const std::filesystem::path &project_root,
                                          const std::vector<std::filesystem::path> &files) {
    GraphCanvasAudioImport result;
    if (project_root.empty()) {
        result.error = "No project is open.";
        return result;
    }
    const auto folder = project_root / "assets" / "audio";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        result.error = "Could not make " + folder.generic_string() + ": " + error.message();
        return result;
    }
    for (const auto &file : files) {
        auto extension = file.extension().string();
        std::ranges::transform(extension, extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".wav" && extension != ".ogg") {
            result.error = file.filename().string() + " is not a .wav or .ogg file.";
            continue;
        }
        const auto stem = file.stem().string();
        const auto original = file.extension().string();
        auto name = stem + original;
        for (int suffix = 2; std::filesystem::exists(folder / name) && suffix < 10000; ++suffix)
            name = stem + "_" + std::to_string(suffix) + original;
        if (std::filesystem::exists(folder / name)) {
            result.error = "No free name for " + file.filename().string() + ".";
            continue;
        }
        std::filesystem::copy_file(file, folder / name, std::filesystem::copy_options::none, error);
        if (error) {
            result.error = "Could not copy " + file.filename().string() + ": " + error.message();
            error.clear();
            continue;
        }
        result.references.push_back("project://assets/audio/" + name);
    }
    return result;
}

// --- IA-1s, decisions 1 and 4: the images a render.image starts from.

namespace {

std::string lowerExtension(const std::filesystem::path &file) {
    auto extension = file.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

// The first free `<stem>[_N]<extension>` in `folder`.
std::optional<std::string> freeName(const std::filesystem::path &folder, const std::string &stem,
                                    const std::string &extension) {
    auto name = stem + extension;
    for (int suffix = 2; std::filesystem::exists(folder / name) && suffix < 10000; ++suffix)
        name = stem + "_" + std::to_string(suffix) + extension;
    if (std::filesystem::exists(folder / name)) return std::nullopt;
    return name;
}

} // namespace

GraphCanvasImageAssetsRead readProjectImageAssets(const std::filesystem::path &project_root) {
    GraphCanvasImageAssetsRead read;
    if (project_root.empty()) {
        read.error = "no project is open";
        return read;
    }
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        project_root, std::filesystem::directory_options::skip_permission_denied, error};
    if (error) {
        read.error = "the project folder could not be read: " + error.message();
        return read;
    }
    try {
        for (auto end = std::filesystem::recursive_directory_iterator{}; walk != end;
             walk.increment(error)) {
            if (error) break;
            const auto &entry = *walk;
            std::error_code kind;
            if (entry.is_directory(kind)) {
                const auto name = entry.path().filename().u8string();
                if (!name.empty() && name.front() == u8'.') walk.disable_recursion_pending();
                continue;
            }
            const auto extension = lowerExtension(entry.path());
            if (extension != ".ktx2" && extension != ".png") continue;
            const auto relative = entry.path().lexically_relative(project_root).generic_u8string();
            if (relative.empty() || relative.starts_with(u8"..")) continue;
            read.assets.emplace_back(relative.begin(), relative.end());
        }
    } catch (const std::exception &caught) {
        read.error = std::string{"the project folder could not be read: "} + caught.what();
    }
    std::ranges::sort(read.assets);
    if (read.assets.empty() && read.error.empty())
        read.error = "no .ktx2 or .png file in the project";
    return read;
}

std::optional<GraphCanvasImageExtent> readImageAssetExtent(const std::filesystem::path &file) {
    std::ifstream input{file, std::ios::binary};
    if (!input) return std::nullopt;
    std::array<unsigned char, 32> header{};
    input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() < 28) return std::nullopt;
    const auto little = [&](std::size_t at) {
        return std::uint32_t{header[at]} | std::uint32_t{header[at + 1]} << 8 |
               std::uint32_t{header[at + 2]} << 16 | std::uint32_t{header[at + 3]} << 24;
    };
    const auto big = [&](std::size_t at) {
        return std::uint32_t{header[at]} << 24 | std::uint32_t{header[at + 1]} << 16 |
               std::uint32_t{header[at + 2]} << 8 | std::uint32_t{header[at + 3]};
    };
    // KTX2: the identifier, vkFormat, typeSize, then pixelWidth and pixelHeight.
    constexpr std::array<unsigned char, 12> ktx2{0xAB, 'K', 'T', 'X', ' ', '2', '0',
                                                 0xBB, '\r', '\n', 0x1A, '\n'};
    if (std::equal(ktx2.begin(), ktx2.end(), header.begin())) {
        GraphCanvasImageExtent extent{little(20), little(24)};
        // A 2D image's height is never 0; a 1D one is one row.
        if (extent.height == 0) extent.height = 1;
        if (extent.width == 0) return std::nullopt;
        return extent;
    }
    // PNG: the signature, then IHDR's width and height.
    constexpr std::array<unsigned char, 8> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (std::equal(png.begin(), png.end(), header.begin())) {
        GraphCanvasImageExtent extent{big(16), big(20)};
        if (extent.width == 0 || extent.height == 0) return std::nullopt;
        return extent;
    }
    return std::nullopt;
}

GraphCanvasImageImport importProjectImage(const std::filesystem::path &project_root,
                                          const std::filesystem::path &file) {
    GraphCanvasImageImport result;
    if (project_root.empty()) {
        result.error = "No project is open.";
        return result;
    }
    const auto extension = lowerExtension(file);
    if (extension != ".ktx2" && extension != ".png" && extension != ".hdr") {
        result.error = file.filename().string() + " is not a .ktx2, .png or .hdr file.";
        return result;
    }
    const auto folder = project_root / "assets" / "images";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        result.error = "Could not make " + folder.generic_string() + ": " + error.message();
        return result;
    }
    const auto stem = file.stem().string();
    const auto name = freeName(folder, stem, file.extension().string());
    if (!name) {
        result.error = "No free name for " + file.filename().string() + ".";
        return result;
    }
    std::filesystem::copy_file(file, folder / *name, std::filesystem::copy_options::none, error);
    if (error) {
        result.error = "Could not copy " + file.filename().string() + ": " + error.message();
        return result;
    }
    if (extension != ".hdr") {
        result.reference = "assets/images/" + *name;
        return result;
    }
    // The engine reads no .hdr: it is baked into an RGBA32F .ktx2 beside it.
    const auto baked = freeName(folder, std::filesystem::path{*name}.stem().string(), ".ktx2");
    if (!baked) {
        result.error = "No free name for the .ktx2 of " + file.filename().string() + ".";
        return result;
    }
    result.reference = "assets/images/" + *baked;
    result.convert = GraphCanvasImageImport::Conversion{folder / *name, folder / *baked};
    return result;
}

} // namespace PelicanStudio
