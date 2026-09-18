#pragma once

#include "sceneload.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace Pelican {

// A scene load is the same shape as an editor projection transaction:
// prepare stages everything that can fail, publication is a sequence of
// noexcept swaps, rollback undoes exactly what was staged or published, and
// finish drops the inverse tokens.  `EditorProjectionAdapter` states those
// four verbs for the editor's document transaction; scene load needs the same
// four plus the steps its executor has always had between them -- an early
// publication that must happen before ECS entities exist, the activation
// window where game-DLL callbacks run, and the no-throw commit / retire /
// flush tail.  Rather than widen the editor adapter (whose prepare takes a
// document pair a scene load does not have), this is the sibling interface
// with the same verbs.
enum class SceneLoadVerb : std::uint8_t {
    prepare,
    early_publish,
    publish,
    activate,
    commit,
    retire,
    flush,
    finish,
    rollback,
};

// One named fault-injection point.  `participant` and `mutation_id` are the
// exact strings the injector already observes, so a manifest row keeps
// addressing the same executor edge now that the executor is a driver.
struct SceneLoadMutationPoint {
    SceneLoadVerb verb = SceneLoadVerb::prepare;
    // Deliberately independent of the verb: fault phases name every executor
    // edge, including the no-throw tail, and a point may be queried under a
    // phase name that does not match the verb it guards (the metadata
    // document split is a `commit` fault applied at the end of publication).
    SceneLoadFaultPhase fault_phase = SceneLoadFaultPhase::prepare;
    std::string_view participant;
    std::string_view mutation_id;
    // A `before` point runs instead of, or ahead of, the verb body; an
    // `after` point only runs once the body has succeeded, so it can only sit
    // on a verb that is allowed to throw.
    bool after = false;
};

// One participant of the scene-load transaction.  Concrete participants own
// their candidate state and the inverse of their own publication; the driver
// owns nothing but the order.
class SceneLoadParticipant {
  public:
    virtual ~SceneLoadParticipant() = default;

    // The manifest owner this participant stands for.
    virtual std::string_view name() const noexcept = 0;

    virtual std::span<const SceneLoadMutationPoint>
    mutationPoints() const noexcept {
        return {};
    }
    // Returns true when the selected mutation replaces the verb body.
    virtual bool mutateBefore(std::string_view mutation_id) noexcept {
        (void)mutation_id;
        return false;
    }
    virtual void mutateAfter(std::string_view mutation_id) {
        (void)mutation_id;
    }

    // Stages everything that can fail.  Nothing live has changed yet.
    virtual void prepare() {}
    // The part of publication that has to precede ECS entity creation, or
    // that stages against the entities the previous step created.
    virtual void earlyPublish() {}
    // Publication proper: prepared state swapped in, no allocation, no throw.
    virtual void publish() noexcept {}
    // The window where game-DLL callbacks run and may still fail the load.
    virtual void activate() {}
    virtual void commit() {}
    virtual void retire() {}
    virtual void flush() {}
    virtual void finish() {}
    // Before publication this discards prepared state; after publication it
    // swaps the snapshot back.  Both paths are allocation-free.
    virtual void rollback() noexcept {}
};

// The wrapper that owns fault injection.  Participant bodies stay free of it,
// and every `sceneLoadMutationEnabled` query a scene load makes goes through
// here, keyed by the point's own participant name and mutation id.
class SceneLoadMutationGate {
    SceneLoadFaultInjector *injector_ = nullptr;

    bool applyBefore(SceneLoadParticipant &participant,
                     SceneLoadVerb verb) const noexcept;

  public:
    explicit SceneLoadMutationGate(
        SceneLoadFaultInjector *injector) noexcept
        : injector_{injector} {}

    SceneLoadFaultInjector *injector() const noexcept { return injector_; }

    void run(SceneLoadParticipant &participant, SceneLoadVerb verb) const;
};

} // namespace Pelican
