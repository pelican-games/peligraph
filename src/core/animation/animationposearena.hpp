#pragma once

#include "animationjobs.hpp"
#include "animationprobe.hpp"
#include "../userpublic/animation/abi_v1.hpp"

#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Pelican::Animation::Internal {

// D-7. The pose half of the animation service: `arenas` / `poses` /
// `stale_poses` and the job functions that touch nothing else. Split out of
// `AnimationServiceRuntime::Impl`, which composes this and keeps the owner and
// handle-generation tables.
//
// Lock discipline is unchanged and is NOT expressed here: the service's single
// `std::recursive_mutex` stays on `Impl`, and every method below is called
// with it already held. This class owns no synchronisation of its own.
class PoseArena {
  public:
    struct ArenaRecord {
        PoseArenaHandle handle{};
        AnimationOwnerHandle owner{};
        std::thread::id thread;
        std::unique_ptr<ProbeRuntime> runtime;
        PoseArenaHandle internal_arena{};
        std::vector<std::uint64_t> poses;
    };
    struct PoseRecord {
        PoseViewV1 view{};
        std::uint64_t arena_identity{};
        std::vector<Matrix4fV1> model_matrices;
    };

    // Handle vocabulary is unchanged: a pose that was never issued is
    // `invalid_handle` (through ProbeRuntime::validatePoseHandle), one whose
    // arena or layout went away is `stale_generation`.
    PoseRecord *findPose(PoseHandle pose);
    Status resolvePose(PoseHandle pose, PoseRecord *&out);

    // Owner validity is decided by `Impl` before this is called.
    Status beginFrame(AnimationOwnerHandle owner, std::uint64_t frame_revision,
                      std::uint64_t &next_identity, PoseArenaHandle &out_arena);
    // Layout generation is validated by `Impl` before this is called.
    Status acquirePose(PoseArenaHandle arena, PoseLayoutHandle layout,
                       std::uint32_t joint_count, PoseViewV1 &view);
    Status blendNormalJob(const AnimationRig &rig,
                          const BlendNormalDescV1 &desc);
    Status localToModelJob(const AnimationRig &rig, LocalToModelDescV1 &desc);

    void retireArenasForOwner(std::uint64_t owner_identity);
    // Reload boundary: poses built against a replaced layout become stale
    // rather than disappearing. A zero identity means nothing is retired.
    void stalePosesForLayout(std::uint64_t layout_identity);
    void clearForReset();

  private:
    std::unordered_map<std::uint64_t, ArenaRecord> arenas_;
    std::unordered_map<std::uint64_t, PoseRecord> poses_;
    std::unordered_map<std::uint64_t, std::uint32_t> stale_poses_;
};

} // namespace Pelican::Animation::Internal
