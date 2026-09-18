#pragma once

#include "../userpublic/animation/abi_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Pelican::Animation::Internal {

// D-7. The phase half of the animation service: the `phases` table, the run
// ordering, and the "which sink / revision / phase is running right now"
// vocabulary that publish_animation_frame_from_source reads back. Split out of
// `AnimationServiceRuntime::Impl`, which composes this and keeps the owner and
// handle-generation tables.
//
// The drivers themselves -- runPhases / runAllPhases -- stay on `Impl`: they
// span this table, the sink registry and the pose arena. What lives here is
// everything those drivers need that is phase state and nothing else.
//
// Lock discipline is unchanged: the service's single `std::recursive_mutex`
// stays on `Impl`. Every method below is called with it held; the one place it
// is deliberately released -- across a client phase callback -- is in
// `Impl::runPhases`, and `orderedActivePhases` returns pointers into a
// node-based map that nothing erases so they survive that gap.
class PhaseScheduler {
  public:
    struct PhaseRecord {
        PhaseRegistrationHandle handle{};
        AnimationOwnerHandle owner{};
        PhaseRegistrationV1 registration{};
        AnimationPhaseCallbackV1 callback{};
        void *user_context{};
        bool active{true};
    };

    // Owner validity and descriptor shape are decided by `Impl` before these
    // are called.
    Status registerPhase(AnimationOwnerHandle owner,
                         const PhaseRegistrationV1 &registration,
                         AnimationPhaseCallbackV1 callback, void *user_context,
                         std::uint64_t &next_identity,
                         PhaseRegistrationHandle &out);
    Status unregisterPhase(AnimationOwnerHandle owner,
                           PhaseRegistrationHandle registration);

    std::vector<PhaseRecord *> orderedActivePhases();

    void beginRun(AnimationSinkHandle sink, std::uint64_t revision) noexcept;
    void clearActive() noexcept;
    const std::optional<AnimationSinkHandle> &activeSink() const noexcept {
        return active_phase_sink_;
    }
    std::uint64_t activeRevision() const noexcept {
        return active_phase_revision_;
    }
    Phase activePhase() const noexcept { return active_phase_; }
    void setActivePhase(Phase phase) noexcept { active_phase_ = phase; }

    void retirePhasesForOwner(std::uint64_t owner_identity);
    void clearForReset();
    std::size_t phaseCount() const noexcept { return phases_.size(); }

    // What a client callback is allowed to throw, and what the caller is told.
    // The callback is a raw function pointer from a game DLL invoked with no
    // shim in between, so this is the only place the mapping can live.
    static Status statusForCallbackException() noexcept;

  private:
    std::unordered_map<std::uint64_t, PhaseRecord> phases_;
    std::optional<AnimationSinkHandle> active_phase_sink_;
    std::uint64_t active_phase_revision_{};
    Phase active_phase_{Phase::parameter_snapshot};
};

} // namespace Pelican::Animation::Internal
