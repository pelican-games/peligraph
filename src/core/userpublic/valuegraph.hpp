#pragma once

#include "animation/curve.hpp"
#include "export.hpp"
#include "../../project/graphinstanceowner.hpp"
#include <handle.hpp>
#include <functional>
#include <span>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican { class GameContext; }
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
struct PublishResult {
    std::size_t applied{};
    std::vector<std::string> unavailable;
};

class Graph;
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
    bool nodeActive(std::string_view node, const InactiveScopes &inactive) const;
    // The graph must outlive its state. Separate states evaluate independently.
    EvaluationState createState() const { return EvaluationState{this}; }
    void resetState(EvaluationState &state, const InactiveScopes &scopes) const;
    bool emitEvent(EvaluationState &state, std::string_view input,
                   Animation::CurvePlaybackCommand command, double time) const;
    Outputs evaluate(const Inputs &inputs, const ParameterValues &parameters = {},
                     const InactiveScopes &inactive = {}, EvaluationState *state = nullptr) const;
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
                          const SoundResolver &sound_resolver = {}) const;

  private:
    const GraphInstanceOwner *scopeOwner(std::string_view node) const noexcept;
    Outputs evaluateInternal(const Inputs &inputs, const ParameterValues &parameters,
                             const InactiveScopes &inactive, EvaluationState *state) const;
    ParameterValues parameter_defaults_;
    GraphInstanceOwners instance_owners_;
    ScopeDependencies node_scopes_;
    ScopeDependencies output_scopes_;
    std::vector<std::string> lowered_outputs_;
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
