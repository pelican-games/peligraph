#pragma once

#include "sceneload.hpp"
#include "../userpublic/deterministicrng.hpp"

#include <optional>

namespace Pelican::internal {

class PreparedEventOutbox;

bool sceneLoadMutationEnabled(
    SceneLoadFaultInjector *injector, SceneLoadFaultPhase phase,
    std::string_view participant_name,
    std::string_view mutation_id) noexcept;

bool sceneLoadRootDisconnected(SceneLoadRoot root,
                               SceneLoadFaultInjector *injector) noexcept;

// Normal rollback discards all three candidates.  These helpers are the
// deliberately bad counterfactuals selected by a manifest mutation id.
void rollbackSceneEventOutbox(
    PreparedEventOutbox &outbox,
    SceneLoadFaultInjector *injector) noexcept;
void rollbackSceneRngCandidate(
    DeterministicRng &rng,
    const DeterministicRng::PreparedSequence &candidate,
    SceneLoadFaultInjector *injector) noexcept;
void rollbackScenePendingCandidate(
    std::optional<SceneId> &live,
    std::optional<SceneId> &candidate,
    SceneLoadFaultInjector *injector) noexcept;

} // namespace Pelican::internal
