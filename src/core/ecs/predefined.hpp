#pragma once

#include "../container.hpp"

#include "componentinfo.hpp"

#include "predefined/camera.hpp"
#include "predefined/modelview.hpp"
#include "predefined/transform.hpp"

namespace Pelican {

DECLARE_MODULE(ECSPredefinedRegistration) {
  public:
    void reg();
};

DECLARE_COMPONENT(TransformComponent, 1);
DECLARE_COMPONENT(SimpleModelViewComponent, 2);
DECLARE_COMPONENT(CameraComponent, 3);

} // namespace Pelican
