#include "scene.hpp"

#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"

#include "../ecs/componentinfo.hpp"
#include "basicconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "../log.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "pathresolver.hpp"
#include "sceneformat.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <components/localtransform.hpp>
#include <filesystem>
#include <optional>
#include <span>
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
    std::string name;
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
        ecs_object.name = object_name;
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

std::string lowerExtension(const std::filesystem::path &path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension;
}

ModelTemplate loadGltfTemplate(const std::filesystem::path &path) {
    auto &loader = GET_MODULE(GltfLoader);
    const auto path_string = path.string();
    return lowerExtension(path) == ".gltf" ? loader.loadGltf(path_string) : loader.loadGltfBinary(path_string);
}

SceneObjectTransform identityObjectTransform() {
    return SceneObjectTransform{
        .pos = glm::vec3{0.0f, 0.0f, 0.0f},
        .rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        .scale = glm::vec3{1.0f, 1.0f, 1.0f},
    };
}

void assignTransform(TransformComponent &dst, const SceneObjectTransform &src) {
    dst.pos = src.pos;
    dst.rotation = src.rotation;
    dst.scale = src.scale;
}

} // namespace

void SceneLoader::load(SceneId scene_id) {
    auto &ecs = GET_MODULE(ECSCore);
    auto &config = GET_MODULE(ProjectBasicConfig);
    object_bindings.clear();

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

    const auto transform_id = component_info_manager.getComponentIdByName("transform");
    const auto simple_model_view_id = component_info_manager.getComponentIdByName("simplemodelview");

    for (const auto &object : ecs_objects) {
        std::vector<void *> components_ptr;
        components_ptr.resize(object.components_id.size());
        ecs.allocateEntity(object.components_id, components_ptr, 1);

        TransformComponent *transform = nullptr;
        SimpleModelViewComponent *simple_model_view = nullptr;
        for (int i = 0; const auto &component : object.components_json) {
            GET_MODULE(ComponentInfoManager).loadByJson(components_ptr[i], component);
            if (object.components_id[i] == transform_id) {
                transform = static_cast<TransformComponent *>(components_ptr[i]);
            } else if (object.components_id[i] == simple_model_view_id) {
                simple_model_view = static_cast<SimpleModelViewComponent *>(components_ptr[i]);
            }
            i++;
        }
        if (!object.name.empty() && transform != nullptr) {
            bindObjectTransform(object.name, transform, simple_model_view);
        }
    }
}

void SceneLoader::bindObjectTransform(const std::string &name, void *transform, void *simple_model_view) {
    if (name.empty() || transform == nullptr) {
        return;
    }
    if (object_bindings.contains(name)) {
        throw std::runtime_error("duplicate object name for transform binding: " + name);
    }
    object_bindings.emplace(name, ObjectBinding{
                                      .transform = transform,
                                      .simple_model_view = simple_model_view,
                                  });
}

bool SceneLoader::hasObjectTransform(std::string_view name) const {
    return object_bindings.find(std::string{name}) != object_bindings.end();
}

void SceneLoader::applyObjectTransform(std::string_view name, const SceneObjectTransform &transform) {
    const auto binding_it = object_bindings.find(std::string{name});
    if (binding_it == object_bindings.end()) {
        throw std::runtime_error("unknown object name: " + std::string{name});
    }

    const auto &binding = binding_it->second;
    auto *bound_transform = static_cast<TransformComponent *>(binding.transform);
    auto *simple_model_view = static_cast<SimpleModelViewComponent *>(binding.simple_model_view);
    assignTransform(*bound_transform, transform);
    if (simple_model_view != nullptr && simple_model_view->model_instance_id) {
        GET_MODULE(PolygonInstanceContainer)
            .setTrs(*simple_model_view->model_instance_id, transform.pos, transform.rotation, transform.scale);
    }
}

std::filesystem::path SceneLoader::loadTransientGltf(std::string_view path_ref, const std::optional<std::string> &name) {
    const auto path = GET_MODULE(PathResolver).resolveExistingFile(path_ref);
    auto model_template = loadGltfTemplate(path);
    const auto model_instance_id = GET_MODULE(PolygonInstanceContainer).placeModelInstance(model_template);

    auto &component_info_manager = GET_MODULE(ComponentInfoManager);
    const std::array<ComponentId, 2> component_ids{
        component_info_manager.getComponentIdByName("transform"),
        component_info_manager.getComponentIdByName("simplemodelview"),
    };
    std::array<void *, 2> component_ptrs{};
    GET_MODULE(ECSCore).allocateEntity(std::span<const ComponentId>{component_ids}, std::span<void *>{component_ptrs},
                                       1);

    auto *transform = static_cast<TransformComponent *>(component_ptrs[0]);
    auto *simple_model_view = static_cast<SimpleModelViewComponent *>(component_ptrs[1]);
    component_info_manager.initComponent(component_ids[0], transform);
    component_info_manager.initComponent(component_ids[1], simple_model_view);

    const auto initial_transform = identityObjectTransform();
    assignTransform(*transform, initial_transform);
    simple_model_view->model_instance_id = model_instance_id;
    GET_MODULE(PolygonInstanceContainer)
        .setTrs(model_instance_id, initial_transform.pos, initial_transform.rotation, initial_transform.scale);

    if (name && !name->empty()) {
        bindObjectTransform(*name, transform, simple_model_view);
    }
    return path;
}

} // namespace Pelican
