#include "predefined.hpp"
#include "componentinfo.hpp"
#include "core.hpp"

#include "predefined/camera.hpp"
#include "predefined/modelview.hpp"
#include "predefined/transform.hpp"

#include "predefined/camerasystem.hpp"
#include "predefined/localtransformsystem.hpp"
#include "predefined/modelviewtransoformsystem.hpp"
#include "predefined/modelviewupdatesystem.hpp"

#include <details/component/registerer.hpp>

namespace Pelican {

void ECSPredefinedRegistration::reg() {
    // predefined component
    internal::getComponentRegisterer().registerComponent<EntityId>("eid");
    internal::getComponentRegisterer().registerComponent<TransformComponent>("transform");
    internal::getComponentRegisterer().registerComponent<LocalTransformComponent>("localtransform");
    internal::getComponentRegisterer().registerComponent<SimpleModelViewComponent>("simplemodelview");
    internal::getComponentRegisterer().registerComponent<SimpleModelViewUpdateComponent>("simplemodelviewupdate");
    internal::getComponentRegisterer().registerComponent<CameraComponent>("camera");

    GET_MODULE(ECSCore)
        .registerSystemForce<SimpleModelViewTransformSystem, TransformComponent, SimpleModelViewComponent>(
            GET_MODULE(SimpleModelViewTransformSystem), {});
    GET_MODULE(ECSCore).registerSystemForce<CameraSystem, TransformComponent, CameraComponent>(GET_MODULE(CameraSystem),
                                                                                               {});
    GET_MODULE(ECSCore)
        .registerSystemForce<LocalTransformSystem, EntityId, TransformComponent, LocalTransformComponent>(
            GET_MODULE(LocalTransformSystem), {});

    GET_MODULE(ECSCore)
        .registerSystemForce<SimpleModelViewUpdateSystem, SimpleModelViewComponent, SimpleModelViewUpdateComponent>(
            GET_MODULE(SimpleModelViewUpdateSystem), {});
}

} // namespace Pelican
