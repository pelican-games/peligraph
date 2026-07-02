#include "scene.hpp"

#include "../ecs/core.hpp"
#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"

#include "../ecs/componentinfo.hpp"
#include "basicconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "../log.hpp"
#include "sceneformat.hpp"
#include <nlohmann/json.hpp>

#include <components/localtransform.hpp>
#include <optional>
#include <stdexcept>
#include <utility>
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

std::string displayObjectName(const std::string &object_name) {
    return object_name.empty() ? std::string{"<unnamed>"} : object_name;
}

ComponentId getComponentIdForObject(ComponentInfoManager &component_info_manager, const std::string &component_name,
                                    const std::string &object_name) {
    try {
        return component_info_manager.getComponentIdByName(component_name);
    } catch (const std::out_of_range &) {
        throw std::runtime_error("Unknown component '" + component_name + "' on object '" +
                                 displayObjectName(object_name) + "'");
    }
}

struct EcsObjectLoad {
    std::vector<nlohmann::json> components_json;
    std::vector<ComponentId> components_id;
};

std::vector<EcsObjectLoad> prepareSceneBindings(const nlohmann::json &objects, ComponentInfoManager &component_info_manager,
                                                std::vector<LightLoadEntry> &light_entries) {
    std::vector<EcsObjectLoad> ecs_objects;
    ecs_objects.reserve(objects.size());

    for (const auto &object : objects) {
        const auto object_name = object.value("name", std::string{});
        const auto components_json = expandSceneComponents(object.at("components"));

        EcsObjectLoad ecs_object;
        ecs_object.components_json.reserve(components_json.size());
        ecs_object.components_id.reserve(components_json.size());

        for (const auto &component : components_json) {
            const std::string component_name = component.at("name");
            if (component_name == "light") {
                light_entries.push_back(LightLoadEntry{object_name, component});
                continue;
            }

            ecs_object.components_json.push_back(component);
            ecs_object.components_id.push_back(
                getComponentIdForObject(component_info_manager, component_name, object_name));
        }

        if (!ecs_object.components_json.empty()) {
            ecs_objects.push_back(std::move(ecs_object));
        }
    }

    return ecs_objects;
}

} // namespace

void SceneLoader::load(SceneId scene_id) {
    auto &ecs = GET_MODULE(ECSCore);
    auto &config = GET_MODULE(ProjectBasicConfig);

    const auto scene_document = normalizeSceneDataJson(nlohmann::json::parse(config.sceneDataJson()));
    for (const auto &warning : scene_document.warnings) {
        if (logger != nullptr) {
            LOG_WARNING(logger, "{}", warning);
        }
    }

    const auto scene_it = scene_document.scenes.find(scene_id);
    if (scene_it == scene_document.scenes.end()) {
        throw std::runtime_error("scene not found: " + scene_id);
    }

    std::vector<LightLoadEntry> light_entries;
    const auto &objects = scene_it.value().at("objects");
    auto &component_info_manager = GET_MODULE(ComponentInfoManager);
    const auto ecs_objects = prepareSceneBindings(objects, component_info_manager, light_entries);

    GET_MODULE(LightContainer).load(light_entries);

    for (const auto &object : ecs_objects) {
        std::vector<void *> components_ptr;
        components_ptr.resize(object.components_id.size());
        ecs.allocateEntity(object.components_id, components_ptr, 1);

        for (int i = 0; const auto &component : object.components_json) {
            GET_MODULE(ComponentInfoManager).loadByJson(components_ptr[i], component);
            i++;
        }
    }
}

} // namespace Pelican
