#include "vatplayer.hpp"

#include "../build_features.hpp"
#include "../launchconfig.hpp"

namespace Pelican {

VatPlayer::VatPlayer() {
    if (GET_MODULE(EngineLaunchConfig).play_vat) {
        throwBuildFeatureDisabled("PELICAN_WITH_VAT", "--play-vat is unavailable");
    }
}

void VatPlayer::applyCameraOverride() {}

void VatPlayer::releaseInstanceForSceneLoad() {
    instance.reset();
    enabled = false;
}

VatPlayer::PreparedSceneState VatPlayer::snapshotPreparedScene() const noexcept {
    return PreparedSceneState{.enabled = enabled, .instance = instance};
}

VatPlayer::PreparedSceneState VatPlayer::prepareSceneLoad() const noexcept {
    return PreparedSceneState{};
}

void VatPlayer::publishPreparedScene(
    PreparedSceneState &&prepared) noexcept {
    std::swap(enabled, prepared.enabled);
    instance.swap(prepared.instance);
}

} // namespace Pelican
