#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <span>

#include "camera.hpp"
#include "transform.hpp"

namespace Pelican {

class Camera;

DECLARE_MODULE(CameraSystem) {
    Camera *camera = nullptr;

  public:
    using QueryComponents = std::tuple<TransformComponent *, CameraComponent *>;

    void prepareEcsWorkerDependencies(bool has_matching_chunks);
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
