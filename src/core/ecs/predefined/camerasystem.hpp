#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <span>

#include "camera.hpp"
#include "transform.hpp"

namespace Pelican {

DECLARE_MODULE(CameraSystem) {
  public:
    using QueryComponents = std::tuple<TransformComponent *, CameraComponent *>;

    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
