#pragma once

#include "posegraph.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
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
    // True when an animation component already drives this object. A graph sink
    // on the same object is pose.sink_conflict@1, never a silent second writer.
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
    // Forget every attachment whose graph is no longer published.
    void retireUnreferenced() noexcept;
    void clear() noexcept;

    struct SinkTrace {
        std::string sink;
        std::string node;
        std::uint64_t layout_identity = 0;
        std::uint32_t layout_generation = 0;
        std::uint64_t frame_revision = 0;
        std::uint64_t pose_hash = 0;
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
