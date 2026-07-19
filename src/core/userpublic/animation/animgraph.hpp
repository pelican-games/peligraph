#pragma once

#include "abi_v1.hpp"
#include "../export.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::AnimationGraph {

enum class StateKind : std::uint32_t { clip = 0, blend1d = 1 };
enum class InterruptMode : std::uint32_t { never = 0, higher_priority = 1, always = 2 };

struct ClipNodeV1 {
    std::string clip;
    double speed = 1.0;
    double start_offset = 0.0;
    bool loop = true;
    double threshold = 0.0;
};

struct StateV1 {
    std::string name;
    StateKind kind = StateKind::clip;
    std::string parameter;
    std::vector<ClipNodeV1> clips;
};

struct ConditionV1 {
    std::string parameter;
    std::string operation;
    double value = 0.0;
};

struct TransitionV1 {
    std::string from;
    std::string to;
    std::int32_t priority = 0;
    InterruptMode interrupt = InterruptMode::never;
    double duration = 0.0;
    std::vector<ConditionV1> conditions;
    std::uint32_t declaration_index = 0;
};

struct ParameterV1 {
    std::string name;
    double initial_value = 0.0;
};

struct DocumentV1 {
    std::vector<ParameterV1> parameters;
    std::vector<StateV1> states;
    std::vector<TransitionV1> transitions;
    std::string initial_state;
};

// Throws std::runtime_error for malformed documents. The accepted envelope is
// {"schema":"pelican.anim_graph","version":1,...}. v2-reserved keys are
// rejected instead of being silently ignored.
PELICAN_API DocumentV1 parseDocumentV1(std::string_view json_text);

struct CursorStatusV1 {
    std::string clip;
    double time_seconds = 0.0;
    double normalized_phase = 0.0;
    std::uint64_t asset_identity = 0;
    std::uint32_t asset_generation = 0;
    std::uint32_t profile_version = 0;
    std::string source_rig_sha256;
    std::string target_rig_sha256;
};

struct StatusTraceV1 {
    std::string current_state;
    std::vector<CursorStatusV1> cursors;
    bool transition_active = false;
    std::string transition_target;
    double transition_progress = 0.0;
    std::uint64_t snapshot_revision = 0;
    std::uint64_t snapshot_layout_identity = 0;
    std::uint32_t snapshot_layout_generation = 0;
    std::uint64_t snapshot_pose_hash = 0;
    std::uint64_t semantic_pose_hash = 0;
    std::uint64_t frame_revision = 0;
    std::uint64_t source_reset_count = 0;
    Animation::AnimationSourceAuthorityV1 authority =
        Animation::AnimationSourceAuthorityV1::graph_apply;
};

class PELICAN_API EvaluatorV1 {
  public:
    EvaluatorV1(DocumentV1 document, std::string object_name, std::uint32_t source_ordinal = 100);
    ~EvaluatorV1();
    EvaluatorV1(EvaluatorV1 &&) noexcept;
    EvaluatorV1 &operator=(EvaluatorV1 &&) noexcept;
    EvaluatorV1(const EvaluatorV1 &) = delete;
    EvaluatorV1 &operator=(const EvaluatorV1 &) = delete;

    // Resolves the object and clips, claims the skeletal sink, and registers a
    // base-pose phase callback through AnimationServiceV1.
    Animation::Status bind();
    // Re-resolves an asset after stale_generation without resetting graph
    // parameters, state clocks, transition snapshots, source, or phase.
    Animation::Status rebind();
    bool bound() const noexcept;

    // Captures one deterministic parameter/time snapshot. Evaluation happens
    // when the engine invokes the registered animation phase later this tick.
    Animation::Status prepareTick(double absolute_time, double delta_seconds,
                                  std::uint64_t frame_revision);
    Animation::Status setParameter(std::string_view name, double value);
    Animation::Status forceState(std::string_view state);
    // Explicit discontinuities reset/reconstruct graph clock state before the
    // next tick and are forwarded to the engine for temporal-history reset.
    Animation::Status notify(Animation::AnimationNotificationKind kind, double time_seconds,
                             Animation::PoseLayoutHandle observed_layout = {});

    StatusTraceV1 getStatus() const;
    std::vector<std::byte> lastPoseBytes() const;
    std::string lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Pelican::AnimationGraph
