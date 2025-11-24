#pragma once

#include "component.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include "../asset/model.hpp"
#include "../ecs/component_meta.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"

namespace Pelican {

ComponentId ComponentLoader::componentIdFromName(std::string_view name) const {
    if (name == "transform") {
        return ComponentIdByType<TransformComponent>::value;
    } else if (name == "simplemodelview") {
        return ComponentIdByType<SimpleModelViewComponent>::value;
    }
    throw std::runtime_error(std::string("invalid component name: ") + std::string(name));
}

void ComponentLoader::initByJson(void *dst_ptr, const nlohmann::json &hint) const {
    const std::string name = hint.at("name");
    if (name == "transform") {
        TransformComponent &transform = *static_cast<TransformComponent *>(dst_ptr);

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
    } else if (name == "simplemodelview") {
        SimpleModelViewComponent &model = *static_cast<SimpleModelViewComponent *>(dst_ptr);

        const auto &model_name = hint.at("model");
        auto &pic = GET_MODULE(PolygonInstanceContainer);
        auto &asset_con = GET_MODULE(ModelAssetContainer);
        model.model_instance_id = pic.placeModelInstance(asset_con.getModelTemplateByName(model_name));
    }
}

} // namespace Pelican
