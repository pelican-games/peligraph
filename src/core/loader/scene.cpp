#include "scene.hpp"

#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../ecs/core.hpp"
#include "../model/gltf.hpp"
#include "../playback/seqplayer.hpp"
#include "../playback/vatplayer.hpp"
#include "../renderer/camera.hpp"

#include "../ecs/componentinfo.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../gamelogic/gamelogicreload.hpp"
#include "basicconfig.hpp"
#include "componentcodec.hpp"
#include "../light/lightcontainer.hpp"
#include "../log.hpp"
#include "../build_features.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physworld.hpp"
#endif
#include "../renderer/polygoninstancecontainer.hpp"
#include "../userpublic/events.hpp"
#include "../userpublic/gameobjects.hpp"
#include "pathresolver.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <components/localtransform.hpp>
#include <components/predefined.hpp>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {

void internal::validateResolvedSceneFeatureBindings(
    const ResolvedSceneView &scene) {
#if !PELICAN_WITH_PHYSICS
    for (const auto &object : scene.objects) {
        if (std::any_of(object.components.begin(), object.components.end(),
                        [](const auto &component) {
                            return component.name == "collider";
                        })) {
            throwBuildFeatureDisabled("PELICAN_WITH_PHYSICS",
                                      "scene contains collider components");
        }
    }
#else
    (void)scene;
#endif
}

SceneLoader::SceneLoader() {}
SceneLoader::~SceneLoader() { releaseTransientModels(false); }

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
    struct ComponentLoad {
        nlohmann::json effective_json;
        const ComponentCodec *codec = nullptr;
        ComponentCodecValue decoded;
    };

    AuthoringObjectId authoring_object_id{};
    std::string name;
    std::string parent;
    bool hierarchy_participant = false;
    std::vector<ComponentLoad> components;
    std::vector<ComponentId> components_id;
    std::vector<ColliderComponent> colliders;
    std::vector<PreparedSceneBehaviorAttachment> behaviors;
};

ColliderComponent loadColliderComponent(const ResolvedComponent &component,
                                        const std::string &object_name) {
    try {
        ColliderComponent collider;
        if (component.runtime_codec == nullptr) {
            throw std::logic_error("resolved collider has no runtime codec");
        }
        component.runtime_codec->applyRuntime(component.requireRuntimeValue(),
                                              &collider);
        return collider;
    } catch (const std::exception &ex) {
        throw std::runtime_error("Invalid collider on object '" + displayObjectName(object_name) + "': " + ex.what());
    }
}

std::vector<EcsObjectLoad> prepareSceneBindings(const ResolvedSceneView &scene,
                                                ComponentInfoManager &component_info_manager,
                                                std::vector<LightLoadEntry> &light_entries,
                                                std::vector<PreparedSceneBehaviorAttachment> behavior_entries) {
    std::vector<EcsObjectLoad> ecs_objects;
    ecs_objects.reserve(scene.objects.size());

    std::vector<std::vector<PreparedSceneBehaviorAttachment>> behaviors_by_object(
        scene.objects.size());
    for (auto &entry : behavior_entries) {
        behaviors_by_object.at(entry.object_index).push_back(std::move(entry));
    }

    std::unordered_set<std::string> parent_names;
    for (const auto &object : scene.objects) {
        if (object.parent) parent_names.insert(*object.parent);
    }
    const bool scene_uses_parents = !parent_names.empty();
    const auto local_transform_id = scene_uses_parents
                                        ? std::optional<ComponentId>{
                                              component_info_manager.getComponentIdByName("localtransform")}
                                        : std::nullopt;

    for (std::size_t object_index = 0; object_index < scene.objects.size();
         ++object_index) {
        const auto &object = scene.objects[object_index];
        const auto object_name = object.name.value_or(std::string{});
        const auto parent_name = object.parent.value_or(std::string{});

        EcsObjectLoad ecs_object;
        ecs_object.authoring_object_id = object.authoring_object_id;
        ecs_object.name = object_name;
        ecs_object.parent = parent_name;
        ecs_object.hierarchy_participant = !parent_name.empty() || parent_names.contains(object_name);
        ecs_object.behaviors = std::move(behaviors_by_object[object_index]);
        ecs_object.components.reserve(object.components.size());
        ecs_object.components_id.reserve(object.components.size());

        bool has_transform = false;
        bool has_local_transform = false;
        for (const auto &component : object.components) {
            const auto &component_name = component.name;
            has_transform = has_transform || component_name == "transform";
            has_local_transform = has_local_transform || component_name == "localtransform";
        }
        if (ecs_object.hierarchy_participant && !has_transform) {
            throw std::runtime_error("parent hierarchy object '" + displayObjectName(object_name) +
                                     "' requires a transform component");
        }

        for (const auto &component : object.components) {
            const auto &component_name = component.name;
            if (component_name == "light") {
                try {
                    (void)component.requireRuntimeValue();
                } catch (const std::exception &error) {
                    throw std::runtime_error(
                        "Invalid light on object '" +
                        displayObjectName(object_name) + "': " +
                        error.what());
                }
                light_entries.push_back(LightLoadEntry{
                    object_name, component.effective_json});
                continue;
            }
            if (component_name == "collider") {
                ecs_object.colliders.push_back(loadColliderComponent(component, object_name));
                continue;
            }
            if (component_name == "behavior") {
                continue;
            }

            ecs_object.components.push_back(EcsObjectLoad::ComponentLoad{
                .effective_json = component.effective_json,
                .codec = component.runtime_codec,
                .decoded = component.runtime_codec != nullptr
                               ? component.requireRuntimeValue()
                               : ComponentCodecValue{},
            });
            ecs_object.components_id.push_back(
                getComponentIdForObject(component_info_manager, component_name, object_name));
            if (ecs_object.hierarchy_participant && component_name == "transform" && !has_local_transform) {
                auto local_component = component.effective_json;
                local_component["name"] = "localtransform";
                ecs_object.components.push_back(EcsObjectLoad::ComponentLoad{
                    .effective_json = std::move(local_component),
                });
                ecs_object.components_id.push_back(*local_transform_id);
            }
        }

        if (!ecs_object.components.empty() || !ecs_object.colliders.empty() ||
            !ecs_object.behaviors.empty()) {
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

PreparedGltf prepareGltfTemplate(const std::filesystem::path &path,
                                 std::optional<AssetFragmentRef> fragment = std::nullopt) {
    auto &loader = GET_MODULE(GltfLoader);
    const auto path_string = path.string();
    return lowerExtension(path) == ".gltf" ? loader.prepareGltf(path_string, std::move(fragment))
                                           : loader.prepareGltfBinary(path_string, std::move(fragment));
}

struct UnpublishedModelCandidate {
    ModelTemplate model;
    bool owns_resources = true;

    ~UnpublishedModelCandidate() {
        if (owns_resources) releaseModelGpuResources(model, false);
    }
};

SceneObjectTransform identityObjectTransform() {
    return SceneObjectTransform{
        .pos = glm::vec3{0.0f, 0.0f, 0.0f},
        .rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        .scale = glm::vec3{1.0f, 1.0f, 1.0f},
    };
}

#if PELICAN_WITH_PHYSICS
PhysWorldTransform identityPhysWorldTransform() {
    return PhysWorldTransform{
        .pos = vec3{0.0f, 0.0f, 0.0f},
        .rotation = quat{0.0f, 0.0f, 0.0f, 1.0f},
        .scale = vec3{1.0f, 1.0f, 1.0f},
    };
}
#endif

void assignTransform(TransformComponent &dst, const SceneObjectTransform &src) {
    dst.pos = src.pos;
    dst.rotation = src.rotation;
    dst.scale = src.scale;
}

void applyComponentLoad(ComponentInfoManager &component_info_manager, void *target,
                        const EcsObjectLoad::ComponentLoad &component) {
    if (component.codec == nullptr || component.codec->runtime_kind != ComponentCodecRuntimeKind::Ecs) {
        component_info_manager.loadByJson(target, component.effective_json);
        return;
    }
    if (component.codec->name == "transform") {
        TransformCodecTarget transform_target{.world = static_cast<TransformComponent *>(target)};
        component.codec->applyRuntime(component.decoded, &transform_target);
        return;
    }
    component.codec->applyRuntime(component.decoded, target);
}

const EcsObjectLoad::ComponentLoad &transformLoad(const EcsObjectLoad &object) {
    const auto found = std::find_if(object.components.begin(), object.components.end(), [](const auto &component) {
        return component.codec != nullptr && component.codec->name == "transform";
    });
    if (found == object.components.end()) {
        throw std::logic_error("hierarchy participant has no prepared transform codec");
    }
    return *found;
}

} // namespace

void SceneLoader::load(SceneId scene_id) {
    auto &config = GET_MODULE(ProjectBasicConfig);

    const auto &scene_document = config.resolvedScene();
    for (const auto &warning : scene_document.warnings()) {
        if (logger != nullptr) {
            LOG_WARNING(logger, "{}", warning);
        }
    }

    const auto *scene = scene_document.findScene(scene_id);
    if (scene == nullptr) {
        throw std::runtime_error("scene not found: " + scene_id);
    }
    internal::validateResolvedSceneFeatureBindings(*scene);
    if (runtime_scene_epoch == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("runtime scene epoch space exhausted");
    }

    std::vector<LightLoadEntry> light_entries;
    auto &component_info_manager = GET_MODULE(ComponentInfoManager);
    const auto game_logic_status = configuredGameLogicStatus();
    const auto behavior_availability = game_logic_status.loaded
                                           ? BehaviorRegistryAvailability::active
                                           : BehaviorRegistryAvailability::dll_unavailable;
    auto behavior_entries = prepareResolvedSceneBehaviorAttachments(
        scene->objects, behavior_availability);
    auto ecs_objects = prepareSceneBindings(*scene, component_info_manager,
                                            light_entries,
                                            std::move(behavior_entries));

    const auto transform_id = component_info_manager.getComponentIdByName("transform");

    clearRuntimeScene();
    GET_MODULE(LightContainer).load(light_entries);
    GET_MODULE(Camera).loadSceneCameras(scene_id);

#if PELICAN_WITH_PHYSICS
    auto &phys_world = GET_MODULE(PhysWorld);
#endif
    const bool scene_uses_parents = std::any_of(ecs_objects.begin(), ecs_objects.end(), [](const auto &object) {
        return object.hierarchy_participant;
    });
    std::vector<GameObjectId> object_ids(ecs_objects.size(), invalidGameObjectId);
    if (scene_uses_parents) {
        std::unordered_map<std::string, GameObjectId> object_ids_by_name;

        for (size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
            const auto &object = ecs_objects[object_index];
            if (!object.components_id.empty() || !object.behaviors.empty()) {
                object_ids[object_index] = GameObjects::createWithComponents(
                    object.components_id, [&](std::span<void *> ptrs) {
                        for (size_t i = 0; i < object.components.size(); ++i) {
                            applyComponentLoad(component_info_manager, ptrs[i], object.components[i]);
                        }
                    });
                bindRuntimeObject(object.authoring_object_id,
                                  object_ids[object_index]);
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

        // Project authored local TRS to world before any SceneLoaded observer can
        // query the runtime. This is the same recurrence as LocalTransformSystem.
        std::unordered_map<std::string, size_t> object_indices_by_name;
        for (size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
            if (!ecs_objects[object_index].name.empty()) {
                object_indices_by_name.emplace(ecs_objects[object_index].name, object_index);
            }
        }
        std::vector<std::uint8_t> transform_state(ecs_objects.size(), 0);
        const auto project_transform = [&](auto &&self, size_t object_index) -> void {
            if (transform_state[object_index] == 2) return;
            if (transform_state[object_index] == 1) {
                throw std::logic_error("validated scene hierarchy became cyclic during projection");
            }
            transform_state[object_index] = 1;
            const auto &object = ecs_objects[object_index];
            const TransformComponent *parent_world = nullptr;
            if (!object.parent.empty()) {
                const auto parent_index = object_indices_by_name.at(object.parent);
                self(self, parent_index);
                parent_world = ecs.tryComponent<TransformComponent>(object_ids[parent_index]);
            }
            auto *world = ecs.tryComponent<TransformComponent>(object_ids[object_index]);
            auto *local = ecs.tryComponent<LocalTransformComponent>(object_ids[object_index]);
            TransformCodecTarget target{.world = world, .local = local, .parent_world = parent_world};
            const auto &prepared = transformLoad(object);
            prepared.codec->applyRuntime(prepared.decoded, &target);
            transform_state[object_index] = 2;
        };
        for (size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
            if (ecs_objects[object_index].hierarchy_participant) {
                project_transform(project_transform, object_index);
            }
        }

        for (size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
            const auto &object = ecs_objects[object_index];
            const auto object_id = object_ids[object_index];
            const bool has_transform = object_id != invalidGameObjectId &&
                                       ecs.tryComponent<TransformComponent>(object_id) != nullptr;
#if PELICAN_WITH_PHYSICS
            for (const auto &collider : object.colliders) {
                const auto identity_name = runtimeObjectIdentityName(
                    scene_id, object.authoring_object_id, object.name);
                if (has_transform) {
                    phys_world.bindCollider(identity_name, collider, object_id);
                } else {
                    phys_world.bindCollider(identity_name, collider,
                                            identityPhysWorldTransform());
                }
            }
#endif
        }
    } else {
        for (std::size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
            const auto &object = ecs_objects[object_index];
            if (!object.components_id.empty() || !object.behaviors.empty()) {
                auto &object_id = object_ids[object_index];
                object_id = GameObjects::createWithComponents(object.components_id, [&](std::span<void *> ptrs) {
                    for (size_t i = 0; i < object.components.size(); ++i) {
                        applyComponentLoad(component_info_manager, ptrs[i], object.components[i]);
                    }
                });
                bindRuntimeObject(object.authoring_object_id, object_id);
            }
            const auto object_id = object_ids[object_index];
            const bool has_transform =
                object_id != invalidGameObjectId &&
                std::find(object.components_id.begin(), object.components_id.end(), transform_id) !=
                    object.components_id.end();
            if (!object.name.empty() && has_transform) {
                bindObjectTransform(object.name, object_id);
            }
#if PELICAN_WITH_PHYSICS
            for (const auto &collider : object.colliders) {
                const auto identity_name = runtimeObjectIdentityName(
                    scene_id, object.authoring_object_id, object.name);
                if (has_transform) {
                    phys_world.bindCollider(identity_name, collider, object_id);
                } else {
                    phys_world.bindCollider(identity_name, collider,
                                            identityPhysWorldTransform());
                }
            }
#endif
        }
    }

    std::vector<BoundSceneBehaviorAttachment> bound_behaviors;
    for (std::size_t object_index = 0; object_index < ecs_objects.size(); ++object_index) {
        auto &object = ecs_objects[object_index];
        for (auto &behavior : object.behaviors) {
            bound_behaviors.push_back(BoundSceneBehaviorAttachment{
                .prepared = std::move(behavior),
                .entity = object_ids[object_index],
            });
        }
    }
    if (!bound_behaviors.empty()) {
        try {
            auto &arena = GET_MODULE(BehaviorAttachmentArena);
            arena.publishSceneAttachments(std::move(bound_behaviors));
            arena.activatePublished();
        } catch (...) {
            clearRuntimeScene();
            throw;
        }
    }
    current_scene_id = std::move(scene_id);
    runtime_only_changes = false;
    ++runtime_scene_epoch;
    internal::getEventRegisterer().emit(SceneLoaded{current_scene_id});
}

void SceneLoader::requestLoad(SceneId scene_id) {
    auto &config = GET_MODULE(ProjectBasicConfig);
    if (config.resolvedScene().findScene(scene_id) == nullptr) {
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
    // Behavior pre-destroy runs while entity/component/parent bindings, physics,
    // renderer instances, and named transform bindings are still resolvable.
    internal::preDestroyAllBehaviorObjects();
    object_bindings.clear();
    runtime_object_bindings.clear();
    if (auto *seq_player = FastModuleContainer::tryGet<SeqPlayer>()) {
        seq_player->releaseInstancesForSceneLoad();
    }
    if (auto *vat_player = FastModuleContainer::tryGet<VatPlayer>()) {
        vat_player->releaseInstanceForSceneLoad();
    }
#if PELICAN_WITH_PHYSICS
    GET_MODULE(PhysWorld).clear();
#endif
    GameObjects::removeAll();
    GET_MODULE(PolygonInstanceContainer).clear();
    releaseTransientModels(true);
}

void SceneLoader::releaseTransientModels(bool deferred) noexcept {
    for (auto &model : transient_models) {
        releaseModelGpuResources(model, deferred);
    }
    transient_models.clear();
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

void SceneLoader::bindRuntimeObject(AuthoringObjectId authoring_object_id,
                                    GameObjectId object_id) {
    if (authoring_object_id.value == 0 || object_id == invalidGameObjectId) {
        throw std::logic_error("runtime object binding is incomplete");
    }
    const auto duplicate = std::find_if(
        runtime_object_bindings.begin(), runtime_object_bindings.end(),
        [&](const auto &binding) {
            return binding.authoring_object_id == authoring_object_id;
        });
    if (duplicate != runtime_object_bindings.end()) {
        throw std::logic_error("duplicate authoring object runtime binding");
    }
    runtime_object_bindings.push_back(
        SceneRuntimeObjectBinding{authoring_object_id, object_id});
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
    runtime_only_changes = true;
}

std::filesystem::path SceneLoader::loadTransientGltf(std::string_view path_ref, const std::optional<std::string> &name) {
    using BindingNode = decltype(object_bindings)::node_type;
    std::optional<BindingNode> staged_binding;
    if (name && !name->empty()) {
        if (const auto existing = object_bindings.find(*name);
            existing != object_bindings.end()) {
            if (GET_MODULE(ECSCore)
                    .getTemplatePublicModule()
                    .tryComponent<TransformComponent>(existing->second.object_id) != nullptr) {
                throw std::runtime_error("duplicate object name for transform binding: " + *name);
            }
            object_bindings.erase(existing);
        }

        // Allocate the hash node and any bucket growth before parsing or GPU
        // staging. Publishing the prepared node after entity creation cannot
        // allocate and therefore cannot split the transaction.
        object_bindings.reserve(object_bindings.size() + 1);
        decltype(object_bindings) staging;
        staging.emplace(*name, ObjectBinding{.object_id = invalidGameObjectId});
        staged_binding.emplace(staging.extract(*name));
    }

    auto &component_info_manager = GET_MODULE(ComponentInfoManager);
    const std::array<ComponentId, 2> component_ids{
        component_info_manager.getComponentIdByName("transform"),
        component_info_manager.getComponentIdByName("simplemodelview"),
    };
    transient_models.reserve(transient_models.size() + 1);

    const auto resolved = GET_MODULE(PathResolver).resolveExistingFileReference(path_ref);
    const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved);
    const auto path = fragment != nullptr ? fragment->path : std::get<std::filesystem::path>(resolved);
    auto prepared = prepareGltfTemplate(path, fragment != nullptr
                                                  ? std::optional<AssetFragmentRef>{fragment->fragment}
                                                  : std::nullopt);
    auto &gltf_loader = GET_MODULE(GltfLoader);
    auto &instances = GET_MODULE(PolygonInstanceContainer);

    // inspect() is the WP110 side-effect-free candidate pass. It resolves and
    // validates fragments and lets the slot/draw inventory reject capacity
    // before any model-specific Vulkan resource is allocated.
    const auto preview = gltf_loader.inspect(prepared);
    instances.preflightModelInstance(preview);

    UnpublishedModelCandidate candidate{
        .model = gltf_loader.commit(std::move(prepared)),
    };
    auto staged_instance = instances.stageModelInstance(candidate.model);
    const auto model_instance_id = staged_instance.id();
    const auto initial_transform = identityObjectTransform();
    transient_models.push_back(std::move(candidate.model));
    candidate.owns_resources = false;

    GameObjectId object_id;
    try {
        object_id = GameObjects::createWithComponents(component_ids, [&](std::span<void *> ptrs) {
            assignTransform(*static_cast<TransformComponent *>(ptrs[0]), initial_transform);
            static_cast<SimpleModelViewComponent *>(ptrs[1])->model_instance_id = model_instance_id;
        });
    } catch (...) {
        releaseModelGpuResources(transient_models.back(), false);
        transient_models.pop_back();
        throw;
    }

    // No operation below allocates: this is the single publication point for
    // the entity's slot, draw commands, resources, and optional name.
    instances.publishModelInstance(std::move(staged_instance));
    if (staged_binding) {
        staged_binding->mapped().object_id = object_id;
        const auto inserted = object_bindings.insert(std::move(*staged_binding));
        assert(inserted.inserted);
    }
    runtime_only_changes = true;
    return path;
}

} // namespace Pelican
