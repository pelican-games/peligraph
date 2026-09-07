#include "predefined.hpp"
#include "componentinfo.hpp"
#include "core.hpp"

#include "predefined/camera.hpp"
#include "predefined/modelview.hpp"
#include "predefined/transform.hpp"
#include "enginecomponentregistrar.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../renderer/proceduralgeometrycomponent.hpp"
#include "../userpublic/components/rayinstance.hpp"

#include "predefined/camerasystem.hpp"
#include "predefined/animationsystem.hpp"
#include "predefined/localtransformsystem.hpp"
#include "predefined/modelviewtransoformsystem.hpp"
#include "predefined/modelviewupdatesystem.hpp"
#include "predefined/spriteviewsystem.hpp"

namespace Pelican {

void ECSPredefinedRegistration::reg() {
    // predefined component
    auto &components = internal::getEngineComponentRegisterer();
    components.registerComponent<EntityId>("eid");
    components.registerComponent<TransformComponent>("transform");
    components.registerComponent<LocalTransformComponent>(
        "localtransform");
    components.registerComponent<SimpleModelViewComponent>(
        "simplemodelview");
    components.registerComponent<CameraComponent>("camera");
    components.registerComponent<AnimationComponent>("animation");
    registerSpriteViewComponent();
    components.registerComponent<ProceduralGeometryComponent>(
        "pelican_procedural_geometry");
    if (const auto *ray_layout =
            FastModuleContainer::tryGet<RayLayoutRuntimeState>();
        ray_layout != nullptr && ray_layout->builtinRayInstanceEnabled()) {
        components.registerComponent<RayInstanceComponent>(
            "pelican_ray_instance");
    }

    auto &ecs = GET_MODULE(ECSCore);
    const auto local_transform_system =
        ecs.registerSystemForce<LocalTransformSystem, EntityId, TransformComponent, LocalTransformComponent>(
            GET_MODULE(LocalTransformSystem), {});
    const auto model_update_system =
        ecs.registerSystemForce<SimpleModelViewUpdateSystem, EntityId,
                                SimpleModelViewComponent>(
            GET_MODULE(SimpleModelViewUpdateSystem), {});
    const auto animation_system =
        ecs.registerSystemForce<AnimationSystem, EntityId, AnimationComponent, SimpleModelViewComponent>(
        GET_MODULE(AnimationSystem), {model_update_system});
    const auto model_transform_system =
        ecs.registerSystemForce<SimpleModelViewTransformSystem, TransformComponent, SimpleModelViewComponent>(
            GET_MODULE(SimpleModelViewTransformSystem),
            {local_transform_system, model_update_system, animation_system});
    const auto camera_system = ecs.registerSystemForce<CameraSystem, TransformComponent, CameraComponent>(
        GET_MODULE(CameraSystem), {local_transform_system, model_transform_system});
    ecs.registerSystemForce<SpriteViewRenderSystem, EntityId, TransformComponent, SpriteViewComponent>(
        GET_MODULE(SpriteViewRenderSystem), {local_transform_system, camera_system});
}

} // namespace Pelican
