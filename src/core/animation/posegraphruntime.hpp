#pragma once

#include "posegraph.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace Pelican::Animation {

// What the owner thread knows this frame and the pose graph does not: which
// authored objects are live, what they are called, whether an animation
// component is already writing them, and the scalars the values partition
// produced for the animation-domain inputs.
struct PoseGraphFrame {
    double time = 0.0;
    double delta_seconds = 0.0;
    std::uint64_t frame_revision = 0;
    std::uint64_t time_set_revision = 0;
    // Empty when the owner has no live object this frame.
    std::function<std::optional<std::string>(const GraphInstanceOwner &)> object_name;
    // True when the animation component's clip path has already published this
    // object's frame for `frame_revision`. It writes the renderer directly,
    // outside the service's single-writer bookkeeping, so the answer comes from
    // the writer token the publish left behind rather than from the document. A
    // graph sink on the same object is pose.sink_conflict@1, never a silent
    // second writer.
    std::function<bool(const GraphInstanceOwner &)> component_writer;
    // Confirms the object the owner resolves to really carries the model a
    // clip_sample or state_machine names, so a rig parameter cannot drift away
    // from the rig whose layout the chain was checked against.
    std::function<void(const GraphInstanceOwner &, const std::string &rig,
                       const std::string &node)> verify_rig;
    std::map<std::string, double, std::less<>> scalars;
};

// Attaches compiled pose graphs to live objects and evaluates them inside the
// animation phase. `synchronize` runs on the owner thread before the phases:
// it binds what became live, releases what did not, and captures one snapshot
// per graph. Evaluation itself happens once per frame revision, in the phase
// callback the attachment registered.
class PoseGraphRuntime {
  public:
    PoseGraphRuntime();
    ~PoseGraphRuntime();
    PoseGraphRuntime(const PoseGraphRuntime &) = delete;
    PoseGraphRuntime &operator=(const PoseGraphRuntime &) = delete;

    void synchronize(const std::shared_ptr<const PoseGraph> &graph, const PoseGraphFrame &frame);
    // Release every attachment whose graph is not one of `live` -- the pose
    // graphs of the programs the caller is about to synchronize. A generation
    // that is no longer published has no owner left to write, and leaving it
    // attached would leave its wrapped state machines claiming objects the next
    // generation is about to claim. Run it before synchronizing, so a stale
    // claim is gone before a new one is made.
    //
    // This is also where a state machine crosses from one generation to the
    // next. A retiring attachment's EvaluatorV1 is kept aside rather than
    // destroyed; the attachment that binds the same owner and the same node id
    // with the same anim_graph document adopts it through rebind(), keeping its
    // current state, clip cursors and transition progress instead of starting
    // over at the document's initial state. One that nothing adopts is released
    // at the next call, so a retired machine cannot keep claiming an object.
    void retainOnly(const std::set<const PoseGraph *> &live) noexcept;
    void clear() noexcept;

    struct SinkTrace {
        std::string sink;
        std::string node;
        std::uint64_t layout_identity = 0;
        std::uint32_t layout_generation = 0;
        std::uint64_t frame_revision = 0;
        std::uint64_t pose_hash = 0;
        // The state the wrapped state machine is in, when this sink is driven
        // by one. Empty for a sink with no pose.state_machine in its chain. It
        // is what says an edit kept the machine where it was instead of quietly
        // starting it over at the document's initial state.
        std::string state;
    };
    // Deterministic record of the last evaluation: one entry per sink, in sink
    // order, carrying the node that produced the pose and the layout generation
    // it was produced for.
    std::vector<SinkTrace> trace() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

PoseGraphRuntime &poseGraphRuntime();

} // namespace Pelican::Animation
