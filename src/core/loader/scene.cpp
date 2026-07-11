#include "scene.hpp"

#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../ecs/core.hpp"
#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"

#include "../ecs/componentinfo.hpp"
#include "basicconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "../log.hpp"
#include "../phys/physworld.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../userpublic/events.hpp"
#include "../userpublic/gameobjects.hpp"
#include "pathresolver.hpp"
#include "../../project/sceneformat.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <components/localtransform.hpp>
#include <components/predefined.hpp>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {

SceneLoader::SceneLoader() {}
SceneLoader::~SceneLoader() {}

namespace {

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
    std::string parent;
    bool hierarchy_participant = false;
    std::vector<nlohmann::json> components_json;
    std::vector<ComponentId> components_id;
    std::vector<ColliderComponent> colliders;
};

ColliderComponent loadColliderComponent(const nlohmann::json &component, const std::string &object_name) {
    try {
        ColliderComponent collider;
        JsonArchiveLoader archive{static_cast<const void *>(&component)};
        collider.ref(archive);
        return collider;
    } catch (const std::exception &ex) {
        throw std::runtime_error("Invalid collider on object '" + displayObjectName(object_name) + "': " + ex.what());
    }
}

std::vector<EcsObjectLoad> prepareSceneBindings(const nlohmann::json &objects, ComponentInfoManager &component_info_manager,
                                                std::vector<LightLoadEntry> &light_entries) {
    std::vector<EcsObjectLoad> ecs_objects;
    ecs_objects.reserve(objects.size());

    std::unordered_set<std::string> parent_names;
    for (const auto &object : objects) {
        if (const auto parent = object.find("parent"); parent != object.end()) {
            parent_names.insert(parent->get<std::string>());
        }
    }
    const bool scene_uses_parents = !parent_names.empty();
    const auto local_transform_id = scene_uses_parents
                                        ? std::optional<ComponentId>{
                                              component_info_manager.getComponentIdByName("localtransform")}
                                        : std::nullopt;

    for (const auto &object : objects) {
        const auto object_name = object.value("name", std::string{});
        const auto parent_name = object.value("parent", std::string{});
        const auto &components_json = object.at("components");

        EcsObjectLoad ecs_object;
        ecs_object.name = object_name;
        ecs_object.parent = parent_name;
        ecs_object.hierarchy_participant = !parent_name.empty() || parent_names.contains(object_name);
        ecs_object.components_json.reserve(components_json.size());
        ecs_object.components_id.reserve(components_json.size());

        bool has_transform = false;
        bool has_local_transform = false;
        for (const auto &component : components_json) {
            const auto component_name = component.at("name").get<std::string>();
            has_transform = has_transform || component_name == "transform";
            has_local_transform = has_local_transform || component_name == "localtransform";
        }
        if (ecs_object.hierarchy_participant && !has_transform) {
            throw std::runtime_error("parent hierarchy object '" + displayObjectName(object_name) +
                                     "' requires a transform component");
        }

        for (const auto &component : components_json) {
            const std::string component_name = component.at("name");
            if (component_name == "light") {
                light_entries.push_back(LightLoadEntry{object_name, component});
                continue;
            }
            if (component_name == "collider") {
                ecs_object.colliders.push_back(loadColliderComponent(component, object_name));
                continue;
            }

            if (component_name == "animation") {
                if (component.contains("graph")) {
                    throw std::runtime_error("animation component on object '" +
                                             displayObjectName(object_name) +
                                             "' uses reserved v1 key 'graph'");
                }
                if (!component.contains("clip") || !component.at("clip").is_string()) {
                    throw std::runtime_error("animation component on object '" +
                                             displayObjectName(object_name) +
                                             "' requires string clip");
                }
                auto normalized_animation = component;
                normalized_animation["speed"] = component.value("speed", 1.0);
                normalized_animation["start_time"] = component.value("start_time", 0.0);
                if (component.contains("loop") && !component.at("loop").is_boolean()) {
                    throw std::runtime_error("animation component loop must be boolean on object '" +
                                             displayObjectName(object_name) + "'");
                }
                normalized_animation["loop"] = component.value("loop", true) ? 1 : 0;
                ecs_object.components_json.push_back(std::move(normalized_animation));
                ecs_object.components_id.push_back(
                    getComponentIdForObject(component_info_manager, component_name, object_name));
                continue;
            }

            ecs_object.components_json.push_back(component);
            ecs_object.components_id.push_back(
                getComponentIdForObject(component_info_manager, component_name, object_name));
            if (ecs_object.hierarchy_participant && component_name == "transform" && !has_local_transform) {
                auto local_component = component;
                local_component["name"] = "localtransform";
                ecs_object.components_json.push_back(std::move(local_component));
                ecs_object.components_id.push_back(*local_transform_id);
            }
        }

        if (!ecs_object.components_json.empty() || !ecs_object.colliders.empty()) {
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

ModelTemplate loadGltfTemplate(const std::filesystem::path &path,
                               std::optional<AssetFragmentRef> fragment = std::nullopt) {
    auto &loader = GET_MODULE(GltfLoader);
    const auto path_string = path.string();
    return lowerExtension(path) == ".gltf" ? loader.loadGltf(path_string, std::move(fragment))
                                           : loader.loadGltfBinary(path_string, std::move(fragment));
}

SceneObjectTransform identityObjectTransform() {
    return SceneObjectTransform{
        .pos = glm::vec3{0.0f, 0.0f, 0.0f},
        .rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        .scale = glm::vec3{1.0f, 1.0f, 1.0f},
    };
}

PhysWorldTransform identityPhysWorldTransform() {
    return PhysWorldTransform{
        .pos = vec3{0.0f, 0.0f, 0.0f},
        .rotation = quat{0.0f, 0.0f, 0.0f, 1.0f},
        .scale = vec3{1.0f, 1.0f, 1.0f},
    };
}

void assignTransform(TransformComponent &dst, const SceneObjectTransform &src) {
    dst.pos = src.pos;
    dst.rotation = src.rotation;
    dst.scale = src.scale;
}

} // namespace

void SceneLoader::load(SceneId scene_id) {
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

    const auto transform_id = component_info_manager.getComponentIdByName("transform");

    clearRuntimeScene();
    GET_MODULE(LightContainer).load(light_entries);
    GET_MODULE(Camera).loadSceneCameras(scene_id);

    auto &phys_world = GET_MODULE(PhysWorld);
    const bool scene_uses_parents = std::any_of(ecs_objects.begin(), ecs_objects.end(), [](const auto &object) {
        return object.hierarchy_participant;
    });
    if (scene_uses_parents) {
        std::vector<GameObjectId> object_ids(ecs_objects.size(), invalidGameObjectId);
        std::unordered_map<std::string, GameObjectId> object_ids_by_name;

        for (size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
            const auto &object = ecs_objects[object_index];
            if (!object.components_id.empty()) {
                object_ids[object_index] = GameObjects::createWithComponents(
                    object.components_id, [&](std::span<void *> ptrs) {
                        for (size_t i = 0; i < object.components_json.size(); ++i) {
                            component_info_manager.loadByJson(ptrs[i], object.components_json[i]);
                        }
                    });
            }
            if (!object.name.empty() && object_ids[object_index] != invalidGameObjectId) {
                object_ids_by_name.emplace(object.name, object_ids[object_index]);
            }
            const bool has_transform =
                object_ids[object_index] != invalidGameObjectId &&
                std::find(object.components_id.begin(), object.components_id.end(), transform_id) !=
                    object.components_id.end();
            if (!object.name.empty() && has_transform) {
                bindObjectTransform(object.name, object_ids[object_index]);
            }
        }

        auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
        for (size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
            const auto &object = ecs_objects[object_index];
            if (!object.hierarchy_participant) {
                continue;
            }
            auto *local = ecs.tryComponent<LocalTransformComponent>(object_ids[object_index]);
            if (local == nullptr) {
                throw std::runtime_error("parent hierarchy object '" + displayObjectName(object.name) +
                                         "' has no localtransform ECS component");
            }
            if (!object.parent.empty()) {
                const auto parent = object_ids_by_name.find(object.parent);
                if (parent == object_ids_by_name.end() ||
                    ecs.tryComponent<TransformComponent>(parent->second) == nullptr) {
                    throw std::runtime_error("parent '" + object.parent + "' for object '" +
                                             displayObjectName(object.name) + "' has no ECS transform");
                }
                local->parent = parent->second;
                (void)ecs.markComponentChanged(object_ids[object_index],
                                               ComponentIdByType<LocalTransformComponent>::value);
            }
        }

        for (size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
            const auto &object = ecs_objects[object_index];
            const auto object_id = object_ids[object_index];
            const bool has_transform = object_id != invalidGameObjectId &&
                                       ecs.tryComponent<TransformComponent>(object_id) != nullptr;
            for (const auto &collider : object.colliders) {
                if (has_transform) {
                    phys_world.bindCollider(object.name, collider, object_id);
                } else {
                    phys_world.bindCollider(object.name, collider, identityPhysWorldTransform());
                }
            }
        }
    } else {
        for (const auto &object : ecs_objects) {
            GameObjectId object_id = invalidGameObjectId;
            if (!object.components_id.empty()) {
                object_id = GameObjects::createWithComponents(object.components_id, [&](std::span<void *> ptrs) {
                    for (size_t i = 0; i < object.components_json.size(); ++i) {
                        component_info_manager.loadByJson(ptrs[i], object.components_json[i]);
                    }
                });
            }
            const bool has_transform =
                object_id != invalidGameObjectId &&
                std::find(object.components_id.begin(), object.components_id.end(), transform_id) !=
                    object.components_id.end();
            if (!object.name.empty() && has_transform) {
                bindObjectTransform(object.name, object_id);
            }
            for (const auto &collider : object.colliders) {
                if (has_transform) {
                    phys_world.bindCollider(object.name, collider, object_id);
                } else {
                    phys_world.bindCollider(object.name, collider, identityPhysWorldTransform());
                }
            }
        }
    }
    current_scene_id = std::move(scene_id);
    internal::getEventRegisterer().emit(SceneLoaded{current_scene_id});
}

void SceneLoader::requestLoad(SceneId scene_id) {
    auto &config = GET_MODULE(ProjectBasicConfig);
    const auto scene_document = normalizeSceneDataJson(nlohmann::json::parse(config.sceneDataJson()));
    if (scene_document.scenes.find(scene_id) == scene_document.scenes.end()) {
        throw std::runtime_error("scene not found: " + scene_id);
    }
    pending_scene_id = std::move(scene_id);
}

bool SceneLoader::applyPendingLoad() {
    if (!pending_scene_id) {
        return false;
    }
    auto scene_id = std::move(*pending_scene_id);
    pending_scene_id.reset();
    load(std::move(scene_id));
    return true;
}

const SceneId &SceneLoader::currentScene() const {
    return current_scene_id;
}

void SceneLoader::clearRuntimeScene() {
    object_bindings.clear();
    GET_MODULE(PhysWorld).clear();
    GameObjects::removeAll();
    GET_MODULE(PolygonInstanceContainer).clear();
}

void SceneLoader::bindObjectTransform(const std::string &name, GameObjectId object_id) {
    if (name.empty() ||
        GET_MODULE(ECSCore).getTemplatePublicModule().tryComponent<TransformComponent>(object_id) == nullptr) {
        return;
    }
    if (const auto existing = object_bindings.find(name); existing != object_bindings.end()) {
        if (GET_MODULE(ECSCore).getTemplatePublicModule().tryComponent<TransformComponent>(existing->second.object_id) ==
            nullptr) {
            object_bindings.erase(existing);
        } else {
            throw std::runtime_error("duplicate object name for transform binding: " + name);
        }
    }
    object_bindings.emplace(name, ObjectBinding{
                                      .object_id = object_id,
                                  });
}

std::optional<GameObjectId> SceneLoader::objectId(std::string_view name) const {
    const auto found = object_bindings.find(std::string{name});
    if (found == object_bindings.end() ||
        GET_MODULE(ECSCore).getTemplatePublicModule().tryComponent<TransformComponent>(found->second.object_id) ==
            nullptr) {
        return std::nullopt;
    }
    return found->second.object_id;
}

bool SceneLoader::hasObjectTransform(std::string_view name) const {
    return objectId(name).has_value();
}

SceneObjectTransform SceneLoader::objectTransform(std::string_view name) const {
    const auto binding_it = object_bindings.find(std::string{name});
    if (binding_it == object_bindings.end()) {
        throw std::runtime_error("unknown object name: " + std::string{name});
    }

    const auto object_id = binding_it->second.object_id;
    const auto *bound_transform =
        GET_MODULE(ECSCore).getTemplatePublicModule().tryComponent<TransformComponent>(object_id);
    if (bound_transform == nullptr) {
        throw std::runtime_error("object was deleted: " + std::string{name} + " (" + toString(object_id) + ")");
    }
    return SceneObjectTransform{
        .pos = bound_transform->pos,
        .rotation = bound_transform->rotation,
        .scale = bound_transform->scale,
    };
}

void SceneLoader::applyObjectTransform(std::string_view name, const SceneObjectTransform &transform) {
    const auto binding_it = object_bindings.find(std::string{name});
    if (binding_it == object_bindings.end()) {
        throw std::runtime_error("unknown object name: " + std::string{name});
    }

    const auto &binding = binding_it->second;
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto *bound_transform = ecs.tryComponent<TransformComponent>(binding.object_id);
    auto *simple_model_view = ecs.tryComponent<SimpleModelViewComponent>(binding.object_id);
    if (bound_transform == nullptr) {
        throw std::runtime_error("object was deleted: " + std::string{name} + " (" +
                                 toString(binding.object_id) + ")");
    }
    assignTransform(*bound_transform, transform);
    (void)ecs.markComponentChanged(binding.object_id, ComponentIdByType<TransformComponent>::value);
    if (simple_model_view != nullptr && simple_model_view->model_instance_id) {
        GET_MODULE(PolygonInstanceContainer)
            .setTrs(*simple_model_view->model_instance_id, transform.pos, transform.rotation, transform.scale);
    }
}

std::filesystem::path SceneLoader::loadTransientGltf(std::string_view path_ref, const std::optional<std::string> &name) {
    const auto resolved = GET_MODULE(PathResolver).resolveExistingFileReference(path_ref);
    const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved);
    const auto path = fragment != nullptr ? fragment->path : std::get<std::filesystem::path>(resolved);
    auto model_template = loadGltfTemplate(path, fragment != nullptr
                                                     ? std::optional<AssetFragmentRef>{fragment->fragment}
                                                     : std::nullopt);
    const auto model_instance_id = GET_MODULE(PolygonInstanceContainer).placeModelInstance(model_template);

    auto &component_info_manager = GET_MODULE(ComponentInfoManager);
    const std::array<ComponentId, 2> component_ids{
        component_info_manager.getComponentIdByName("transform"),
        component_info_manager.getComponentIdByName("simplemodelview"),
    };
    const auto initial_transform = identityObjectTransform();
    GameObjectId object_id = invalidGameObjectId;
    try {
        object_id = GameObjects::createWithComponents(component_ids, [&](std::span<void *> ptrs) {
            assignTransform(*static_cast<TransformComponent *>(ptrs[0]), initial_transform);
            static_cast<SimpleModelViewComponent *>(ptrs[1])->model_instance_id = model_instance_id;
        });
    } catch (...) {
        GET_MODULE(PolygonInstanceContainer).removeModelInstance(model_instance_id);
        throw;
    }
    GET_MODULE(PolygonInstanceContainer)
        .setTrs(model_instance_id, initial_transform.pos, initial_transform.rotation, initial_transform.scale);

    if (name && !name->empty()) {
        bindObjectTransform(*name, object_id);
    }
    return path;
}

} // namespace Pelican
