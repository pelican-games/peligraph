#include "seqplayer.hpp"
#include "vatplayer.hpp"

#include "../loader/sceneload.hpp"

#include <utility>

namespace Pelican {

// The prepared-scene snapshot/publish protocol for both players, compiled in
// every build variant.
//
// It used to live four times: once in seqplayer.cpp, once in vatplayer.cpp,
// and verbatim again in each of the two `*_stub.cpp` files -- which is the
// worst place for it, because it has nothing to do with the disabled build
// feature and the stub build is the one nobody runs locally. The stubs now
// hold only what the disabled feature actually changes: a constructor that
// calls throwBuildFeatureDisabled, and the release path that has no instances
// to hand back.
namespace {

// A prepared scene is published by swapping both halves at once, so a rollback
// can put the player back exactly as it was: the live state becomes the
// prepared state and the caller is handed the old state to drop or restore.
template <class Instances>
void swapPreparedScene(bool &enabled, Instances &live, bool &prepared_enabled,
                       Instances &prepared) noexcept {
    std::swap(enabled, prepared_enabled);
    live.swap(prepared);
}

} // namespace

SeqPlayer::PreparedSceneState SeqPlayer::snapshotPreparedScene() const {
    return PreparedSceneState{.enabled = enabled, .instances = instances};
}

SeqPlayer::PreparedSceneState SeqPlayer::prepareSceneLoad() const {
    return PreparedSceneState{};
}

void SeqPlayer::publishPreparedScene(PreparedSceneState &&prepared) noexcept {
    swapPreparedScene(enabled, instances, prepared.enabled, prepared.instances);
}

void SeqPlayer::publishPreparedScene(PreparedSceneState &&prepared,
                                     SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::publish);
    publishPreparedScene(std::move(prepared));
}

VatPlayer::PreparedSceneState VatPlayer::snapshotPreparedScene() const noexcept {
    return PreparedSceneState{.enabled = enabled, .instance = instance};
}

VatPlayer::PreparedSceneState VatPlayer::prepareSceneLoad() const noexcept {
    return PreparedSceneState{};
}

void VatPlayer::publishPreparedScene(PreparedSceneState &&prepared) noexcept {
    swapPreparedScene(enabled, instance, prepared.enabled, prepared.instance);
}

void VatPlayer::publishPreparedScene(PreparedSceneState &&prepared,
                                     SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::publish);
    publishPreparedScene(std::move(prepared));
}

} // namespace Pelican
