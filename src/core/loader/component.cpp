#pragma once

#include "component.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include "../ecs/predefined/transform.hpp"

namespace Pelican {

void ComponentLoader::initByJson(const nlohmann::json &hint) const {
    const std::string name = hint.at("name");
    if (name == "transform") {
        const auto &pos = hint.at("pos");
        const auto &rot = hint.at("rotation");
        const auto &scale = hint.at("scale");
        TransformComponent transform;
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
    }
    if (name == "model") {
        auto &pic = GET_MODULE(PolygonInstanceContainer);
        // pic.placeModelInstance(model);
    }
}

} // namespace Pelican
