#include "scene.hpp"

#include "../ecs/core.hpp"
#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include "basicconfig.hpp"
#include "component.hpp"
#include <nlohmann/json.hpp>

#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"

namespace Pelican {

SceneLoader::SceneLoader() {}
SceneLoader::~SceneLoader() {}

void SceneLoader::load(SceneId scene_id) {
    // for test
    auto &camera = GET_MODULE(Camera);
    camera.setPos({3.0, 3.0, 3.0});
    camera.setDir({-3.0, -3.0, -3.0});

    auto &ecs = GET_MODULE(ECSCore);
    auto &config = GET_MODULE(ProjectBasicConfig);

    // TODO: load from config file
    // TODO: separate into asset loader class
    {
        auto &loader = GET_MODULE(GltfLoader);
        auto model = loader.loadGltfBinary("AliciaSolid.vrm");
    }

    // load from json
    const auto scene_data = nlohmann::json::parse(config.sceneDataJson()).at(scene_id);
    const auto &objects = scene_data.at("objects");

    for (const auto &object : objects) {
        const auto &components_json = object.at("components");
        std::vector<ComponentId> components_id;
        components_id.reserve(components_json.size());
        for (const auto &component : components_json) {
            const std::string name = component.at("name");

            // TODO
        }

        std::vector<void *> components_ptr;
        components_ptr.resize(components_id.size());
    }
}

} // namespace Pelican