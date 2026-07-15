#include "camerasystem.hpp"

#include "../renderer/camera.hpp"

#include <stdexcept>

namespace Pelican {

void CameraSystem::prepareEcsWorkerDependencies(bool has_matching_chunks) {
    if (has_matching_chunks) camera = &GET_MODULE(Camera);
}

void CameraSystem::process(QueryComponents components, size_t count) {
    if (count == 0) return;
    if (camera == nullptr)
        throw std::logic_error("CameraSystem dependencies were not prepared on the ECS owner thread");
    auto transforms = std::get<TransformComponent *>(components);
    auto cameraconfig = std::get<CameraComponent *>(components);

    for (int i = 0; i < 1; i++) {
        camera->setPos(transforms[i].pos);
        camera->setDir(transforms[i].rotation * glm::vec3{0.0, 0.0, 1.0});
    }
}

} // namespace Pelican
