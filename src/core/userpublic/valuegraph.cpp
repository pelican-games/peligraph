#include "valuegraph.hpp"
#include "gamecontext.hpp"
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

Json lowerScalarGraph(const Json &authored) {
    const auto expanded = partitionGraphDocument(authored).values;
    const auto registry = makeBuiltinLogicalTypeRegistry();
    const auto operations = valueGraphOperationContracts(registry);
    Json result{{"schema", "pelican.value_graph"}, {"version", 1},
                {"nodes", Json::array()}, {"outputs", Json::object()}, {"bindings", Json::array()}};
    const auto source = [](const Json &endpoint) {
        if (endpoint.at("port") != "value")
            throw std::invalid_argument("scalar backend requires a value output port");
        return endpoint.at("node");
    };
    for (const auto &[output, endpoint] : expanded.outputs.items())
        result["outputs"][output] = Json::array({source(endpoint)});
    for (const auto &node : expanded.nodes) {
        const auto op = name(node.at("op"));
        const auto parameters = node.value("parameters", Json::object());
        const auto &inputs = node.at("inputs");
        for (const auto &[port, contract] : operations.at(op).inputs)
            if (!contract.required && parameters.contains(port))
                (void)number(parameters.at(port)); // References belong in typed input edges.
        if (op == "render.scalar" || op == "audio.volume" || op == "audio.pitch") {
            keys(parameters, {"target"});
            const auto output = "$binding/" + name(node.at("id"));
            if (result["outputs"].contains(output)) throw std::invalid_argument("reserved graph output: " + output);
            result["outputs"][output] = Json::array({source(inputs.at("value"))});
            result["bindings"].push_back({{"output", output},
                {"kind", op == "render.scalar" ? "render_buffer" : op == "audio.volume" ? "sound_volume" : "sound_pitch"},
                {"target", name(parameters.at("target"))}});
            continue;
        }
        if (!parameters.is_object()) throw std::invalid_argument("node parameters must be an object");
        Json lowered = parameters;
        if (lowered.contains("id") || lowered.contains("op"))
            throw std::invalid_argument("node parameters cannot replace id/op");
        lowered["id"] = node.at("id");
        lowered["op"] = op.substr(6);
        for (const auto &[port, endpoint] : inputs.items()) {
            if (lowered.contains(port)) throw std::invalid_argument("input also defined as a parameter: " + port);
            lowered[port] = source(endpoint);
        }
        result["nodes"].push_back(std::move(lowered));
    }
    return result;
}
}

Graph::Graph(std::string_view document) {
    const auto authored = Json::parse(document);
    const auto root = authored.value("schema", std::string{}) == "pelican.graph"
        ? lowerScalarGraph(authored) : authored;
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
        if (op == "input") {
            keys(authored, {"id", "op", "name"});
            node.op = Op::input;
            node.input_name = name(authored.at("name"));
        } else if (op == "constant") {
            keys(authored, {"id", "op", "value"});
            node.op = Op::constant;
            node.constant = number(authored.at("value"));
        } else if (op == "add" || op == "multiply") {
            keys(authored, {"id", "op", "a", "b"});
            node.op = op == "add" ? Op::add : Op::multiply;
            node.dependencies = {resolve(authored.at("a")), resolve(authored.at("b"))};
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
        } else throw std::invalid_argument("unknown value graph operation: " + op);
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
        for (const auto &reference : references) outputs_[output].push_back(resolve(reference));
    }
    if (const auto bindings = root.find("bindings"); bindings != root.end()) {
        if (!bindings->is_array()) throw std::invalid_argument("value graph bindings must be an array");
        std::set<std::pair<std::string, std::string>> targets;
        for (const auto &binding : *bindings) {
            keys(binding, {"output", "kind", "target"});
            Binding decoded;
            decoded.output = name(binding.at("output"));
            decoded.target = name(binding.at("target"));
            decoded.kind = name(binding.at("kind"));
            if (decoded.kind == "render_buffer") decoded.type = Target::render_buffer;
            else if (decoded.kind == "sound_volume") decoded.type = Target::sound_volume;
            else if (decoded.kind == "sound_pitch") decoded.type = Target::sound_pitch;
            else throw std::invalid_argument("unknown value graph binding kind: " + decoded.kind);
            const auto output = outputs_.find(decoded.output);
            if (output == outputs_.end()) throw std::invalid_argument("binding references unknown output: " + decoded.output);
            if (decoded.type != Target::render_buffer && output->second.size() != 1)
                throw std::invalid_argument("audio binding requires one scalar output");
            if (!targets.emplace(decoded.kind, decoded.target).second)
                throw std::invalid_argument("duplicate value graph target: " + decoded.target);
            bindings_.push_back(std::move(decoded));
        }
    }
    document_ = authored.dump();
}

Outputs Graph::evaluate(const Inputs &inputs) const {
    auto result = evaluateInternal(inputs);
    for (const auto &output : lowered_outputs_) result.erase(output);
    return result;
}

Outputs Graph::evaluateInternal(const Inputs &inputs) const {
    std::vector<double> values(nodes_.size());
    for (const auto index : order_) {
        const auto &node = nodes_[index];
        auto &value = values[index];
        switch (node.op) {
        case Op::input: {
            const auto found = inputs.find(node.input_name);
            if (found == inputs.end()) throw std::invalid_argument("missing value graph input: " + node.input_name);
            value = found->second;
            break;
        }
        case Op::constant: value = node.constant; break;
        case Op::add: value = values[node.dependencies[0]] + values[node.dependencies[1]]; break;
        case Op::multiply: value = values[node.dependencies[0]] * values[node.dependencies[1]]; break;
        case Op::curve: value = node.curve->sample(values[node.dependencies[0]]); break;
        case Op::playback: {
            auto settings = node.playback;
            for (std::size_t i = 0; i < node.playback_inputs.size(); ++i)
                settings.*node.playback_inputs[i] = values[node.dependencies[i + 1]];
            value = Animation::mapCurveTime(settings, values[node.dependencies[0]]).source_time;
            break;
        }
        }
        if (!std::isfinite(value)) throw std::invalid_argument("non-finite value at graph node: " + node.id);
    }
    Outputs result;
    for (const auto &[name, indices] : outputs_) {
        auto &output = result[name];
        output.reserve(indices.size());
        for (const auto index : indices) output.push_back(values[index]);
    }
    return result;
}

PublishResult Graph::publish(const GameContext &context, const Inputs &inputs,
                             const SoundBindings &sounds, const RenderValueWriter &render_writer) const {
    const auto outputs = evaluateInternal(inputs);
    // Convert and validate every value before changing any service.
    std::vector<std::vector<float>> prepared;
    prepared.reserve(bindings_.size());
    for (const auto &binding : bindings_) {
        auto &values = prepared.emplace_back();
        for (const auto value : outputs.at(binding.output)) {
            if (std::abs(value) > (std::numeric_limits<float>::max)())
                throw std::out_of_range("graph output exceeds float32 range: " + binding.output);
            values.push_back(static_cast<float>(value));
        }
        if ((binding.type == Target::sound_volume && values[0] < 0) ||
            (binding.type == Target::sound_pitch && values[0] <= 0))
            throw std::invalid_argument("invalid audio graph output: " + binding.output);
    }
    PublishResult result;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const auto &binding = bindings_[i];
        bool applied = false;
        if (binding.type == Target::render_buffer)
            applied = render_writer ? render_writer(binding.target, prepared[i])
                                    : context.setRenderValues(binding.target, prepared[i]);
        else if (const auto sound = sounds.find(binding.target); sound != sounds.end()) {
            applied = binding.type == Target::sound_volume
                ? context.setSoundVolume(sound->second, prepared[i][0])
                : context.setSoundPitch(sound->second, prepared[i][0]);
        }
        if (applied) ++result.applied;
        else result.unavailable.push_back(binding.kind + ":" + binding.target);
    }
    return result;
}

} // namespace Pelican::ValueGraph
