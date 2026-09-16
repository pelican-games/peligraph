#pragma once

#include "../../project/graphinstanceowner.hpp"
#include "../../project/graphmodule.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican::Animation {

// The backend for pelican.pose. The contracts live in src/project; this is the
// half that knows how to run them, and it exists only when the animation unit
// is built.
enum class PoseGraphOp : std::uint8_t {
    clip_sample,
    blend,
    state_machine,
    retarget,
    skeletal_sink,
    transform_sink,
};

struct PoseGraphNode {
    std::string id;
    PoseGraphOp op = PoseGraphOp::clip_sample;
    // Which authored object the node belongs to. Every node in one sink's chain
    // shares it; the sink is the single writer for that object.
    GraphInstanceOwner owner;
    std::string rig;     // clip_sample / state_machine: the model asset
    std::string clip;    // clip_sample
    std::string profile; // retarget
    nlohmann::json state_machine;
    // Declaration order, so a port and its parameter cannot drift apart.
    std::vector<std::string> parameter_ports;
    // Port -> the values-partition output that carries this frame's scalar.
    std::map<std::string, std::string> scalar_inputs;
    // Port -> index into nodes(). Poses never travel as bytes on a port.
    std::map<std::string, std::size_t> pose_inputs;
    // The model asset whose layout this node's pose is produced for, walked
    // back through the chain. Two chains that disagree cannot be blended.
    std::string resolved_rig;
};

// One compiled animation partition. Construction is the fail-fast point: an
// operation whose parameters are unusable, a cycle, a chain with no owner, or a
// second writer on one object is rejected here and never reaches a frame.
class PoseGraph {
  public:
    PoseGraph(const nlohmann::json &authored, const GraphModuleRegistry &modules);
    explicit PoseGraph(const nlohmann::json &authored);

    bool empty() const noexcept { return nodes_.empty(); }
    const std::vector<PoseGraphNode> &nodes() const noexcept { return nodes_; }
    // Dependency order. Evaluating in this order visits every producer first.
    const std::vector<std::size_t> &order() const noexcept { return order_; }
    // Indices of the sink nodes, in owner order.
    const std::vector<std::size_t> &sinks() const noexcept { return sinks_; }
    // The values-partition outputs this graph reads this frame.
    const std::vector<std::string> &scalarOutputs() const noexcept { return scalar_outputs_; }
    // The node whose pose a sink publishes, walking through the chain.
    // A state-machine-driven sink reports its evaluator node.
    const PoseGraphNode *stateMachineFor(std::size_t sink) const;

    // pose.sink_conflict@1 when `owner` already has a writer. Raised for a
    // second sink in the document and, at attach time, for an animation
    // component that is already driving the same object.
    [[noreturn]] static void raiseSinkConflict(const GraphInstanceOwner &owner,
                                               const std::string &first, const std::string &second);
    // pose.layout_mismatch@1 when two poses on one operation were produced for
    // different pose layouts: a different rig at compile time, or a different
    // PoseLayout generation once the objects are live.
    [[noreturn]] static void raiseLayoutMismatch(const std::string &node, const std::string &port,
                                                 nlohmann::json expected, nlohmann::json observed);

  private:
    void compile(const nlohmann::json &authored, const GraphModuleRegistry &modules);
    std::vector<PoseGraphNode> nodes_;
    std::vector<std::size_t> order_;
    std::vector<std::size_t> sinks_;
    std::vector<std::string> scalar_outputs_;
};

} // namespace Pelican::Animation
