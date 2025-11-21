#include "scene.hpp"

#include "../ecs/core.hpp"
#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include "basicconfig.hpp"
#include <nlohmann/json.hpp>

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
    auto &loader = GET_MODULE(GltfLoader);
    auto model = loader.loadGltfBinary("AliciaSolid.vrm");

    // load from json
    const auto scene_data = nlohmann::json::parse(config.sceneDataJson()).at(scene_id);
    const auto &objects = scene_data.at("objects");

    for (const auto &object : objects) {
        std::vector<ComponentDataRef> components;
        for (const auto &component : object.at("components")) {
            const std::string name = component.at("name");
            // TODO
            if (name == "transform") {

            }
            if (name == "model") {
                auto &pic = GET_MODULE(PolygonInstanceContainer);
                pic.placeModelInstance(model);
            }
            // components.push_back(component);
        }
        ecs.insert_dynamic(components);
    }
}

} // namespace Pelican