#pragma once

#include <components/collider.hpp>
#include <components/animation.hpp>
#include <components/localtransform.hpp>
#include <components/modelview.hpp>
#include <details/ecs/componentdeclare.hpp>

namespace Pelican {

DECLARE_COMPONENT_CLASS(TransformComponent, 1);
DECLARE_COMPONENT(SimpleModelViewComponent, 2);
DECLARE_COMPONENT_CLASS(CameraComponent, 3);

DECLARE_COMPONENT(LocalTransformComponent, 16);
DECLARE_COMPONENT(AnimationComponent, 17);

} // namespace Pelican
