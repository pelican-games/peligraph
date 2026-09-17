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
    // clip_sample playback, the same three the animation component carries. The
    // defaults live in posegraphmodule.hpp; nothing re-states them here.
    double speed = 1.0;
    double start_time = 0.0;
    bool loop = true;
    // AG-2. `root_motion: "extract"` stops the clip's root translation from
    // sliding the mesh inside the instance and hands the per-frame difference
    // to the owning object's transform. `root_node` is the glTF node index that
    // translation belongs to; it is resolved to a layout index once, at attach.
    bool root_motion = false;
    std::uint32_t root_node = 0;
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
    // AG-2. pose.root_motion_unsupported@1: the extracting node's pose reaches
    // something that cannot hand a single root displacement to a single object
    // -- a blend, a state machine, no sink at all -- or the clip it names has
    // no duration or no translation on its root. pose.root_motion_shared@1: two
    // consumers, so two objects would move by one clip's root. Both are
    // decidable before a frame runs except the clip ones, which need the asset.
    [[noreturn]] static void raiseRootMotionUnsupported(const std::string &node,
                                                        const std::string &reason,
                                                        nlohmann::json detail);
    [[noreturn]] static void raiseRootMotionShared(const std::string &node, const std::string &first,
                                                    const std::string &second);
    // pose.root_node_unknown@1: the glTF node index the document names is not
    // one this rig's pose layout has.
    [[noreturn]] static void raiseRootNodeUnknown(const std::string &node, std::uint32_t root_node);
    // pose.root_motion_no_transform@1: the object the sink writes cannot take a
    // local transform, so there is nothing for the root displacement to land
    // on. Named at attach rather than silently dropping the motion.
    [[noreturn]] static void raiseRootMotionNoTransform(const GraphInstanceOwner &owner,
                                                         const std::string &node,
                                                         const std::string &object);

  private:
    void compile(const nlohmann::json &authored, const GraphModuleRegistry &modules);
    std::vector<PoseGraphNode> nodes_;
    std::vector<std::size_t> order_;
    std::vector<std::size_t> sinks_;
    std::vector<std::string> scalar_outputs_;
};

} // namespace Pelican::Animation
