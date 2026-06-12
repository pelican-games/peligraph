#include "camerasystem.hpp"

#include "../renderer/camera.hpp"

namespace Pelican {

void CameraSystem::process(QueryComponents components, size_t count) {
    auto transforms = std::get<TransformComponent *>(components);
    auto cameraconfig = std::get<CameraComponent *>(components);

    auto &camera = GET_MODULE(Camera);
    for (int i = 0; i < 1; i++) {
        camera.setPos(transforms[i].pos);
        camera.setDir(transforms[i].rotation * glm::vec3{0.0, 0.0, 1.0});
    }
}

} // namespace Pelican
