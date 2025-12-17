#include "predefined.hpp"
#include "componentinfo.hpp"
#include "core.hpp"

#include "predefined/camera.hpp"
#include "predefined/modelview.hpp"
#include "predefined/transform.hpp"

#include "../asset/model.hpp"
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
    GET_MODULE(ComponentInfoManager)
        .registerComponent(ComponentInfo{
            .id = ComponentIdByType<TransformComponent>::value,
            .size = sizeof(TransformComponent),
            .name = "transform",
            .cb_init =
                [](void *ptr) {

                },
            .cb_deinit =
                [](void *ptr) {

                },
            .cb_load_by_json =
                [](void *ptr, const nlohmann::json &hint) {
                    TransformComponent &transform = *static_cast<TransformComponent *>(ptr);

                    const auto &pos = hint.at("pos");
                    const auto &rot = hint.at("rotation");
                    const auto &scale = hint.at("scale");
                    transform.pos.x = pos[0];
                    transform.pos.y = pos[1];
                    transform.pos.z = pos[2];
                    transform.rotation.x = rot[0];
                    transform.rotation.y = rot[1];
                    transform.rotation.z = rot[2];
                    transform.rotation.w = rot[3];
                    transform.scale.x = scale[0];
                    transform.scale.y = scale[1];
                    transform.scale.z = scale[2];
                },
        });
    GET_MODULE(ComponentInfoManager)
        .registerComponent(ComponentInfo{
            .id = ComponentIdByType<SimpleModelViewComponent>::value,
            .size = sizeof(SimpleModelViewComponent),
            .name = "simplemodelview",
            .cb_init =
                [](void *ptr) {

                },
            .cb_deinit =
                [](void *ptr) {
                    SimpleModelViewComponent &model = *static_cast<SimpleModelViewComponent *>(ptr);

                    GET_MODULE(PolygonInstanceContainer).removeModelInstance(model.model_instance_id);
                },
            .cb_load_by_json =
                [](void *ptr, const nlohmann::json &hint) {
                    SimpleModelViewComponent &model = *static_cast<SimpleModelViewComponent *>(ptr);

                    const auto &model_name = hint.at("model");
                    auto &pic = GET_MODULE(PolygonInstanceContainer);
                    auto &asset_con = GET_MODULE(ModelAssetContainer);
                    model.model_instance_id = pic.placeModelInstance(asset_con.getModelTemplateByName(model_name));
                },
        });
    GET_MODULE(ComponentInfoManager)
        .registerComponent(ComponentInfo{
            .id = ComponentIdByType<CameraComponent>::value,
            .size = sizeof(CameraComponent),
            .name = "camera",
            .cb_init = [](void *ptr) {},
            .cb_deinit = [](void *ptr) {},
            .cb_load_by_json = [](void *ptr, const nlohmann::json &hint) {},
        });
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
