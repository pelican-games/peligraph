#pragma once

#include "animation/curve.hpp"
#include "export.hpp"
#include "../../project/graphinstanceowner.hpp"
#include <cstdint>
#include <handle.hpp>
#include <functional>
#include <span>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican { class GameContext; class GraphModuleRegistry; }
namespace Pelican::ValueGraph {

using Inputs = std::map<std::string, double, std::less<>>;
using ParameterValues = Inputs;
using InactiveScopes = std::set<std::string>;
using ScopeDependencies = std::map<std::string, std::set<std::string>>;
using Outputs = std::map<std::string, std::vector<double>, std::less<>>;
using SoundBindings = std::map<std::string, SoundHandle, std::less<>>;
using RenderValueWriter = std::function<bool(std::string_view, std::span<const float>)>;
using AudioEventWriter = std::function<bool(std::string_view, Animation::CurvePlaybackCommand)>;
using SoundResolver = std::function<std::optional<SoundHandle>(std::string_view)>;
enum class AudioValueKind { volume, pitch };
using AudioValueWriter = std::function<bool(std::string_view, bool, AudioValueKind, float)>;
// Where an instance input port's value came from in one evaluation.
// `connected` is an edge in the authoring document; `parameter` and `default`
// are the document's stored scalar and the definition's declared default;
// `overridden` is a live setGraphParameter on top of one of those two. An edge
// removes the parameter the override would have written, so the two never
// combine -- setGraphParameter says so by name instead of going quiet.
enum class InstanceInputSource : std::uint8_t { connected, parameter, defaulted, overridden };
std::string_view instanceInputSourceName(InstanceInputSource source);
struct InstanceInputValue {
    InstanceInputSource source = InstanceInputSource::defaulted;
    double value{};
    // connected: the producing node and port. Otherwise empty.
    std::string node;
    std::string port;
    // parameter/default/overridden: the value.parameter name. Otherwise empty.
    std::string parameter;
};
// Keyed by `<instance path>/<port>`.
using InstanceInputValues = std::map<std::string, InstanceInputValue, std::less<>>;
class Graph;
struct PublishResult {
    std::size_t applied{};
    std::vector<std::string> unavailable;
    // What this evaluation actually fed each instance input port. Filled by the
    // publishing pass so a tool can read the drawn generation's values without
    // evaluating the graph a second time -- a second evaluation would advance
    // the clocks and consume the events (see EvaluationState).
    InstanceInputValues instance_inputs;
    // Every node's value this evaluation, in the graph's own node order. The
    // arithmetic is already done -- the evaluation computes every node before
    // projecting the declared outputs -- so this costs one vector and no node
    // names; resolving a name is the reader's job, once, rather than the draw
    // path's, every frame. A node the frame's inactive scopes skipped has no
    // value: absent, rather than a stale or invented zero.
    std::vector<std::optional<double>> node_values;
};
// The same values, stamped with the draw generation that produced them, as one
// engine-owned snapshot a tool can read.
struct PublishedInstanceInputs {
    bool published = false;
    // RendererRuntimeGeneration::generation: the version of the graph structure.
    std::uint64_t generation = 0;
    // EngineTime::frameIndex(): the frame that evaluated these values.
    std::uint64_t frame = 0;
    InstanceInputValues inputs;
    // The graph `node_values` is ordered by. Held, not borrowed: a reader that
    // arrives after the drawn generation was replaced still has the names that
    // make those values readable, and they are that generation's names.
    std::shared_ptr<const Graph> graph;
    std::vector<std::optional<double>> node_values;
};
class EvaluationState {
    friend class Graph;
    explicit EvaluationState(const Graph *graph) : graph_(graph) {}
    const Graph *graph_;
    struct Trigger {
        Animation::CurveTriggerClock clock;
        bool high{};
    };
    std::map<std::size_t, Trigger> triggers_;
    struct Event {
        double time;
        Animation::CurvePlaybackCommand command;
    };
    std::map<std::size_t, std::vector<Event>> pending_;
};

// Immutable scalar dataflow. Stateful nodes update an explicit EvaluationState;
// evaluate never publishes values to external services.
class PELICAN_API Graph {
  public:
    explicit Graph(std::string_view document);
    // Compile against a specific set of registered modules. Production uses the
    // built-in registry; a caller that added or removed a module passes its own.
    Graph(std::string_view document, const GraphModuleRegistry &modules);
    const std::string &serialize() const noexcept { return document_; }
    const ParameterValues &parameterDefaults() const noexcept { return parameter_defaults_; }
    const GraphInstanceOwner *parameterOwner(std::string_view parameter) const noexcept;
    const GraphInstanceOwner *eventOwner(std::string_view input) const noexcept;
    const GraphInstanceOwners &instanceOwners() const noexcept { return instance_owners_; }
    const ScopeDependencies &scopeDependencies() const noexcept { return node_scopes_; }
    const std::map<std::string, std::string> &audioSources() const noexcept { return audio_sources_; }
    struct MorphWeightSource {
        GraphInstanceOwner owner;
        std::uint32_t node;
        std::uint32_t target;
    };
    const std::map<std::string, MorphWeightSource> &morphSources() const noexcept { return morph_sources_; }
    // One instance input port and the lowered output carrying its value.
    struct InstanceInputBinding {
        std::string key;    // "<instance path>/<port>"
        std::string output; // the lowered output this graph publishes for it
        InstanceInputSource source = InstanceInputSource::defaulted;
        std::string node;
        std::string port;
        std::string parameter;
    };
    const std::vector<InstanceInputBinding> &instanceInputs() const noexcept { return instance_inputs_; }
    // True when `parameter` names an instance input port that an edge supplies.
    // Such a port has no value.parameter node, so an override cannot reach it.
    bool parameterShadowed(std::string_view parameter) const noexcept;
    // The owner of a shadowed port, so a caller can tell "an edge took this
    // over" apart from "no such parameter" without knowing either graph.
    const GraphInstanceOwner *shadowedParameterOwner(std::string_view parameter) const noexcept;
    struct RenderBufferBinding {
        std::string target;
        std::size_t element_count;
    };
    // Static output shape; inspecting bindings does not evaluate live inputs.
    std::vector<RenderBufferBinding> renderBufferBindings() const;
    std::set<std::string> inputNames() const;
    bool nodeActive(std::string_view node, const InactiveScopes &inactive) const;
    // The graph must outlive its state. Separate states evaluate independently.
    EvaluationState createState() const { return EvaluationState{this}; }
    struct StateTransfer {
        EvaluationState state;
        std::set<std::string> retained_nodes;
        std::set<std::string> retained_parameters;
    };
    // Preserve only unchanged nodes whose dependencies and owners also match.
    StateTransfer transferState(const Graph &previous, const EvaluationState &state) const;
    void resetState(EvaluationState &state, const InactiveScopes &scopes) const;
    bool emitEvent(EvaluationState &state, std::string_view input,
                   Animation::CurvePlaybackCommand command, double time) const;
    Outputs evaluate(const Inputs &inputs, const ParameterValues &parameters = {},
                     const InactiveScopes &inactive = {}, EvaluationState *state = nullptr) const;
    // The same evaluation, keeping the outputs lowering added rather than the
    // authored ones alone: the publication sinks and the scalars an
    // animation-domain node consumes this frame. A caller that reads one of
    // those runs before the sinks do, so the feed stays one-way.
    Outputs evaluateLowered(const Inputs &inputs, const ParameterValues &parameters = {},
                            const InactiveScopes &inactive = {},
                            EvaluationState *state = nullptr) const;
    // Owner-thread evaluation and explicit service publication. Sound targets
    // are symbolic names, resolved using this instance's live handles.
    // Missing/stopped targets are reported; other bindings can still apply.
    // A renderer may supply a writer resolving names against its retained
    // generation instead of the current global resource name table.
    PublishResult publish(const GameContext &context, const Inputs &inputs,
                          const SoundBindings &sounds = {},
                          const RenderValueWriter &render_writer = {},
                          const ParameterValues &parameters = {},
                          const InactiveScopes &inactive = {}, EvaluationState *state = nullptr,
                          const AudioEventWriter &audio_writer = {},
                          const SoundResolver &sound_resolver = {},
                          const AudioValueWriter &audio_value_writer = {}) const;

    const GraphInstanceOwner *scopeOwner(std::string_view node) const noexcept;

    // The nodes of the values partition, in the order publish() reports their
    // values -- expanded ids, including the ones lowering synthesised. Built on
    // demand: the draw path never spells these names, so a reader that wants
    // them pays for them once per read instead of once per frame.
    std::vector<std::string> nodeIds() const;
    // Every output name this graph projects, authored and lowered alike, and
    // the nodes each one reads. It is how a reader holding a publish snapshot
    // resolves `$pose/<node>/<port>` -- the scalar an animation node consumed
    // this frame -- without evaluating the graph a second time.
    const std::map<std::string, std::vector<std::size_t>, std::less<>> &outputNodes() const noexcept {
        return outputs_;
    }

  private:
    Outputs evaluateInternal(const Inputs &inputs, const ParameterValues &parameters,
                             const InactiveScopes &inactive, EvaluationState *state,
                             std::vector<std::optional<double>> *node_values = nullptr) const;
    // The modules this graph compiled against; they must outlive it, and
    // re-lowering the saved document uses the same set.
    const GraphModuleRegistry *modules_ = nullptr;
    ParameterValues parameter_defaults_;
    GraphInstanceOwners instance_owners_;
    ScopeDependencies node_scopes_;
    ScopeDependencies output_scopes_;
    std::vector<std::string> lowered_outputs_;
    std::vector<InstanceInputBinding> instance_inputs_;
    std::set<std::string, std::less<>> shadowed_parameters_;
    enum class Op { input, parameter, constant, add, multiply, curve, playback, trigger_clock, event_input, event_clock, audio_playback };
    std::map<std::string, std::string> audio_sources_;
    std::map<std::string, MorphWeightSource> morph_sources_;
    std::vector<std::size_t> audio_nodes_;
    std::map<std::string, std::vector<std::size_t>, std::less<>> event_consumers_;
    struct Node {
        std::string id;
        Op op{};
        std::string input_name;
        double constant{};
        std::vector<std::size_t> dependencies;
        std::optional<Animation::ScalarCurve> curve;
        Animation::CurvePlaybackSettings playback;
        std::vector<double Animation::CurvePlaybackSettings::*> playback_inputs;
        bool has_pause{};
        bool has_reset{};
        double pause_default{};
        double reset_default{};
    };
    std::string document_;
    std::vector<Node> nodes_;
    std::vector<std::size_t> order_;
    std::map<std::string, std::vector<std::size_t>, std::less<>> outputs_;
    enum class Target { render_buffer, sound_volume, sound_pitch };
    struct Binding {
        std::string output;
        std::string target;
        std::string kind;
        Target type{};
        bool voice{};
    };
    std::vector<Binding> bindings_;
};

} // namespace Pelican::ValueGraph
