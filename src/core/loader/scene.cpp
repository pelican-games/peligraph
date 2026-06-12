#include "scene.hpp"

#include "../ecs/core.hpp"
#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"

#include "../ecs/componentinfo.hpp"
#include "basicconfig.hpp"
#include "../light/lightcontainer.hpp"
#include <nlohmann/json.hpp>

#include <components/localtransform.hpp>
#include <optional>
#include <vector>

namespace Pelican {

SceneLoader::SceneLoader() {}
SceneLoader::~SceneLoader() {}

namespace {

nlohmann::json makeSimpleModelViewUpdate(const nlohmann::json &component) {
    return nlohmann::json{
        {"name", "simplemodelviewupdate"},
        {"model", component.at("model")},
    };
}

std::vector<nlohmann::json> expandSceneComponents(const nlohmann::json &components_json) {
    std::vector<nlohmann::json> components;
    components.reserve(components_json.size() + 1);

    bool has_simple_model_view_update = false;
    std::optional<nlohmann::json> implicit_simple_model_view_update;

    for (const auto &component : components_json) {
        const std::string name = component.at("name");
        if (name == "simplemodelviewupdate") {
            has_simple_model_view_update = true;
        } else if (name == "simplemodelview" && component.contains("model")) {
            // Preserve scene files that stored the model name directly on simplemodelview.
            implicit_simple_model_view_update = makeSimpleModelViewUpdate(component);
        }
        components.push_back(component);
    }

    if (implicit_simple_model_view_update && !has_simple_model_view_update) {
        components.push_back(std::move(*implicit_simple_model_view_update));
    }

    return components;
}

} // namespace

void SceneLoader::load(SceneId scene_id) {
    auto &ecs = GET_MODULE(ECSCore);
    auto &config = GET_MODULE(ProjectBasicConfig);

    // load from json
    const auto scene_data = nlohmann::json::parse(config.sceneDataJson()).at(scene_id);

    GET_MODULE(LightContainer).load(scene_data);

    const auto &objects = scene_data.at("objects");

    for (const auto &object : objects) {
        const auto components_json = expandSceneComponents(object.at("components"));
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
