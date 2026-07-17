#include "vatplayer.hpp"

#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include <glm/glm.hpp>
#include <stdexcept>

namespace Pelican {

VatPlayer::VatPlayer() {
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    if (!launch_config.play_vat) {
        return;
    }

    applyCameraOverride();
    vat_model = GET_MODULE(GltfLoader).loadGltfBinary(launch_config.play_vat->string());
    instance = GET_MODULE(PolygonInstanceContainer).placeModelInstance(*vat_model);
    enabled = true;

    LOG_INFO(logger, "VatPlayer loaded {}", launch_config.play_vat->string());
}

void VatPlayer::applyCameraOverride() {
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    if (!launch_config.camera_override) {
        return;
    }

    auto &camera = GET_MODULE(Camera);
    const auto &override = *launch_config.camera_override;
    const glm::vec3 position{override.position[0], override.position[1], override.position[2]};
    const glm::vec3 target{override.target[0], override.target[1], override.target[2]};
    const auto dir = target - position;
    if (glm::length(dir) <= 0.0f) {
        throw std::runtime_error("--camera position and target must not be identical");
    }

    camera.setPos(position);
    camera.setDir(glm::normalize(dir));
    camera.setNearFar(override.fov_y, 0.01f, 1000.0f);
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

} // namespace Pelican
