#include "animationphasescheduler.hpp"

#include "animationserviceabi.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace Pelican::Animation::Internal {

Status PhaseScheduler::registerPhase(AnimationOwnerHandle owner,
                                     const PhaseRegistrationV1 &registration,
                                     AnimationPhaseCallbackV1 callback,
                                     void *user_context,
                                     std::uint64_t &next_identity,
                                     PhaseRegistrationHandle &out) {
    for (const auto &[_, existing] : phases_) {
        if (!existing.active) continue;
        if (existing.registration.phase == registration.phase &&
            existing.registration.priority == registration.priority &&
            existing.registration.registration_identity == registration.registration_identity &&
            existing.registration.source_ordinal == registration.source_ordinal)
            return Status::phase_order_error;
    }
    PhaseRecord phase;
    phase.handle = {next_identity++, owner.generation, 0};
    phase.owner = owner;
    phase.registration = registration;
    phase.callback = callback;
    phase.user_context = user_context;
    out = phase.handle;
    phases_.emplace(phase.handle.identity, phase);
    return Status::ok;
}

Status PhaseScheduler::unregisterPhase(AnimationOwnerHandle owner,
                                       PhaseRegistrationHandle registration) {
    const auto found = phases_.find(registration.identity);
    if (found == phases_.end()) return Status::invalid_handle;
    auto &phase = found->second;
    if (!phase.active || !sameHandle(phase.handle, registration) ||
        !sameHandle(phase.owner, owner))
        return Status::stale_generation;
    phase.active = false;
    if (++phase.handle.generation == 0) ++phase.handle.generation;
    return Status::ok;
}

std::vector<PhaseScheduler::PhaseRecord *> PhaseScheduler::orderedActivePhases() {
    std::vector<PhaseRecord *> ordered;
    for (auto &[_, phase] : phases_)
        if (phase.active) ordered.push_back(&phase);
    std::sort(ordered.begin(), ordered.end(),
              [](const PhaseRecord *left, const PhaseRecord *right) {
        if (left->registration.phase != right->registration.phase)
            return left->registration.phase < right->registration.phase;
        if (left->registration.priority != right->registration.priority)
            return left->registration.priority < right->registration.priority;
        if (left->registration.registration_identity != right->registration.registration_identity)
            return left->registration.registration_identity < right->registration.registration_identity;
        return left->registration.source_ordinal < right->registration.source_ordinal;
    });
    return ordered;
}

void PhaseScheduler::beginRun(AnimationSinkHandle sink,
                              std::uint64_t revision) noexcept {
    active_phase_sink_ = sink;
    active_phase_revision_ = revision;
}

void PhaseScheduler::clearActive() noexcept {
    active_phase_sink_.reset();
    active_phase_revision_ = 0;
}

void PhaseScheduler::retirePhasesForOwner(std::uint64_t owner_identity) {
    for (auto &[_, phase] : phases_)
        if (phase.active && phase.owner.identity == owner_identity) phase.active = false;
}

void PhaseScheduler::clearForReset() {
    phases_.clear();
    active_phase_sink_.reset();
    active_phase_revision_ = 0;
}

Status PhaseScheduler::statusForCallbackException() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (const std::invalid_argument &) {
        return Status::invalid_argument;
    } catch (...) {
        return Status::callback_failed;
    }
}

} // namespace Pelican::Animation::Internal
