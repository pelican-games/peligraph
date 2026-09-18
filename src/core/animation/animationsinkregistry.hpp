#pragma once

#include "animationrecords.hpp"
#include "../userpublic/animation/abi_v1.hpp"
#include "../userpublic/animation/pose_staging_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pelican::Animation::Internal {

// D-7. The sink half of the animation service: `sinks` / `instances` /
// `sources` / `blocked_commits` and the single staged frame, plus the
// claim / handoff / commit-gate logic that touches only those. Split out of
// `AnimationServiceRuntime::Impl`, which composes this and keeps the owner and
// handle-generation tables, the object/asset tables and the ProbeRuntime.
//
// Lock discipline is unchanged: the service's single `std::recursive_mutex`
// stays on `Impl` and every method below is called with it already held.
class SinkRegistry {
  public:
    struct SinkRecord {
        AnimationSinkHandle handle{};
        ObjectRecord *object{};
        AnimationSinkKind kind{AnimationSinkKind::skeletal_pose};
        InstanceHandle instance{};
        AnimationSourceHandle active_source{};
        bool reset_history{};
        std::uint64_t last_notification_revision{};
    };
    struct SourceRecord {
        AnimationSourceHandle handle{};
        AnimationOwnerHandle owner{};
        AnimationSinkHandle sink{};
        std::uint32_t ordinal{};
        AnimationSourceAuthorityV1 authority{
            AnimationSourceAuthorityV1::graph_apply};
        bool active{true};
    };
    struct BlockedCommit {
        std::uint64_t instance_identity{};
        std::uint32_t instance_generation{};
        std::uint64_t revision{};
    };
    struct StagedFrame {
        PoseStageHandle handle{};
        AnimationSinkHandle sink{};
        ObjectRecord *object{};
        PublishAnimationFrameDescV1 frame{};
        std::vector<Matrix4fV1> palette;
        bool pose_accessed{};
    };

    SinkRecord *findSink(AnimationSinkHandle sink);
    SourceRecord *findSource(AnimationSourceHandle source);
    Status resolveSinkByInstance(InstanceHandle instance, SinkRecord *&out);
    // Get-or-create for one (object, kind) pair. The object is resolved (and
    // lazily registered) by `Impl` before this is called.
    Status sinkForObject(ObjectRecord &object, AnimationSinkKind kind,
                         std::uint64_t &next_identity,
                         AnimationSinkHandle &out);
    // Unsorted, by design: runAllPhases sorts after releasing the lock.
    std::vector<AnimationSinkHandle> sinkHandles() const;

    // Owner validity is decided by `Impl` before claim / release are called.
    // With `graph_apply` this is the claim_source contract; the other
    // authorities neither conflict with nor become the sink's active source.
    Status claimSource(AnimationOwnerHandle owner, AnimationSinkHandle sink,
                       std::uint32_t ordinal,
                       AnimationSourceAuthorityV1 authority,
                       std::uint64_t &next_identity,
                       AnimationSourceHandle &out);
    Status releaseSource(AnimationOwnerHandle owner,
                         AnimationSourceHandle source);
    // The tail of handoff_source, after `Impl` has validated the current
    // source, its authority, the next owner and the sink.
    AnimationSourceHandle transferSource(SourceRecord &current, SinkRecord &sink,
                                         AnimationOwnerHandle next_owner,
                                         std::uint32_t next_ordinal,
                                         std::uint64_t &next_identity);
    Status notify(const AnimationNotificationDescV1 &desc);

    void blockCommit(InstanceHandle instance, std::uint64_t revision);
    bool commitBlocked(InstanceHandle instance,
                       std::uint64_t revision) const;
    std::optional<StagedFrame> &stagedFrame() noexcept { return staged_frame_; }
    const std::optional<StagedFrame> &stagedFrame() const noexcept {
        return staged_frame_;
    }

    void retireSourcesForOwner(std::uint64_t owner_identity);
    // C-4. Retired sources are erased rather than tombstoned; see the comment
    // on releaseOwner in animationservice.cpp for why that is safe here and
    // not for owners / phases / cursors.
    void compactRetiredSources();
    void retireObject(const ObjectRecord *object);
    void rebindInstancesForObjects(
        const std::unordered_set<ObjectRecord *> &affected);
    void clearForReset();
    std::size_t sourceCount() const noexcept { return sources_.size(); }

  private:
    std::unordered_map<std::uint64_t, SinkRecord> sinks_;
    std::unordered_map<std::uint64_t, std::uint64_t> instances_;
    std::unordered_map<std::uint64_t, SourceRecord> sources_;
    std::vector<BlockedCommit> blocked_commits_;
    std::optional<StagedFrame> staged_frame_;
};

} // namespace Pelican::Animation::Internal
