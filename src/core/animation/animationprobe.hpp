#pragma once

#include "../userpublic/animation/abi_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Pelican::Animation {

struct ProbeAnnotation {
    double time_seconds{};
    AnnotationKind kind{AnnotationKind::event};
    std::uint32_t source{};
    std::uint32_t ordinal{};
    std::uint64_t identity{};
};

class ProbeRuntime {
  public:
    explicit ProbeRuntime(std::size_t arena_capacity = 1u << 20);
    ~ProbeRuntime();
    ProbeRuntime(ProbeRuntime &&) noexcept;
    ProbeRuntime &operator=(ProbeRuntime &&) noexcept;
    ProbeRuntime(const ProbeRuntime &) = delete;
    ProbeRuntime &operator=(const ProbeRuntime &) = delete;

    PoseArenaHandle beginFrame(std::uint64_t frame_revision);
    Status acquirePose(PoseArenaHandle arena, PoseLayoutHandle layout, std::uint32_t joint_count,
                       PoseViewV1 &out_view);
    Status validatePose(PoseHandle pose) const;
    static Status validatePoseHandle(PoseHandle pose);

    CursorHandle createCursor(double duration_seconds, WrapMode wrap_mode,
                              std::span<const ProbeAnnotation> annotations = {});
    Status destroyCursor(CursorHandle cursor);
    Status advanceCursor(const AdvanceDescV1 &desc, IntervalResultV1 &result);
    double cursorTime(CursorHandle cursor) const;

    static QuatfV1 blendQuaternions(std::span<const QuatfV1> values, std::span<const float> weights);
    static Status localToModel(std::span<const TransformV1> local, std::span<const std::int32_t> parents,
                               std::span<Matrix4fV1> model);

    Status publishAnimationFrame(const PublishAnimationFrameDescV1 &desc);
    Status advanceTemporalHistoryAfterRender(const AdvanceTemporalHistoryDescV1 &desc);
    std::uint64_t currentRevision(InstanceHandle instance) const;
    std::uint64_t previousRevision(InstanceHandle instance) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Pelican::Animation
