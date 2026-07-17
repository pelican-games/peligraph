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

} // namespace Pelican
