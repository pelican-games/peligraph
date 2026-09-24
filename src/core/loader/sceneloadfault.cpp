#include "sceneloadfault.hpp"

#include "../userpublic/details/event/registerer.hpp"

#include <array>

namespace Pelican::internal {
namespace {

struct RootMutation {
    SceneLoadRoot root = SceneLoadRoot::unspecified;
    std::string_view participant;
    std::string_view mutation_id;
};

constexpr std::array root_mutations{
    RootMutation{SceneLoadRoot::startup, "root.startup",
                 scene_load_mutation::root_startup},
    RootMutation{SceneLoadRoot::rpc, "root.rpc",
                 scene_load_mutation::root_rpc},
    RootMutation{SceneLoadRoot::pending, "root.pending",
                 scene_load_mutation::root_pending},
    RootMutation{SceneLoadRoot::import_versioned,
                 "root.import.versioned",
                 scene_load_mutation::root_import_versioned},
};

} // namespace

bool sceneLoadMutationEnabled(
    SceneLoadFaultInjector *injector, SceneLoadFaultPhase phase,
    std::string_view participant_name,
    std::string_view mutation_id) noexcept {
    return injector != nullptr &&
           injector->shouldMutate(phase, participant_name, mutation_id);
}

bool sceneLoadRootDisconnected(SceneLoadRoot root,
                               SceneLoadFaultInjector *injector) noexcept {
    for (const auto &mutation : root_mutations) {
        if (mutation.root == root) {
            return sceneLoadMutationEnabled(
                injector, SceneLoadFaultPhase::prepare,
                mutation.participant, mutation.mutation_id);
        }
    }
    return false;
}

void rollbackSceneEventOutbox(
    PreparedEventOutbox &outbox,
    SceneLoadFaultInjector *injector) noexcept {
    if (sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::flush, "event_outbox",
            scene_load_mutation::event_pending)) {
        outbox.flush();
    }
}

void rollbackSceneRngCandidate(
    DeterministicRng &rng,
    const DeterministicRng::PreparedSequence &candidate,
    SceneLoadFaultInjector *injector) noexcept {
    auto leaked = rng.prepareSequence();
    bool publish = false;
    if (sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::flush, "rng_sequence",
            scene_load_mutation::rng_current_seed)) {
        leaked.current_seed = candidate.current_seed;
        publish = true;
    }
    if (sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::flush, "rng_sequence",
            scene_load_mutation::rng_state)) {
        leaked.state = candidate.state;
        publish = true;
    }
    if (publish) rng.publishPreparedSequence(leaked);
}

void rollbackScenePendingCandidate(
    std::optional<SceneId> &live,
    std::optional<SceneId> &candidate,
    SceneLoadFaultInjector *injector) noexcept {
    if (sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::flush, "pending_scene",
            scene_load_mutation::pending_scene)) {
        live.swap(candidate);
    }
}

} // namespace Pelican::internal
