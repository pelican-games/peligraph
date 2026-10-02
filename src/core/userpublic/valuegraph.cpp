#include "valuegraph.hpp"
#include "gamecontext.hpp"
#include "../../project/graphmodule.hpp"
#include "../../project/graphpartition.hpp"
#include "../container.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/sampletable.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican::ValueGraph {
namespace {
using Json = nlohmann::json;
void keys(const Json &object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) throw std::invalid_argument("value graph requires an object");
    for (const auto &[key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("unknown value graph field: " + key);
    }
}
std::string name(const Json &value) {
    if (!value.is_string() || value.get_ref<const std::string &>().empty())
        throw std::invalid_argument("value graph names must be nonempty strings");
    return value.get<std::string>();
}
double number(const Json &value) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw std::invalid_argument("value graph values must be finite numbers");
    return value.get<double>();
}

// AB-1: value.sample_table's asset as the project resolves it; a graph built
// where no project is set up (a tool, a test) names a file.
std::filesystem::path sampleTableFile(const std::string &asset) {
    if (const auto *resolver = FastModuleContainer::tryGet<PathResolver>(); resolver != nullptr && resolver->isSetup())
        return resolver->resolveExistingFile(asset);
    return std::filesystem::path{asset};
}

bool active(const ScopeDependencies &dependencies, std::string_view name, const InactiveScopes &inactive) {
    if (inactive.empty()) return true;
    const auto found = dependencies.find(std::string{name});
    return found == dependencies.end() || std::none_of(found->second.begin(), found->second.end(),
        [&](const auto &scope) { return inactive.contains(scope); });
}

// The three operations whose result depends on what earlier evaluations did.
// Evaluating one without state is not a missing argument, it is the one thing
// this backend cannot do -- so it says so under a name, with the node that
// asked and the operation spelled the way an authored document spells it (the
// lowered schema drops the module namespace; a caller reading the rejection
// should not have to know that).
GraphRegistryError statelessEvaluationRejected(std::string_view op, const std::string &node) {
    return GraphRegistryError("graph.evaluation_requires_state@1",
        {{"node", node}, {"op", std::string{op}}});
}

InstanceInputSource instanceInputSourceOf(GraphInstanceInputSource source) {
    switch (source) {
    case GraphInstanceInputSource::connected: return InstanceInputSource::connected;
    case GraphInstanceInputSource::parameter: return InstanceInputSource::parameter;
    case GraphInstanceInputSource::defaulted: return InstanceInputSource::defaulted;
    }
    throw std::invalid_argument("unknown graph instance input source");
}

// AU-1. audio.playback in two forms that mean the same thing (decision 2):
// the integrated one carries value.playback's time map itself (wrap,
// source_begin, source_end, speed, delay -- each literal or an edge) and is
// started and stopped by its events; the decomposed one follows `position`,
// the source second any graph computes. One node is one or the other.
// In either form `events` is optional (AU-3): without them the owner's first
// frame starts the voice, and the integrated map's elapsed time is counted
// from that frame on the `time` input's clock.
constexpr std::string_view audioMappingKeys[] = {"wrap", "source_begin", "source_end", "speed", "delay"};

Json lowerAudioPlayback(const Json &node, const Json &parameters, const Json &inputs,
                        const std::function<std::set<std::string>(const Json &)> &clockSources) {
    keys(parameters, {"asset", "bus", "wrap", "source_begin", "source_end", "speed", "delay"});
    const auto id = name(node.at("id"));
    const auto source = [](const Json &endpoint) {
        if (endpoint.at("port") != "value")
            throw std::invalid_argument("scalar backend requires a value output port");
        return endpoint.at("node");
    };
    Json lowered{{"id", id}, {"op", "audio_playback"}, {"asset", name(parameters.at("asset"))},
                 {"time", source(inputs.at("time"))}};
    if (parameters.contains("bus")) {
        const auto bus = name(parameters.at("bus"));
        if (bus != "se" && bus != "bgm")
            throw std::invalid_argument("unknown audio bus: " + bus + " (a voice plays on se or bgm)");
        lowered["bus"] = bus;
    }
    const bool position = inputs.contains("position");
    auto mapping = Json::array();
    for (const auto key : audioMappingKeys) {
        const std::string field{key};
        if (parameters.contains(field) || inputs.contains(field)) mapping.push_back(field);
    }
    if (position && !mapping.empty())
        throw GraphRegistryError("audio.playback_mapping_conflict@1",
            {{"node", id}, {"parameters", std::move(mapping)}});
    // A node without events names no scene-stamped arrival, so its `time` may
    // come from any clock -- app time for a BGM that carries on across scenes.
    if (inputs.contains("events")) {
        lowered["events"] = inputs.at("events").at("node");
        // AU-2 (the rest of decision 8): events are always stamped on scene
        // time, and a node reads their arrival with its `time` input. Read on
        // any other clock -- app time, which keeps running across scenes --
        // they would arrive at the wrong moment, so `time` must come from
        // scene_time alone whenever events are connected.
        const auto time_clock = clockSources(inputs.at("time"));
        if (time_clock != std::set<std::string>{"scene_time"})
            throw GraphRegistryError("audio.clock_mismatch@1",
                {{"node", id}, {"events_clock", Json::array({"scene_time"})}, {"time_clock", time_clock}});
    }
    if (position) {
        lowered["position"] = source(inputs.at("position"));
        // Decision 8: events are stamped on the clock the node reads them
        // with. A position computed from another clock would put the two
        // halves of one node on different time lines.
        if (inputs.contains("events")) {
            const auto events_clock = clockSources(inputs.at("time"));
            const auto position_clock = clockSources(inputs.at("position"));
            if (!events_clock.empty() && !position_clock.empty() && events_clock != position_clock)
                throw GraphRegistryError("audio.clock_mismatch@1",
                    {{"node", id}, {"events_clock", events_clock}, {"position_clock", position_clock}});
        }
    }
    if (parameters.contains("wrap")) lowered["wrap"] = name(parameters.at("wrap"));
    for (const auto key : audioMappingKeys) {
        const std::string field{key};
        if (field == "wrap") continue;
        if (parameters.contains(field) && inputs.contains(field))
            throw std::invalid_argument("input also defined as a parameter: " + field);
        if (parameters.contains(field)) lowered[field] = number(parameters.at(field));
        else if (inputs.contains(field)) lowered[field] = source(inputs.at(field));
    }
    return lowered;
}

Json lowerScalarGraph(const Json &authored, const GraphModuleRegistry &modules,
                      GraphInstanceOwners &owners,
                      ScopeDependencies &node_scopes, ScopeDependencies &output_scopes,
                      std::vector<Graph::InstanceInputBinding> &instance_inputs) {
    auto partitions = partitionGraphDocument(authored, modules);
    const auto &expanded = partitions.values;
    owners = expanded.instance_owners;
    for (const auto &[key, input] : expanded.instance_inputs)
        instance_inputs.push_back(Graph::InstanceInputBinding{key, graphInstanceInputOutput(key),
            instanceInputSourceOf(input.source),
            input.source == GraphInstanceInputSource::connected
                ? input.endpoint.at("node").get<std::string>() : std::string{},
            input.source == GraphInstanceInputSource::connected
                ? input.endpoint.at("port").get<std::string>() : std::string{},
            input.parameter});
    node_scopes = std::move(partitions.rendering.node_scopes);
    node_scopes.insert(expanded.node_scopes.begin(), expanded.node_scopes.end());
    output_scopes = expanded.output_scopes;
    Json result{{"schema", "pelican.value_graph"}, {"version", 1},
                {"nodes", Json::array()}, {"outputs", Json::object()}, {"bindings", Json::array()}};
    const auto source = [](const Json &endpoint) {
        if (endpoint.at("port") != "value")
            throw std::invalid_argument("scalar backend requires a value output port");
        return endpoint.at("node");
    };
    for (const auto &[output, endpoint] : expanded.outputs.items())
        if (endpoint.at("port") != "events" && endpoint.at("port") != "voice")
            result["outputs"][output] = Json::array({source(endpoint)});
    std::map<std::string, const Json *, std::less<>> expanded_nodes;
    for (const auto &node : expanded.nodes) expanded_nodes.emplace(name(node.at("id")), &node);
    // The graph's own buffers, which a render.scalar may name (RS-15).
    std::map<std::string, const Json *, std::less<>> rendering_nodes;
    for (const auto &node : partitions.rendering.nodes) rendering_nodes.emplace(name(node.at("id")), &node);
    // The engine clocks an edge reads, however far upstream: the names of the
    // value.input nodes it reaches among time and scene_time.
    const auto clockSources = [&](const Json &endpoint) {
        std::set<std::string> clocks;
        std::set<std::string> seen;
        std::vector<std::string> pending{name(endpoint.at("node"))};
        while (!pending.empty()) {
            auto id = std::move(pending.back());
            pending.pop_back();
            if (!seen.insert(id).second) continue;
            const auto found = expanded_nodes.find(id);
            if (found == expanded_nodes.end()) continue;
            const auto &upstream = *found->second;
            if (upstream.at("op") == "value.input") {
                const auto input = upstream.value("parameters", Json::object()).value("name", std::string{});
                if (input == "time" || input == "scene_time") clocks.insert(input);
            }
            const auto upstream_inputs = upstream.value("inputs", Json::object());
            for (const auto &[port, edge] : upstream_inputs.items()) {
                (void)port;
                pending.push_back(name(edge.at("node")));
            }
        }
        return clocks;
    };
    for (const auto &node : expanded.nodes) {
        const auto op = name(node.at("op"));
        const auto parameters = node.value("parameters", Json::object());
        const auto &inputs = node.at("inputs");
        if (op == "event.playback") {
            keys(parameters, {});
            result["nodes"].push_back({{"id", node.at("id")}, {"op", "event_input"}});
            continue;
        }
        if (op == "value.sample_table") {
            // AB-1. Without a time edge it reads scene_time, the clock
            // audio.playback's events are stamped on, through a value.input
            // the lowering adds.
            keys(parameters, {"asset", "row"});
            const auto id = name(node.at("id"));
            if (!parameters.contains("asset") || !parameters.contains("row"))
                throw std::invalid_argument("value.sample_table requires an asset and a row: " + id);
            Json lowered{{"id", id}, {"op", "sample_table"}, {"asset", name(parameters.at("asset"))},
                         {"row", parameters.at("row")}};
            if (inputs.contains("time")) {
                lowered["time"] = source(inputs.at("time"));
            } else {
                const auto clock = id + "/$scene_time";
                if (expanded_nodes.contains(clock)) throw std::invalid_argument("reserved graph node: " + clock);
                result["nodes"].push_back({{"id", clock}, {"op", "input"}, {"name", "scene_time"}});
                node_scopes[clock] = node_scopes.at(id);
                lowered["time"] = clock;
            }
            result["nodes"].push_back(std::move(lowered));
            continue;
        }
        if (op == "audio.playback") {
            result["nodes"].push_back(lowerAudioPlayback(node, parameters, inputs, clockSources));
            const auto id = name(node.at("id"));
            const auto output = "$audio_time/" + id;
            output_scopes[output] = node_scopes.at(id);
            continue;
        }
        for (const auto &[port, contract] : modules.resolveOperation(node).inputs)
            if (!contract.required && parameters.contains(port))
                (void)number(parameters.at(port)); // References belong in typed input edges.
        if (op == "render.scalar" || op == "audio.volume" || op == "audio.pitch") {
            if (op == "render.scalar") keys(parameters, {"target", "value", "offset"});
            else keys(parameters, {"target"});
            const auto id = name(node.at("id"));
            const bool voice = inputs.contains("voice");
            if (voice && parameters.contains("target"))
                throw std::invalid_argument("audio binding cannot specify both voice and target");
            const auto output = "$binding/" + id;
            if (result["outputs"].contains(output)) throw std::invalid_argument("reserved graph output: " + output);
            Json binding{{"output", output},
                {"kind", op == "render.scalar" ? "render_buffer" : op == "audio.volume" ? "sound_volume" : "sound_pitch"},
                {"target", voice ? name(inputs.at("voice").at("node")) : name(parameters.at("target"))},
                {"voice", voice}};
            if (op == "render.scalar" && parameters.contains("value")) {
                // RS-15: a vec2 .. vec4 written as numbers; each component is
                // a constant the binding publishes in order.
                const auto &value = parameters.at("value");
                if (!value.is_array() || value.size() < 2 || value.size() > 4)
                    throw std::invalid_argument("render.scalar value is a vec2, vec3 or vec4 written as 2 to 4 "
                                                "numbers (a scalar comes in on the value edge): " + id);
                auto references = Json::array();
                for (std::size_t component = 0; component < value.size(); ++component) {
                    const auto constant = output + "/" + std::to_string(component);
                    if (expanded_nodes.contains(constant))
                        throw std::invalid_argument("reserved graph node: " + constant);
                    result["nodes"].push_back({{"id", constant}, {"op", "constant"},
                                               {"value", number(value.at(component))}});
                    node_scopes[constant] = node_scopes.at(id);
                    references.push_back(constant);
                }
                result["outputs"][output] = std::move(references);
            } else {
                result["outputs"][output] = Json::array({source(inputs.at("value"))});
            }
            if (op == "render.scalar") {
                if (const auto offset = parameters.find("offset"); offset != parameters.end()) {
                    // RS-15: where in the buffer the values land, in bytes.
                    if (!offset->is_number_integer() || offset->get<std::int64_t>() < 0 ||
                        offset->get<std::uint64_t>() % sizeof(float) != 0)
                        throw std::invalid_argument("render.scalar offset is a byte count, a multiple of 4: " + id);
                    binding["offset"] = *offset;
                }
                // RS-15: the values are float32. A buffer of the graph whose
                // elements are integers cannot take them.
                if (const auto found = rendering_nodes.find(binding.at("target").get<std::string>());
                    found != rendering_nodes.end()) {
                    const auto &target_node = *found->second;
                    const auto element = target_node.value("parameters", Json::object()).value("element", std::string{});
                    static const std::set<std::string, std::less<>> floats{"float", "vec2", "vec3", "vec4", "mat4"};
                    if ((target_node.at("op") == "render.buffer" || target_node.at("op") == "render.attribute") &&
                        !element.empty() && !floats.contains(element))
                        throw GraphRegistryError("graph.render_scalar_type_mismatch@1",
                            {{"node", id}, {"target", binding.at("target")}, {"element", element},
                             {"components", result["outputs"][output].size()}});
                }
            }
            output_scopes[output] = node_scopes.at(id);
            result["bindings"].push_back(std::move(binding));
            continue;
        }
        if (op == "ui.value" || op == "ui.visible") {
            // The UI node is named by its path in the project's UI document.
            keys(parameters, {"node"});
            if (!parameters.contains("node"))
                throw std::invalid_argument("ui sink requires a node path: " + name(node.at("id")));
            const auto output = "$binding/" + name(node.at("id"));
            if (result["outputs"].contains(output)) throw std::invalid_argument("reserved graph output: " + output);
            result["outputs"][output] = Json::array({source(inputs.at("value"))});
            output_scopes[output] = node_scopes.at(name(node.at("id")));
            result["bindings"].push_back({{"output", output},
                {"kind", op == "ui.value" ? "ui_value" : "ui_visible"},
                {"target", name(parameters.at("node"))}});
            continue;
        }
        if (!parameters.is_object()) throw std::invalid_argument("node parameters must be an object");
        Json lowered = parameters;
        if (lowered.contains("id") || lowered.contains("op"))
            throw std::invalid_argument("node parameters cannot replace id/op");
        lowered["id"] = node.at("id");
        // The backend names an operation without its module namespace.
        lowered["op"] = op.substr(op.find('.') + 1);
        for (const auto &[port, endpoint] : inputs.items()) {
            if (lowered.contains(port)) throw std::invalid_argument("input also defined as a parameter: " + port);
            lowered[port] = op == "value.event_clock" && port == "events" ? endpoint.at("node") : source(endpoint);
        }
        result["nodes"].push_back(std::move(lowered));
    }
    return result;
}
}

Graph::Graph(std::string_view document) : Graph(document, builtinGraphModuleRegistry()) {}

Graph::Graph(std::string_view document, const GraphModuleRegistry &modules) : modules_(&modules) {
    const auto authored = Json::parse(document);
    const auto root = authored.value("schema", std::string{}) == "pelican.graph"
        ? lowerScalarGraph(authored, modules, instance_owners_, node_scopes_, output_scopes_,
                           instance_inputs_) : authored;
    for (const auto &binding : instance_inputs_)
        if (binding.source == InstanceInputSource::connected) shadowed_parameters_.insert(binding.key);
    if (authored.value("schema", std::string{}) == "pelican.graph")
        for (const auto &[output, value] : root.at("outputs").items()) {
            (void)value;
            if (!authored.at("outputs").contains(output)) lowered_outputs_.push_back(output);
        }
    keys(root, {"schema", "version", "nodes", "outputs", "bindings"});
    if (root.at("schema") != "pelican.value_graph" || root.at("version") != 1)
        throw std::invalid_argument("unsupported value graph schema/version");
    const auto &nodes = root.at("nodes");
    if (!nodes.is_array()) throw std::invalid_argument("value graph nodes must be an array");
    std::map<std::string, std::size_t, std::less<>> indices;
    for (const auto &node : nodes) {
        auto id = name(node.at("id"));
        if (!indices.emplace(id, nodes_.size()).second)
            throw std::invalid_argument("duplicate value graph node: " + id);
        nodes_.push_back(Node{.id = std::move(id)});
    }
    const auto resolve = [&](const Json &reference) {
        const auto id = name(reference);
        const auto found = indices.find(id);
        if (found == indices.end()) throw std::invalid_argument("unknown value graph node: " + id);
        return found->second;
    };
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const auto &authored = nodes[i];
        auto &node = nodes_[i];
        const auto op = name(authored.at("op"));
        if (op == "morph_weight") {
            keys(authored, {"id", "op", "node", "target"});
            const auto *owner = scopeOwner(node.id);
            if (!owner) throw std::invalid_argument("morph weight source requires an owned graph instance");
            const auto index = [&](const char *key) {
                const auto &value = authored.at(key);
                if (!value.is_number_integer() ||
                    (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
                    value.get<std::uint64_t>() > (std::numeric_limits<std::uint32_t>::max)())
                    throw std::invalid_argument("morph weight source index must fit uint32");
                return value.get<std::uint32_t>();
            };
            node.op = Op::input;
            node.input_name = "$morph/" + node.id;
            morph_sources_.emplace(node.id, MorphWeightSource{*owner, index("node"), index("target")});
        } else if (op == "input") {
            keys(authored, {"id", "op", "name"});
            node.op = Op::input;
            node.input_name = name(authored.at("name"));
        } else if (op == "parameter") {
            keys(authored, {"id", "op", "name", "default"});
            node.op = Op::parameter;
            node.input_name = name(authored.at("name"));
            node.constant = number(authored.at("default"));
            if (!parameter_defaults_.emplace(node.input_name, node.constant).second)
                throw std::invalid_argument("duplicate value graph parameter: " + node.input_name);
        } else if (op == "constant") {
            keys(authored, {"id", "op", "value"});
            node.op = Op::constant;
            node.constant = number(authored.at("value"));
        } else if (op == "add" || op == "multiply") {
            keys(authored, {"id", "op", "a", "b"});
            node.op = op == "add" ? Op::add : Op::multiply;
            node.dependencies = {resolve(authored.at("a")), resolve(authored.at("b"))};
        } else if (op == "event_input") {
            keys(authored, {"id", "op"});
            node.op = Op::event_input;
            event_consumers_.emplace(node.id, std::vector<std::size_t>{});
        } else if (op == "event_clock") {
            keys(authored, {"id", "op", "time", "events"});
            node.op = Op::event_clock;
            node.dependencies = {resolve(authored.at("time")), resolve(authored.at("events"))};
        } else if (op == "audio_playback") {
            keys(authored, {"id", "op", "time", "events", "asset", "bus", "position",
                            "wrap", "source_begin", "source_end", "speed", "delay"});
            audio_sources_.emplace(node.id, name(authored.at("asset")));
            audio_nodes_.push_back(i);
            node.op = Op::audio_playback;
            node.audio_bus = authored.value("bus", std::string("se"));
            if (node.audio_bus != "se" && node.audio_bus != "bgm")
                throw std::invalid_argument("unknown audio bus: " + node.audio_bus + " (a voice plays on se or bgm)");
            node.dependencies = {resolve(authored.at("time"))};
            node.audio_events = authored.contains("events");
            if (node.audio_events) node.dependencies.push_back(resolve(authored.at("events")));
            node.audio_position = authored.contains("position");
            if (node.audio_position) node.dependencies.push_back(resolve(authored.at("position")));
            // Neither events nor a position (AU-3): the voice starts on its
            // owner's first frame and plays the map from there (publish).
            // The integrated time map (decision 2). The source's own length is
            // the range's end unless source_end says otherwise, and the range's
            // length is its duration: speed 1 plays the source in real time.
            node.playback_input_base = node.dependencies.size();
            using Settings = Animation::CurvePlaybackSettings;
            const std::pair<std::string_view, double Settings::*> fields[] = {
                {"source_begin", &Settings::source_begin}, {"source_end", &Settings::source_end},
                {"speed", &Settings::speed}, {"delay", &Settings::delay}};
            node.playback.source_begin = 0.0;
            bool literal = true;
            for (const auto &[field, member] : fields) {
                const auto found = authored.find(field);
                if (found == authored.end()) continue;
                if (node.audio_position)
                    throw GraphRegistryError("audio.playback_mapping_conflict@1",
                        {{"node", node.id}, {"parameters", Json::array({std::string{field}})}});
                if (member == &Settings::source_end) node.audio_end_from_source = false;
                if (found->is_string()) {
                    node.dependencies.push_back(resolve(*found));
                    node.playback_inputs.push_back(member);
                    literal = false;
                } else node.playback.*member = number(*found);
            }
            const auto wrap = authored.value("wrap", std::string("once"));
            if (authored.contains("wrap") && node.audio_position)
                throw GraphRegistryError("audio.playback_mapping_conflict@1",
                    {{"node", node.id}, {"parameters", Json::array({"wrap"})}});
            if (wrap == "once") node.playback.wrap = Animation::CurveWrap::once;
            else if (wrap == "repeat") node.playback.wrap = Animation::CurveWrap::repeat;
            else if (wrap == "ping_pong") node.playback.wrap = Animation::CurveWrap::ping_pong;
            else throw std::invalid_argument("unknown playback wrap: " + wrap);
            if (literal) {
                const auto &mapping = node.playback;
                if (mapping.speed == 0.0 || mapping.delay < 0.0 || mapping.source_begin < 0.0 ||
                    (!node.audio_end_from_source && mapping.source_end <= mapping.source_begin))
                    throw std::invalid_argument("invalid audio playback range, speed or delay: " + node.id);
            }
            // A loop point the backend can hold by itself: nothing in the map
            // changes while the voice plays and it never runs backwards.
            node.audio_native = !node.audio_position && literal && node.playback.speed > 0.0 &&
                                node.playback.wrap != Animation::CurveWrap::ping_pong;
        } else if (op == "sample_table") {
            // AB-1: the table is read once, here; evaluating reads memory.
            keys(authored, {"id", "op", "asset", "row", "time"});
            node.op = Op::sample_table;
            node.dependencies = {resolve(authored.at("time"))};
            const auto asset = name(authored.at("asset"));
            try {
                node.table = loadSampleTable(sampleTableFile(asset));
            } catch (const std::exception &error) {
                throw GraphRegistryError(std::string{sampleTableUnreadableCode},
                    {{"node", node.id}, {"asset", asset}, {"reason", error.what()}});
            }
            const auto row = sampleTableRow(node.table->layout, authored.at("row"));
            if (!row)
                throw GraphRegistryError(std::string{sampleTableRowUnknownCode},
                    {{"node", node.id}, {"asset", asset}, {"row", authored.at("row")},
                     {"rows", node.table->layout.features}});
            node.table_row = *row;
        } else if (op == "trigger_clock") {
            keys(authored, {"id", "op", "time", "trigger", "paused", "reset"});
            node.op = Op::trigger_clock;
            node.dependencies = {resolve(authored.at("time")), resolve(authored.at("trigger"))};
            const auto optionalInput = [&](const char *field, bool &connected, double &literal) {
                const auto found = authored.find(field);
                if (found == authored.end()) return;
                if (found->is_string()) {
                    connected = true;
                    node.dependencies.push_back(resolve(*found));
                } else literal = number(*found);
            };
            optionalInput("paused", node.has_pause, node.pause_default);
            optionalInput("reset", node.has_reset, node.reset_default);
        } else if (op == "playback") {
            keys(authored, {"id", "op", "elapsed", "source_begin", "source_end",
                            "duration", "speed", "delay", "sample_interval", "wrap"});
            node.op = Op::playback;
            node.dependencies = {resolve(authored.at("elapsed"))};
            using Settings = Animation::CurvePlaybackSettings;
            const std::pair<std::string_view, double Settings::*> fields[] = {
                {"source_begin", &Settings::source_begin}, {"source_end", &Settings::source_end},
                {"duration", &Settings::duration}, {"speed", &Settings::speed},
                {"delay", &Settings::delay}, {"sample_interval", &Settings::sample_interval}};
            for (const auto &[field, member] : fields) {
                const auto found = authored.find(field);
                if (found == authored.end()) continue;
                if (found->is_string()) {
                    node.dependencies.push_back(resolve(*found));
                    node.playback_inputs.push_back(member);
                } else node.playback.*member = number(*found);
            }
            const auto wrap = authored.value("wrap", std::string("once"));
            if (wrap == "once") node.playback.wrap = Animation::CurveWrap::once;
            else if (wrap == "repeat") node.playback.wrap = Animation::CurveWrap::repeat;
            else if (wrap == "ping_pong") node.playback.wrap = Animation::CurveWrap::ping_pong;
            else throw std::invalid_argument("unknown playback wrap: " + wrap);
        } else if (op == "curve") {
            keys(authored, {"id", "op", "time", "keys"});
            node.op = Op::curve;
            node.dependencies = {resolve(authored.at("time"))};
            const auto &curve_keys = authored.at("keys");
            if (!curve_keys.is_array()) throw std::invalid_argument("curve keys must be an array");
            std::vector<Animation::CurveKey> decoded;
            for (const auto &key : curve_keys) {
                keys(key, {"time", "value", "interpolation", "in_tangent", "out_tangent"});
                Animation::CurveKey result;
                result.time = number(key.at("time"));
                result.value = number(key.at("value"));
                result.in_tangent = number(key.value("in_tangent", Json(0.0)));
                result.out_tangent = number(key.value("out_tangent", Json(0.0)));
                const auto interpolation = key.value("interpolation", std::string("linear"));
                if (interpolation == "linear") result.interpolation = Animation::CurveInterpolation::linear;
                else if (interpolation == "step") result.interpolation = Animation::CurveInterpolation::step;
                else if (interpolation == "cubic") result.interpolation = Animation::CurveInterpolation::cubic;
                else throw std::invalid_argument("unknown curve interpolation: " + interpolation);
                decoded.push_back(result);
            }
            node.curve.emplace(std::move(decoded));
        } else {
            // A contract can exist without a backend that runs it; say so by
            // name instead of calling the operation unknown.
            throw GraphRegistryError("graph.operation_unexecutable@1",
                {{"node", node.id}, {"op", op}, {"backend", "pelican.value_graph"}});
        }
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const auto &node = nodes_[i];
        for (std::size_t input = 0; input < node.dependencies.size(); ++input)
            if (nodes_[node.dependencies[input]].op == Op::event_input &&
                !((node.op == Op::event_clock || (node.op == Op::audio_playback && node.audio_events)) &&
                  input == 1))
                throw std::invalid_argument("event stream cannot be used as a scalar input");
        if (node.op != Op::event_clock && !(node.op == Op::audio_playback && node.audio_events)) continue;
        const auto &source = nodes_[node.dependencies[1]];
        if (source.op != Op::event_input) throw std::invalid_argument("event clock requires an event source");
        event_consumers_.at(source.id).push_back(i);
    }
    // Compile once. Declaration order does not constrain connections, and
    // duplicated edges (for example x*x) remain separate dependencies.
    std::vector<std::vector<std::size_t>> consumers(nodes_.size());
    std::vector<std::size_t> pending(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        pending[i] = nodes_[i].dependencies.size();
        for (const auto source : nodes_[i].dependencies) consumers[source].push_back(i);
        if (pending[i] == 0) order_.push_back(i);
    }
    for (std::size_t cursor = 0; cursor < order_.size(); ++cursor)
        for (const auto consumer : consumers[order_[cursor]])
            if (--pending[consumer] == 0) order_.push_back(consumer);
    if (order_.size() != nodes_.size()) throw std::invalid_argument("value graph contains a cycle");
    const auto &outputs = root.at("outputs");
    if (!outputs.is_object()) throw std::invalid_argument("value graph outputs must be an object");
    for (const auto &[output, references] : outputs.items()) {
        if (output.empty() || !references.is_array() || references.empty())
            throw std::invalid_argument("value graph output requires a name and scalar references");
        for (const auto &reference : references) {
            const auto index = resolve(reference);
            if (nodes_[index].op == Op::event_input)
                throw std::invalid_argument("event stream cannot be returned as a scalar output");
            outputs_[output].push_back(index);
        }
    }
    if (const auto bindings = root.find("bindings"); bindings != root.end()) {
        if (!bindings->is_array()) throw std::invalid_argument("value graph bindings must be an array");
        std::map<std::pair<std::string, std::string>, std::string> targets;
        for (const auto &binding : *bindings) {
            keys(binding, {"output", "kind", "target", "voice", "offset"});
            Binding decoded;
            decoded.output = name(binding.at("output"));
            decoded.target = name(binding.at("target"));
            decoded.kind = name(binding.at("kind"));
            if (decoded.kind == "render_buffer") decoded.type = Target::render_buffer;
            else if (decoded.kind == "sound_volume") decoded.type = Target::sound_volume;
            else if (decoded.kind == "sound_pitch") decoded.type = Target::sound_pitch;
            else if (decoded.kind == "ui_value") decoded.type = Target::ui_value;
            else if (decoded.kind == "ui_visible") decoded.type = Target::ui_visible;
            else throw std::invalid_argument("unknown value graph binding kind: " + decoded.kind);
            const bool ui = decoded.type == Target::ui_value || decoded.type == Target::ui_visible;
            // The sink's own id, as the lowering named its output.
            const auto sink_node = decoded.output.starts_with("$binding/")
                ? decoded.output.substr(std::string_view{"$binding/"}.size()) : decoded.output;
            decoded.voice = binding.value("voice", false);
            if (decoded.voice && (decoded.type == Target::render_buffer || !audio_sources_.contains(decoded.target)))
                throw std::invalid_argument("voice binding requires an audio playback producer");
            const auto output = outputs_.find(decoded.output);
            if (output == outputs_.end()) throw std::invalid_argument("binding references unknown output: " + decoded.output);
            if (decoded.type != Target::render_buffer && output->second.size() != 1)
                throw std::invalid_argument(ui ? "ui binding requires one scalar output"
                                               : "audio binding requires one scalar output");
            if (const auto offset = binding.find("offset"); offset != binding.end()) {
                if (decoded.type != Target::render_buffer)
                    throw std::invalid_argument("only a render_buffer binding takes an offset: " + decoded.output);
                if (!offset->is_number_integer() || offset->get<std::int64_t>() < 0 ||
                    offset->get<std::uint64_t>() % sizeof(float) != 0)
                    throw std::invalid_argument("render_buffer offset is a byte count, a multiple of 4: " +
                                                decoded.output);
                decoded.offset = offset->get<std::size_t>();
            }
            // RS-15: several render.scalar sinks share one buffer; their
            // layout is checked below, once every binding is read.
            if (decoded.type == Target::render_buffer) {
                bindings_.push_back(std::move(decoded));
                continue;
            }
            if (const auto [claimed, inserted] = targets.emplace(std::pair{decoded.kind, decoded.target}, sink_node);
                !inserted) {
                // One writer per (document, path, property): v0 has one UI
                // document, so the path and the sink's property are the unit.
                if (ui)
                    throw GraphRegistryError("ui.sink_conflict@1",
                        {{"path", decoded.target},
                         {"property", decoded.type == Target::ui_value ? "value" : "visible"},
                         {"sinks", Json::array({claimed->second, sink_node})}});
                throw std::invalid_argument("duplicate value graph target: " + decoded.target);
            }
            if (ui)
                ui_sinks_.push_back({sink_node, decoded.target,
                    decoded.type == Target::ui_value ? UiSinkProperty::value : UiSinkProperty::visible});
            bindings_.push_back(std::move(decoded));
        }
        // RS-15: the sinks writing one buffer tile it from byte 0, each at
        // its offset, without a gap or an overlap: publish stages the buffer
        // whole, in offset order, as one write.
        std::map<std::string, std::vector<const Binding *>, std::less<>> by_target;
        for (const auto &binding : bindings_)
            if (binding.type == Target::render_buffer) by_target[binding.target].push_back(&binding);
        for (auto &[target, sinks] : by_target) {
            std::stable_sort(sinks.begin(), sinks.end(),
                             [](const Binding *a, const Binding *b) { return a->offset < b->offset; });
            std::size_t expected = 0;
            for (const auto *sink : sinks) {
                if (sink->offset != expected) {
                    const auto sink_node = sink->output.starts_with("$binding/")
                        ? sink->output.substr(std::string_view{"$binding/"}.size()) : sink->output;
                    throw GraphRegistryError("graph.render_scalar_layout_invalid@1",
                        {{"target", target}, {"node", sink_node}, {"offset", sink->offset},
                         {"expected_offset", expected}, {"reason", sink->offset < expected ? "overlap" : "gap"}});
                }
                expected += outputs_.at(sink->output).size() * sizeof(float);
            }
        }
    }
    for (const auto index : audio_nodes_) {
        const auto output = "$audio_time/" + nodes_[index].id;
        if (authored.at("outputs").contains(output)) throw std::invalid_argument("reserved graph output: " + output);
        outputs_[output] = {index};
        lowered_outputs_.push_back(output);
    }
    document_ = authored.dump();
}

const GraphInstanceOwner *Graph::parameterOwner(std::string_view parameter) const noexcept {
    if (!parameter_defaults_.contains(parameter)) return nullptr;
    return scopeOwner(parameter);
}
const GraphInstanceOwner *Graph::eventOwner(std::string_view input) const noexcept {
    return event_consumers_.contains(input) ? scopeOwner(input) : nullptr;
}
const GraphInstanceOwner *Graph::scopeOwner(std::string_view parameter) const noexcept {
    return graphInstanceOwnerOf(instance_owners_, parameter);
}

bool Graph::parameterShadowed(std::string_view parameter) const noexcept {
    return shadowed_parameters_.contains(parameter);
}

const GraphInstanceOwner *Graph::shadowedParameterOwner(std::string_view parameter) const noexcept {
    return parameterShadowed(parameter) ? scopeOwner(parameter) : nullptr;
}

std::string_view instanceInputSourceName(InstanceInputSource source) {
    switch (source) {
    case InstanceInputSource::connected: return "connected";
    case InstanceInputSource::parameter: return "parameter";
    case InstanceInputSource::defaulted: return "default";
    case InstanceInputSource::overridden: return "override";
    }
    throw std::invalid_argument("unknown instance input source");
}

bool Graph::nodeActive(std::string_view node, const InactiveScopes &inactive) const {
    return active(node_scopes_, node, inactive);
}

Graph::StateTransfer Graph::transferState(const Graph &previous, const EvaluationState &state) const {
    if (state.graph_ != &previous) throw std::invalid_argument("evaluation state belongs to a different graph");
    const auto definitions = [](const Graph &graph) {
        auto document = Json::parse(graph.serialize());
        if (document.at("schema") == "pelican.graph") {
            GraphInstanceOwners owners;
            ScopeDependencies nodes, outputs;
            std::vector<Graph::InstanceInputBinding> instance_inputs;
            document = lowerScalarGraph(document, *graph.modules_, owners, nodes, outputs, instance_inputs);
        }
        std::map<std::string, Json> result;
        for (const auto &node : document.at("nodes")) result.emplace(name(node.at("id")), node);
        return result;
    };
    const auto old_definitions = definitions(previous);
    const auto new_definitions = definitions(*this);
    std::map<std::string, std::size_t> old_indices;
    for (std::size_t index = 0; index < previous.nodes_.size(); ++index)
        old_indices.emplace(previous.nodes_[index].id, index);
    StateTransfer result{createState(), {}, {}};
    const std::set<std::string> no_scopes;
    const auto scopes = [&](const Graph &graph, const std::string &id) -> const auto & {
        const auto found = graph.node_scopes_.find(id);
        return found == graph.node_scopes_.end() ? no_scopes : found->second;
    };
    for (const auto index : order_) {
        const auto &node = nodes_[index];
        const auto old = old_indices.find(node.id);
        if (old == old_indices.end() || old_definitions.at(node.id) != new_definitions.at(node.id)) continue;
        const auto &owners = scopes(*this, node.id);
        if (owners != scopes(previous, node.id) || std::any_of(owners.begin(), owners.end(), [&](const auto &scope) {
            return instance_owners_.at(scope) != previous.instance_owners_.at(scope);
        })) continue;
        if (std::any_of(node.dependencies.begin(), node.dependencies.end(), [&](auto dependency) {
            return !result.retained_nodes.contains(nodes_[dependency].id);
        })) continue;
        result.retained_nodes.insert(node.id);
        if (node.op == Op::parameter) result.retained_parameters.insert(node.input_name);
        if (const auto trigger = state.triggers_.find(old->second); trigger != state.triggers_.end())
            result.state.triggers_.emplace(index, trigger->second);
        if (const auto events = state.pending_.find(old->second); events != state.pending_.end())
            result.state.pending_.emplace(index, events->second);
        if (const auto voice = state.voices_.find(old->second); voice != state.voices_.end())
            result.state.voices_.emplace(index, voice->second);
    }
    return result;
}

void Graph::resetState(EvaluationState &state, const InactiveScopes &scopes) const {
    if (state.graph_ != this) throw std::invalid_argument("evaluation state belongs to a different graph");
    std::erase_if(state.triggers_, [&](const auto &entry) { return !nodeActive(nodes_[entry.first].id, scopes); });
    std::erase_if(state.pending_, [&](const auto &entry) { return !nodeActive(nodes_[entry.first].id, scopes); });
    std::erase_if(state.voices_, [&](const auto &entry) { return !nodeActive(nodes_[entry.first].id, scopes); });
}
bool Graph::emitEvent(EvaluationState &state, std::string_view input,
                      Animation::CurvePlaybackCommand command, double time) const {
    if (state.graph_ != this) throw std::invalid_argument("evaluation state belongs to a different graph");
    if (!std::isfinite(time)) throw std::invalid_argument("event time must be finite");
    switch (command) {
    case Animation::CurvePlaybackCommand::start:
    case Animation::CurvePlaybackCommand::pause:
    case Animation::CurvePlaybackCommand::resume:
    case Animation::CurvePlaybackCommand::reset: break;
    default: throw std::invalid_argument("unknown playback event command");
    }
    const auto found = event_consumers_.find(input);
    if (found == event_consumers_.end()) return false;
    for (const auto consumer : found->second) {
        auto &queue = state.pending_[consumer];
        const auto position = std::upper_bound(queue.begin(), queue.end(), time,
            [](double time, const auto &event) { return time < event.time; });
        queue.insert(position, {time, command});
    }
    return true;
}

Outputs Graph::evaluate(const Inputs &inputs, const ParameterValues &parameters, const InactiveScopes &inactive,
                        EvaluationState *state) const {
    auto result = evaluateInternal(inputs, parameters, inactive, state);
    for (const auto &output : lowered_outputs_) result.erase(output);
    return result;
}

Outputs Graph::evaluateLowered(const Inputs &inputs, const ParameterValues &parameters,
                               const InactiveScopes &inactive, EvaluationState *state) const {
    return evaluateInternal(inputs, parameters, inactive, state);
}

std::vector<std::string> Graph::nodeIds() const {
    std::vector<std::string> result;
    result.reserve(nodes_.size());
    for (const auto &node : nodes_) result.push_back(node.id);
    return result;
}

Outputs Graph::evaluateInternal(const Inputs &inputs, const ParameterValues &parameters, const InactiveScopes &inactive,
                                EvaluationState *state,
                                std::vector<std::optional<double>> *node_values) const {
    if (state && state->graph_ != this) throw std::invalid_argument("evaluation state belongs to a different graph");
    auto triggers = state ? state->triggers_ : std::map<std::size_t, EvaluationState::Trigger>{};
    auto pending = state ? state->pending_ : std::map<std::size_t, std::vector<EvaluationState::Event>>{};
    for (const auto &[name, value] : parameters) {
        if (!parameter_defaults_.contains(name)) throw std::invalid_argument("unknown graph parameter: " + name);
        if (!std::isfinite(value)) throw std::invalid_argument("graph parameter must be finite: " + name);
    }
    std::vector<double> values(nodes_.size());
    // Every node this evaluation actually ran, for a reader that wants the
    // values behind the projection. A node the inactive scopes skip below is
    // left absent rather than reported as the zero `values` was sized with.
    if (node_values) node_values->assign(nodes_.size(), std::nullopt);
    for (const auto index : order_) {
        const auto &node = nodes_[index];
        if (!nodeActive(node.id, inactive)) {
            triggers.erase(index);
            pending.erase(index);
            continue;
        }
        auto &value = values[index];
        switch (node.op) {
        case Op::input: {
            const auto found = inputs.find(node.input_name);
            if (found == inputs.end()) throw std::invalid_argument("missing value graph input: " + node.input_name);
            value = found->second;
            break;
        }
        case Op::constant: value = node.constant; break;
        case Op::parameter: {
            const auto found = parameters.find(node.input_name);
            value = found == parameters.end() ? node.constant : found->second;
            break;
        }
        case Op::add: value = values[node.dependencies[0]] + values[node.dependencies[1]]; break;
        case Op::multiply: value = values[node.dependencies[0]] * values[node.dependencies[1]]; break;
        case Op::curve: value = node.curve->sample(values[node.dependencies[0]]); break;
        case Op::sample_table: value = node.table->sample(node.table_row, values[node.dependencies[0]]); break;
        case Op::event_input: break;
        case Op::audio_playback:
            if (!state) throw statelessEvaluationRejected("audio.playback", node.id);
            value = values[node.dependencies[0]];
            break;
        case Op::event_clock: {
            if (!state) throw statelessEvaluationRejected("value.event_clock", node.id);
            auto &clock = triggers[index].clock;
            const auto time = values[node.dependencies[0]];
            auto &queue = pending[index];
            const auto end = std::upper_bound(queue.begin(), queue.end(), time,
                [](double time, const auto &event) { return time < event.time; });
            for (auto event = queue.begin(); event != end; ++event) {
                switch (event->command) {
                case Animation::CurvePlaybackCommand::start: clock.trigger(event->time); break;
                case Animation::CurvePlaybackCommand::pause: clock.pause(event->time); break;
                case Animation::CurvePlaybackCommand::resume: clock.resume(event->time); break;
                case Animation::CurvePlaybackCommand::reset: clock.stop(); break;
                }
            }
            queue.erase(queue.begin(), end);
            value = clock.elapsed(time);
            break;
        }
        case Op::trigger_clock: {
            if (!state) throw statelessEvaluationRejected("value.trigger_clock", node.id);
            auto &trigger = triggers[index];
            const auto time = values[node.dependencies[0]];
            const bool high = values[node.dependencies[1]] != 0;
            const bool paused = (node.has_pause ? values[node.dependencies[2]] : node.pause_default) != 0;
            const bool reset = (node.has_reset ? values[node.dependencies[2 + node.has_pause]] : node.reset_default) != 0;
            if (reset) trigger.clock.stop();
            else {
                if (high && !trigger.high) trigger.clock.trigger(time);
                if (paused) trigger.clock.pause(time);
                else trigger.clock.resume(time);
            }
            trigger.high = high;
            value = trigger.clock.elapsed(time);
            break;
        }
        case Op::playback: {
            auto settings = node.playback;
            for (std::size_t i = 0; i < node.playback_inputs.size(); ++i)
                settings.*node.playback_inputs[i] = values[node.dependencies[i + 1]];
            value = Animation::mapCurveTime(settings, values[node.dependencies[0]]).source_time;
            break;
        }
        }
        if (!std::isfinite(value)) throw std::invalid_argument("non-finite value at graph node: " + node.id);
        if (node_values) (*node_values)[index] = value;
    }
    Outputs result;
    for (const auto &[name, indices] : outputs_) {
        if (!active(output_scopes_, name, inactive)) continue;
        auto &output = result[name];
        output.reserve(indices.size());
        for (const auto index : indices) output.push_back(values[index]);
    }
    if (state) {
        state->triggers_ = std::move(triggers);
        state->pending_ = std::move(pending);
    }
    return result;
}

std::vector<Graph::RenderBufferBinding> Graph::renderBufferBindings() const {
    std::vector<RenderBufferBinding> result;
    for (const auto &binding : bindings_) {
        if (binding.type != Target::render_buffer) continue;
        const auto count = outputs_.at(binding.output).size();
        const auto shared = std::find_if(result.begin(), result.end(),
                                         [&](const RenderBufferBinding &entry) { return entry.target == binding.target; });
        if (shared != result.end()) shared->element_count += count;
        else result.push_back({binding.target, count});
    }
    return result;
}

std::set<std::string> Graph::inputNames() const {
    std::set<std::string> result;
    for (const auto &node : nodes_)
        if (node.op == Op::input) result.insert(node.input_name);
    return result;
}

PublishResult Graph::publish(const GameContext &context, const Inputs &inputs,
                             const SoundBindings &sounds, const RenderValueWriter &render_writer,
                             const ParameterValues &parameters, const InactiveScopes &inactive, EvaluationState *state,
                             const AudioVoiceWriter &voice_writer, const SoundResolver &sound_resolver,
                             const AudioValueWriter &audio_value_writer,
                             const UiValueWriter &ui_writer) const {
    PublishResult result;
    const auto outputs = evaluateInternal(inputs, parameters, inactive, state, &result.node_values);
    // Convert and validate every value before changing any service.
    std::vector<std::vector<float>> prepared;
    prepared.reserve(bindings_.size());
    for (const auto &binding : bindings_) {
        auto &values = prepared.emplace_back();
        const auto output = outputs.find(binding.output);
        if (output == outputs.end()) continue;
        for (const auto value : output->second) {
            if (std::abs(value) > (std::numeric_limits<float>::max)())
                throw std::out_of_range("graph output exceeds float32 range: " + binding.output);
            values.push_back(static_cast<float>(value));
        }
        if ((binding.type == Target::sound_volume && values[0] < 0) ||
            (binding.type == Target::sound_pitch && values[0] <= 0))
            throw std::invalid_argument("invalid audio graph output: " + binding.output);
    }
    // Keep the drawn generation's instance input values before any service is
    // touched. An inactive instance publishes no output, so its ports are
    // absent rather than stale.
    for (const auto &binding : instance_inputs_) {
        const auto found = outputs.find(binding.output);
        if (found == outputs.end() || found->second.empty()) continue;
        InstanceInputValue value{binding.source, found->second.front(), binding.node,
                                 binding.port, binding.parameter};
        if (binding.source != InstanceInputSource::connected && parameters.contains(binding.parameter))
            value.source = InstanceInputSource::overridden;
        result.instance_inputs.emplace(binding.key, std::move(value));
    }
    for (const auto index : audio_nodes_) {
        const auto &node = nodes_[index];
        const auto time = outputs.find("$audio_time/" + node.id);
        if (time == outputs.end()) {
            state->voices_.erase(index);
            continue;
        }
        const auto now = time->second[0];
        using Phase = AudioVoiceDirective::Phase;
        auto &voice = state->voices_[index];
        const auto start = [&](double at) {
            voice.phase = Phase::playing;
            voice.started = at;
            ++voice.epoch;
            voice.last_position.reset();
            voice.last_elapsed.reset();
        };
        // The state machine of decision 3. Events arrive in time order; a node
        // without events plays from its owner's first frame.
        if (node.audio_events) {
            auto &queue = state->pending_[index];
            while (!queue.empty() && queue.front().time <= now) {
                const auto &event = queue.front();
                switch (event.command) {
                case Animation::CurvePlaybackCommand::start: start(event.time); break;
                case Animation::CurvePlaybackCommand::pause:
                    if (voice.phase == Phase::playing) {
                        voice.phase = Phase::paused;
                        voice.paused_at = event.time;
                    }
                    break;
                case Animation::CurvePlaybackCommand::resume:
                    if (voice.phase == Phase::paused) {
                        voice.phase = Phase::playing;
                        voice.started += event.time - voice.paused_at;
                    }
                    break;
                case Animation::CurvePlaybackCommand::reset: voice.phase = Phase::idle; break;
                }
                queue.erase(queue.begin());
                ++result.applied;
            }
        } else if (voice.phase == Phase::idle) start(now);
        AudioVoiceDirective directive;
        directive.phase = voice.phase;
        directive.epoch = voice.epoch;
        directive.bus = node.audio_bus;
        const auto value = [&](std::size_t dependency) {
            const auto &found = result.node_values[node.dependencies[dependency]];
            if (!found) throw std::logic_error("audio playback input was not evaluated: " + node.id);
            return *found;
        };
        if (node.audio_position) {
            directive.follows_position = true;
            directive.position = value(node.audio_events ? 2 : 1);
            directive.advancing = !voice.last_position || *voice.last_position != directive.position;
            voice.last_position = directive.position;
        } else {
            directive.mapping = node.playback;
            for (std::size_t i = 0; i < node.playback_inputs.size(); ++i)
                directive.mapping.*node.playback_inputs[i] = value(node.playback_input_base + i);
            directive.end_from_source = node.audio_end_from_source;
            directive.native = node.audio_native;
            directive.elapsed = voice.phase == Phase::idle ? 0.0
                : (voice.phase == Phase::paused ? voice.paused_at : now) - voice.started;
            directive.advancing = !voice.last_elapsed || *voice.last_elapsed != directive.elapsed;
            voice.last_elapsed = directive.elapsed;
        }
        if (!voice_writer ? directive.phase != Phase::idle : !voice_writer(node.id, directive))
            result.unavailable.push_back("audio.playback:" + node.id);
    }
    std::set<std::string_view> staged_buffers;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const auto &binding = bindings_[i];
        if (binding.type == Target::render_buffer) {
            // RS-15: the sinks sharing a buffer tile it (checked when the
            // graph was read), so they stage it as one write, in offset order.
            if (!staged_buffers.insert(binding.target).second) continue;
            std::vector<std::size_t> sinks;
            for (std::size_t j = i; j < bindings_.size(); ++j)
                if (bindings_[j].type == Target::render_buffer && bindings_[j].target == binding.target)
                    sinks.push_back(j);
            const auto evaluated = static_cast<std::size_t>(std::count_if(
                sinks.begin(), sinks.end(), [&](std::size_t j) { return !prepared[j].empty(); }));
            if (evaluated == 0) continue;
            if (evaluated != sinks.size()) {
                // Part of the buffer has no value this frame (an inactive
                // owner's sink); a partial write cannot be staged.
                result.unavailable.push_back(binding.kind + ":" + binding.target);
                continue;
            }
            std::stable_sort(sinks.begin(), sinks.end(),
                             [&](std::size_t a, std::size_t b) { return bindings_[a].offset < bindings_[b].offset; });
            std::vector<float> joined;
            if (sinks.size() > 1)
                for (const auto j : sinks) joined.insert(joined.end(), prepared[j].begin(), prepared[j].end());
            const std::span<const float> values = sinks.size() > 1 ? std::span<const float>{joined}
                                                                    : std::span<const float>{prepared[i]};
            const bool applied = render_writer ? render_writer(binding.target, values)
                                               : context.render().setRenderValues(binding.target, values);
            if (applied) result.applied += sinks.size();
            else result.unavailable.push_back(binding.kind + ":" + binding.target);
            continue;
        }
        if (prepared[i].empty()) continue;
        bool applied = false;
        if (binding.type == Target::ui_value || binding.type == Target::ui_visible)
            // Only the caller holds the UI; without a writer the sink is
            // reported unavailable rather than dropped.
            applied = ui_writer && ui_writer(binding.target,
                binding.type == Target::ui_value ? UiSinkProperty::value : UiSinkProperty::visible,
                prepared[i][0]);
        else if (audio_value_writer) {
            applied = audio_value_writer(binding.target, binding.voice,
                binding.type == Target::sound_volume ? AudioValueKind::volume : AudioValueKind::pitch,
                prepared[i][0]);
        } else if (binding.voice && sound_resolver) {
            const auto sound = sound_resolver(binding.target);
            // A connected producer can exist before start, or after reset/end.
            if (!sound) continue;
            applied = binding.type == Target::sound_volume
                ? context.audio().setSoundVolume(*sound, prepared[i][0])
                : context.audio().setSoundPitch(*sound, prepared[i][0]);
        } else if (!binding.voice) {
            const auto sound = sounds.find(binding.target);
            if (sound != sounds.end())
                applied = binding.type == Target::sound_volume
                    ? context.audio().setSoundVolume(sound->second, prepared[i][0])
                    : context.audio().setSoundPitch(sound->second, prepared[i][0]);
        }
        if (applied) ++result.applied;
        else result.unavailable.push_back(binding.kind + ":" + binding.target);
    }
    return result;
}

} // namespace Pelican::ValueGraph
