#include "valuegraph.hpp"
#include "gamecontext.hpp"
#include "../../project/graphmodule.hpp"
#include "../../project/graphpartition.hpp"

#include <algorithm>
#include <cmath>
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
    for (const auto &node : expanded.nodes) {
        const auto op = name(node.at("op"));
        const auto parameters = node.value("parameters", Json::object());
        const auto &inputs = node.at("inputs");
        if (op == "event.playback") {
            keys(parameters, {});
            result["nodes"].push_back({{"id", node.at("id")}, {"op", "event_input"}});
            continue;
        }
        if (op == "audio.playback") {
            keys(parameters, {"asset"});
            const auto id = name(node.at("id"));
            result["nodes"].push_back({{"id", id}, {"op", "audio_playback"},
                {"asset", name(parameters.at("asset"))}, {"time", source(inputs.at("time"))},
                {"events", inputs.at("events").at("node")}});
            const auto output = "$audio_time/" + id;
            output_scopes[output] = node_scopes.at(id);
            continue;
        }
        for (const auto &[port, contract] : modules.resolveOperation(node).inputs)
            if (!contract.required && parameters.contains(port))
                (void)number(parameters.at(port)); // References belong in typed input edges.
        if (op == "render.scalar" || op == "audio.volume" || op == "audio.pitch") {
            keys(parameters, {"target"});
            const bool voice = inputs.contains("voice");
            if (voice && parameters.contains("target"))
                throw std::invalid_argument("audio binding cannot specify both voice and target");
            const auto output = "$binding/" + name(node.at("id"));
            if (result["outputs"].contains(output)) throw std::invalid_argument("reserved graph output: " + output);
            result["outputs"][output] = Json::array({source(inputs.at("value"))});
            output_scopes[output] = node_scopes.at(name(node.at("id")));
            result["bindings"].push_back({{"output", output},
                {"kind", op == "render.scalar" ? "render_buffer" : op == "audio.volume" ? "sound_volume" : "sound_pitch"},
                {"target", voice ? name(inputs.at("voice").at("node")) : name(parameters.at("target"))},
                {"voice", voice}});
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
        } else if (op == "event_clock" || op == "audio_playback") {
            if (op == "event_clock") keys(authored, {"id", "op", "time", "events"});
            else {
                keys(authored, {"id", "op", "time", "events", "asset"});
                audio_sources_.emplace(node.id, name(authored.at("asset")));
                audio_nodes_.push_back(i);
            }
            node.op = op == "event_clock" ? Op::event_clock : Op::audio_playback;
            node.dependencies = {resolve(authored.at("time")), resolve(authored.at("events"))};
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
                !((node.op == Op::event_clock || node.op == Op::audio_playback) && input == 1))
                throw std::invalid_argument("event stream cannot be used as a scalar input");
        if (node.op != Op::event_clock && node.op != Op::audio_playback) continue;
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
        std::set<std::pair<std::string, std::string>> targets;
        for (const auto &binding : *bindings) {
            keys(binding, {"output", "kind", "target", "voice"});
            Binding decoded;
            decoded.output = name(binding.at("output"));
            decoded.target = name(binding.at("target"));
            decoded.kind = name(binding.at("kind"));
            if (decoded.kind == "render_buffer") decoded.type = Target::render_buffer;
            else if (decoded.kind == "sound_volume") decoded.type = Target::sound_volume;
            else if (decoded.kind == "sound_pitch") decoded.type = Target::sound_pitch;
            else throw std::invalid_argument("unknown value graph binding kind: " + decoded.kind);
            decoded.voice = binding.value("voice", false);
            if (decoded.voice && (decoded.type == Target::render_buffer || !audio_sources_.contains(decoded.target)))
                throw std::invalid_argument("voice binding requires an audio playback producer");
            const auto output = outputs_.find(decoded.output);
            if (output == outputs_.end()) throw std::invalid_argument("binding references unknown output: " + decoded.output);
            if (decoded.type != Target::render_buffer && output->second.size() != 1)
                throw std::invalid_argument("audio binding requires one scalar output");
            if (!targets.emplace(decoded.kind, decoded.target).second)
                throw std::invalid_argument("duplicate value graph target: " + decoded.target);
            bindings_.push_back(std::move(decoded));
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
    }
    return result;
}

void Graph::resetState(EvaluationState &state, const InactiveScopes &scopes) const {
    if (state.graph_ != this) throw std::invalid_argument("evaluation state belongs to a different graph");
    std::erase_if(state.triggers_, [&](const auto &entry) { return !nodeActive(nodes_[entry.first].id, scopes); });
    std::erase_if(state.pending_, [&](const auto &entry) { return !nodeActive(nodes_[entry.first].id, scopes); });
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
    for (const auto &binding : bindings_)
        if (binding.type == Target::render_buffer)
            result.push_back({binding.target, outputs_.at(binding.output).size()});
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
                             const AudioEventWriter &audio_writer, const SoundResolver &sound_resolver,
                             const AudioValueWriter &audio_value_writer) const {
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
        if (time == outputs.end()) continue;
        auto &queue = state->pending_[index];
        while (!queue.empty() && queue.front().time <= time->second[0]) {
            if (!audio_writer || !audio_writer(node.id, queue.front().command)) {
                result.unavailable.push_back("audio.playback:" + node.id);
                break;
            }
            queue.erase(queue.begin());
            ++result.applied;
        }
    }
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        if (prepared[i].empty()) continue;
        const auto &binding = bindings_[i];
        bool applied = false;
        if (binding.type == Target::render_buffer)
            applied = render_writer ? render_writer(binding.target, prepared[i])
                                    : context.render().setRenderValues(binding.target, prepared[i]);
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
