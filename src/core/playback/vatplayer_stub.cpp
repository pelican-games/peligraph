#include "vatplayer.hpp"

#include "../build_features.hpp"
#include "../launchconfig.hpp"

namespace Pelican {

// Everything the disabled build feature actually changes. The prepared-scene
// snapshot/publish protocol is shared with the real build in
// playbackscenestate.cpp rather than copied here.
VatPlayer::VatPlayer() {
    if (GET_MODULE(EngineLaunchConfig).play_vat) {
        throwBuildFeatureDisabled("PELICAN_WITH_VAT", "--play-vat is unavailable");
    }
}

void VatPlayer::releaseInstanceForSceneLoad() {
    instance.reset();
    enabled = false;
}

} // namespace Pelican
