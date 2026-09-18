#pragma once

#include "../../project/graphinstanceowner.hpp"

#include "posegraph.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
    // AG-2. The object a root-motion displacement lands on, as an opaque token
    // the ECS side packs and unpacks; empty when the owner has no live object
    // or that object cannot take a local transform. Asked once, at attach, and
    // the answer is kept on the attachment: this callback captures the owner
    // thread's locals and is gone by the time the phase runs.
    std::function<std::optional<std::uint64_t>(const GraphInstanceOwner &)> root_transform_object;
    // Adds `delta` (in the object's own local frame, before its rotation) to
    // that object's local translation. A function pointer on purpose: unlike
    // everything above it is called from the phase callback, long after the
    // owner thread's frame locals have died, so it must not be able to capture
    // them. Returns false when the object can no longer take the write.
    bool (*apply_root_translation)(std::uint64_t object, const double *delta) = nullptr;
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

    // Raises again what an evaluation threw inside a phase callback, if any
    // attachment is carrying one. An exception cannot cross the C callback
    // boundary, so the phase kept the exception object and answered
    // `callback_failed`; this is how the owner thread turns that status back
    // into the message the graph's author needs -- "pose port is unconnected:
    // blend1.b" rather than "something in the animation phase failed". Call it
    // wherever a non-ok phase result is about to be reported; it returns
    // normally when no attachment failed. The next `synchronize` raises
    // anything still pending, so a caller that ignores it loses nothing.
    void rethrowPendingFailure();

    // What kind of chain produced this sink's pose. It decides where the
    // cursors below come from: a wrapped state machine keeps its own, a
    // clip_sample node has none of its own and the runtime records what it
    // sampled.
    enum class SinkSource : std::uint8_t { clip, state_machine };
    static std::string_view sinkSourceName(SinkSource source) noexcept;
    // Where one clip stood on the frame this trace records. `normalized_phase`
    // is the position inside the clip's own [start, end], so a clip whose keys
    // do not begin at zero still reports 0 at its first key and 1 at its last.
    struct CursorTrace {
        std::string clip;
        double time_seconds = 0.0;
        double normalized_phase = 0.0;
    };
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
        // AG-3a. The object this sink writes: the same owner the document
        // named, so an observer can say which individual moved without holding
        // the graph. It is the attachment's, not the trace's, so it is copied
        // here rather than published as a second lookup.
        GraphInstanceOwner owner;
        SinkSource source = SinkSource::clip;
        // One entry per clip the chain sampled this frame, in chain order for a
        // clip_sample chain and in the evaluator's own order for a state
        // machine (where a transition has two).
        std::vector<CursorTrace> cursors;
        bool transition_active = false;
        std::string transition_target;
        double transition_progress = 0.0;
        // AG-2's displacement for this frame, present only on a chain that
        // extracts root motion. It is the amount in the clip's own space --
        // what the applier turns by the individual's rotation before adding it
        // -- so on a rotated individual it is not the distance travelled.
        bool root_motion = false;
        std::array<double, 3> root_delta{};
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
