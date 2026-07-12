#include "predefined.hpp"
#include "componentinfo.hpp"
#include "core.hpp"

#include "predefined/camera.hpp"
#include "predefined/modelview.hpp"
#include "predefined/transform.hpp"

#include "predefined/camerasystem.hpp"
#include "predefined/animationsystem.hpp"
#include "predefined/localtransformsystem.hpp"
#include "predefined/modelviewtransoformsystem.hpp"
#include "predefined/modelviewupdatesystem.hpp"
#include "predefined/spriteviewsystem.hpp"

#include <details/component/registerer.hpp>

namespace Pelican {

void ECSPredefinedRegistration::reg() {
    // predefined component
    internal::getComponentRegisterer().registerComponent<EntityId>("eid");
    internal::getComponentRegisterer().registerComponent<TransformComponent>("transform");
    internal::getComponentRegisterer().registerComponent<LocalTransformComponent>("localtransform");
    internal::getComponentRegisterer().registerComponent<SimpleModelViewComponent>("simplemodelview");
    internal::getComponentRegisterer().registerComponent<CameraComponent>("camera");
    internal::getComponentRegisterer().registerComponent<AnimationComponent>("animation");
    registerSpriteViewComponent();

    auto &ecs = GET_MODULE(ECSCore);
    const auto local_transform_system =
        ecs.registerSystemForce<LocalTransformSystem, EntityId, TransformComponent, LocalTransformComponent>(
            GET_MODULE(LocalTransformSystem), {});
    const auto model_update_system =
        ecs.registerSystemForce<SimpleModelViewUpdateSystem, SimpleModelViewComponent>(
            GET_MODULE(SimpleModelViewUpdateSystem), {});
    ecs.registerSystemForce<AnimationSystem, AnimationComponent, SimpleModelViewComponent>(
        GET_MODULE(AnimationSystem), {model_update_system});
    ecs.registerSystemForce<SimpleModelViewTransformSystem, TransformComponent, SimpleModelViewComponent>(
        GET_MODULE(SimpleModelViewTransformSystem), {local_transform_system, model_update_system});
    ecs.registerSystemForce<CameraSystem, TransformComponent, CameraComponent>(
        GET_MODULE(CameraSystem), {local_transform_system});
    ecs.registerSystemForce<SpriteViewRenderSystem, EntityId, TransformComponent, SpriteViewComponent>(
        GET_MODULE(SpriteViewRenderSystem), {local_transform_system});
}

} // namespace Pelican
