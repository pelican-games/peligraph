#include "componentinfo.hpp"
#include "../asset/model.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

ComponentInfoManager::ComponentInfoManager() {
    // predefined component
    registerComponent(ComponentInfo{
        .id = 0,
        .size = sizeof(EntityId),
        .name = "eid",
    });
    registerComponent(ComponentInfo{
        .id = 1,
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
    registerComponent(ComponentInfo{
        .id = 2,
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
}

void ComponentInfoManager::registerComponent(ComponentInfo info) {
    if (infos.size() < info.id + 1)
        infos.resize(info.id + 1);
    infos[info.id] = info;
    name_id_map.insert({info.name, info.id});
}

uint32_t ComponentInfoManager::getSizeFromComponentId(ComponentId id) const { return infos[id].size; }
ComponentId ComponentInfoManager::getComponentIdByName(const std::string &name) const { return name_id_map.at(name); }

void ComponentInfoManager::loadByJson(void *dst_ptr, const nlohmann::json &hint) const {
    const auto id = getComponentIdByName(hint.at("name"));
    infos[id].cb_load_by_json(dst_ptr, hint);
}

} // namespace Pelican