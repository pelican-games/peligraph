#pragma once

#include "animation/curve.hpp"
#include "export.hpp"
#include <handle.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican { class GameContext; }
namespace Pelican::ValueGraph {

using Inputs = std::map<std::string, double, std::less<>>;
using Outputs = std::map<std::string, std::vector<double>, std::less<>>;
using SoundBindings = std::map<std::string, SoundHandle, std::less<>>;
struct PublishResult {
    std::size_t applied{};
    std::vector<std::string> unavailable;
};

// Immutable scalar dataflow. Inputs supply clocks or other external values;
// evaluation does not advance a clock or perform side effects.
class PELICAN_API Graph {
  public:
    explicit Graph(std::string_view document);
    const std::string &serialize() const noexcept { return document_; }
    Outputs evaluate(const Inputs &inputs) const;
    // Owner-thread evaluation and explicit service publication. Sound targets
    // are symbolic names, resolved using this instance's live handles.
    // Missing/stopped targets are reported; other bindings can still apply.
    PublishResult publish(const GameContext &context, const Inputs &inputs,
                          const SoundBindings &sounds = {}) const;

  private:
    enum class Op { input, constant, add, multiply, curve, playback };
    struct Node {
        std::string id;
        Op op{};
        std::string input_name;
        double constant{};
        std::vector<std::size_t> dependencies;
        std::optional<Animation::ScalarCurve> curve;
        Animation::CurvePlaybackSettings playback;
        std::vector<double Animation::CurvePlaybackSettings::*> playback_inputs;
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
    };
    std::vector<Binding> bindings_;
};

} // namespace Pelican::ValueGraph
