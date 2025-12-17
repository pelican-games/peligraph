#include "predefined.hpp"
#include "componentinfo.hpp"
#include "core.hpp"

#include "predefined/camera.hpp"
#include "predefined/modelview.hpp"
#include "predefined/transform.hpp"

#include "../geomhelper/geomhelper.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include <details/component/registerer.hpp>

namespace Pelican {

DECLARE_MODULE(SimpleModelViewTransformSystem) {
  public:
    using QueryComponents = std::tuple<TransformComponent *, SimpleModelViewComponent *>;

    void process(QueryComponents components, size_t count) {
        auto transforms = std::get<TransformComponent *>(components);
        auto models = std::get<SimpleModelViewComponent *>(components);

        auto &pic = GET_MODULE(PolygonInstanceContainer);
        for (int i = 0; i < count; i++) {
            pic.setTrs(models[i].model_instance_id, transforms[i].pos, transforms[i].rotation, transforms[i].scale);
        }
    }
};

DECLARE_MODULE(CameraSystem) {
  public:
    using QueryComponents = std::tuple<TransformComponent *, CameraComponent *>;

    void process(QueryComponents components, size_t count) {
        auto transforms = std::get<TransformComponent *>(components);
        auto cameraconfig = std::get<CameraComponent *>(components);

        auto &camera = GET_MODULE(Camera);
        for (int i = 0; i < 1; i++) {
            camera.setPos(transforms[i].pos);
            camera.setDir(transforms[i].rotation * glm::vec3{0.0, 0.0, 1.0});
        }
    }
};

DECLARE_MODULE(LocalTransformSystem) {
  public:
    using QueryComponents = std::tuple<TransformComponent *, LocalTransformComponent *>;

    void process(QueryComponents components, size_t count) {
        auto transforms = std::get<TransformComponent *>(components);
        auto localtransforms = std::get<LocalTransformComponent *>(components);

        for (int i = 0; i < count; i++) {
            transforms[i].pos = to_glm(localtransforms[i].pos);
            transforms[i].rotation = to_glm(localtransforms[i].rotation);
            transforms[i].scale = to_glm(localtransforms[i].scale);
        }
    }
};

void ECSPredefinedRegistration::reg() {
    // predefined component
    GET_MODULE(ComponentInfoManager)
        .registerComponent(ComponentInfo{
            .id = ComponentIdByType<EntityId>::value,
            .size = sizeof(EntityId),
            .name = "eid",
        });
    internal::getComponentRegisterer().registerComponent<TransformComponent>("transform");
    internal::getComponentRegisterer().registerComponent<SimpleModelViewComponent>("simplemodelview");
    internal::getComponentRegisterer().registerComponent<CameraComponent>("camera");
    internal::getComponentRegisterer().registerComponent<LocalTransformComponent>("localtransform");

    GET_MODULE(ECSCore)
        .registerSystemForce<SimpleModelViewTransformSystem, TransformComponent, SimpleModelViewComponent>(
            GET_MODULE(SimpleModelViewTransformSystem), {});
    GET_MODULE(ECSCore).registerSystemForce<CameraSystem, TransformComponent, CameraComponent>(GET_MODULE(CameraSystem),
                                                                                               {});
    GET_MODULE(ECSCore).registerSystemForce<LocalTransformSystem, TransformComponent, LocalTransformComponent>(
        GET_MODULE(LocalTransformSystem), {});
}

} // namespace Pelican
