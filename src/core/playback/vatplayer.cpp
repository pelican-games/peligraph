#include "vatplayer.hpp"

#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../model/gltf.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

VatPlayer::VatPlayer() {
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    if (!launch_config.play_vat) {
        return;
    }

    vat_model = GET_MODULE(GltfLoader).loadGltfBinary(launch_config.play_vat->string());
    instance = GET_MODULE(PolygonInstanceContainer).placeModelInstance(*vat_model);
    enabled = true;

    LOG_INFO(logger, "VatPlayer loaded {}", launch_config.play_vat->string());
}

void VatPlayer::releaseInstanceForSceneLoad() {
    if (instance) {
        if (auto *instance_container =
                FastModuleContainer::tryGet<PolygonInstanceContainer>()) {
            (void)instance_container->removeModelInstance(*instance);
        }
        instance.reset();
    }
    enabled = false;
}

// snapshotPreparedScene / prepareSceneLoad / publishPreparedScene live in
// playbackscenestate.cpp, which every build variant compiles.

} // namespace Pelican
