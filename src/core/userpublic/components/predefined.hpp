#pragma once

#include <components/collider.hpp>
#include <components/animation.hpp>
#include <components/localtransform.hpp>
#include <components/modelview.hpp>
#include <components/rayinstance.hpp>
#include <components/spriteview.hpp>
#include <details/ecs/componentdeclare.hpp>

namespace Pelican {

class TransformComponent;
class CameraComponent;

} // namespace Pelican

// DECLARE_COMPONENT specialises Pelican::ComponentIdByType by its qualified
// name, the form a game uses from the global namespace. GCC rejects that
// qualification inside namespace Pelican, so the engine's own components are
// declared from the global namespace too (same ids, same types).
DECLARE_COMPONENT(Pelican::TransformComponent, 1);
DECLARE_COMPONENT(Pelican::SimpleModelViewComponent, 2);
DECLARE_COMPONENT(Pelican::CameraComponent, 3);

DECLARE_COMPONENT(Pelican::LocalTransformComponent, 16);
DECLARE_COMPONENT(Pelican::AnimationComponent, 17);
DECLARE_COMPONENT(Pelican::SpriteViewComponent, 18);
DECLARE_COMPONENT(Pelican::RayInstanceComponent, 19);
