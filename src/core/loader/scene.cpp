#include "scene.hpp"

#include "../ecs/core.hpp"
#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"

#include "../ecs/componentinfo.hpp"
#include "basicconfig.hpp"
#include <nlohmann/json.hpp>

#include <components/localtransform.hpp>

namespace Pelican {

SceneLoader::SceneLoader() {}
SceneLoader::~SceneLoader() {}

void SceneLoader::load(SceneId scene_id) {
    auto &ecs = GET_MODULE(ECSCore);
    auto &config = GET_MODULE(ProjectBasicConfig);

    // load from json
    const auto scene_data = nlohmann::json::parse(config.sceneDataJson()).at(scene_id);
    const auto &objects = scene_data.at("objects");

    for (const auto &object : objects) {
        const auto &components_json = object.at("components");
        std::vector<ComponentId> components_id;
        components_id.reserve(components_json.size());
        for (const auto &component : components_json) {
            const std::string name = component.at("name");
            components_id.push_back(GET_MODULE(ComponentInfoManager).getComponentIdByName(name.c_str()));
        }

        std::vector<void *> components_ptr;
        components_ptr.resize(components_id.size());
        ecs.allocateEntity(components_id, components_ptr, 1);

        for (int i = 0; const auto &component : components_json) {
            GET_MODULE(ComponentInfoManager).loadByJson(components_ptr[i], component);
            i++;
        }
    }
}

} // namespace Pelican