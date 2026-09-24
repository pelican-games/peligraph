#define GLM_ENABLE_EXPERIMENTAL
#include "editorprojectionadapters.hpp"
#include "modelphysicsbinding.hpp"
#include "../renderer/proceduralgeometrycontainer.hpp"

#include "componentcodec.hpp"
#include "basicconfig.hpp"
#include "sceneload.hpp"
#include "scene.hpp"
#include "../../project/assetdataformat.hpp"
#include "../vkcore/core.hpp"
#include <limits>
#include "../asset/model.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../ecs/archetypemigration.hpp"
#include "../ecs/componentinfo.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../light/lightcontainer.hpp"
#include "../phys/physworld.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include <components/predefined.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/coretemplate.hpp>

#include <algorithm>
#include <any>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Pelican {

namespace {

using Json = nlohmann::json;

const ResolvedSceneView *findResolvedScene(
    const ResolvedScene &scene, std::string_view scene_id) {
    return scene.findScene(scene_id);
}

const ResolvedObject *findResolvedObject(const ResolvedSceneView *scene,
                                         AuthoringObjectId object_id) {
    if (scene == nullptr) return nullptr;
    const auto found = std::find_if(
        scene->objects.begin(), scene->objects.end(),
        [object_id](const auto &object) {
            return object.authoring_object_id == object_id;
        });
    return found == scene->objects.end() ? nullptr : &*found;
}

const ResolvedComponent *findResolvedComponent(const ResolvedObject *object,
                                               std::string_view component_name) {
    if (object == nullptr) return nullptr;
    for (const auto &component : object->components) {
        if (component.name == component_name) return &component;
    }
    return nullptr;
}

void appendModelPhysicsBinding(
    std::vector<PhysWorld::Binding> &next, const ResolvedObject &object,
    const std::string &identity_name,
    std::variant<GameObjectId, PhysWorldTransform> transform,
    const std::unordered_map<std::string, const PhysWorld::Binding *> &old_by_name) {
    const auto *component = findResolvedComponent(&object, "simplemodelview");
    if (component == nullptr) return;
    const auto &data = std::any_cast<const SimpleModelViewCodecData &>(
        component->requireRuntimeValue());
    if (data.model.empty()) return;
    const auto &model = GET_MODULE(ModelAssetContainer).getModelTemplateByName(data.model);
    auto binding = makeModelPhysicsBinding(model, identity_name, std::move(transform));
    if (!binding) return;
    if (const auto old = old_by_name.find(binding->identity.name); old != old_by_name.end())
        binding->identity.collider_id = old->second->identity.collider_id;
    next.push_back(std::move(*binding));
}

void appendRuntimePhysicsBindings(std::vector<PhysWorld::Binding> &next,
    const PhysWorld::PreparedState &previous, ECSCoreTemplatePublic &ecs) {
    for (const auto &binding : previous.bindings) {
        if (!binding.runtime_only) continue;
        const auto *entity = std::get_if<GameObjectId>(&binding.transform_source);
        if (entity && ecs.tryComponent<TransformComponent>(*entity) == nullptr) continue;
        next.push_back(binding);
    }
}

Json proceduralComponents(const ResolvedSceneView *scene) {
    auto components = Json::array();
    if (!scene) return components;
    for (const auto &object : scene->objects) {
        for (const auto &component : object.components) {
            if (component.name == "pelican_procedural_geometry")
                components.push_back({{"object", object.authoring_object_id.value},
                    {"component", resolvedComponentWitness(component)},
                    {"value", component.effective_json}});
        }
    }
    return components;
}

std::unordered_map<std::uint64_t, EntityId> bindingMap(
    const std::vector<EditorProjectionRuntimeObjectBinding> &bindings) {
    std::unordered_map<std::uint64_t, EntityId> result;
    result.reserve(bindings.size());
    for (const auto &binding : bindings) {
        if (binding.authoring_object_id.value == 0 ||
            binding.entity == invalidEntityId ||
            !result.emplace(binding.authoring_object_id.value, binding.entity)
                 .second) {
            throw std::invalid_argument(
                "editor projection runtime object binding is invalid or duplicated");
        }
    }
    return result;
}

template <class Token> void publishToken(Token &token) noexcept {
    token.publish();
}

template <class Token> void rollbackToken(Token &token) noexcept {
    token.rollback();
}

template <class Token> void finishToken(Token &token) noexcept {
    token.finish();
}

std::vector<LightLoadEntry> prepareLightEntries(
    const ResolvedScene &document, std::string_view scene_id) {
    std::vector<LightLoadEntry> entries;
    const auto *scene = document.findScene(scene_id);
    if (scene == nullptr) return entries;
    for (const auto &object : scene->objects) {
        const auto object_name = object.name.value_or(std::string{});
        for (const auto &component : object.components) {
            if (component.name != "light") continue;
            entries.push_back(LightLoadEntry{
                object_name,
                std::any_cast<const LightCodecData &>(
                    component.requireRuntimeValue()),
            });
        }
    }
    return entries;
}

PhysWorldTransform resolvedPhysTransform(const ResolvedObject &object) {
    const auto *component = findResolvedComponent(&object, "transform");
    if (component == nullptr) return {};
    const auto &trs = std::any_cast<const TransformCodecData &>(
        component->requireRuntimeValue());
    return PhysWorldTransform{
        .pos = trs.pos,
        .rotation = trs.rotation,
        .scale = trs.scale,
    };
}

} // namespace

struct SceneObjectProjectionAdapter::Impl {
    struct SpawnLoad {
        std::string name;
        ComponentId id{};
        Json authored;
        const ComponentCodec *codec = nullptr;
        ComponentCodecValue decoded;
        bool injected_local = false;
    };

    ECSCoreTemplatePublic &ecs;
    ComponentInfoManager &components;
    ModelAssetContainer *models = nullptr;
    PolygonInstanceContainer *renderer = nullptr;
    Camera *camera = nullptr;
    LightContainer *lights = nullptr;
    PhysWorld *physics = nullptr;
    bool require_connected_participants = false;
    std::string scene_id;
    AuthoringObjectId target_object{};
    AuthoringObjectId prepared_target{};
    std::string adapter_name;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings;
    std::vector<EditorProjectionRuntimeObjectBinding> next_bindings;
    SceneLoader *scene_loader = nullptr;
    std::vector<SceneRuntimeObjectBinding> loader_bindings;
    // GP-2: a runtime spawn or destroy, which has no authoring object id and
    // no place in the runtime object bindings.
    bool transient = false;

    std::optional<ECSEntityMutationToken> entity;
    std::optional<StagedModelInstance> staged_model;
    std::optional<ModelInstanceId> staged_model_id;
    bool staged_model_published = false;

    std::optional<Camera::PreparedSceneState> old_camera;
    std::optional<Camera::PreparedSceneState> next_camera;
    std::optional<LightContainer::PreparedLoad> old_lights;
    std::optional<LightContainer::PreparedLoad> next_lights;
    std::optional<PhysWorld::PreparedState> old_physics;
    std::optional<PhysWorld::PreparedState> next_physics;
    bool camera_published = false;
    bool lights_published = false;
    bool physics_published = false;
    bool bindings_published = false;

    EntityId requireBoundEntity() const {
        const auto found = std::find_if(
            bindings.begin(), bindings.end(), [&](const auto &binding) {
                return binding.authoring_object_id == prepared_target;
            });
        if (found == bindings.end() || !ecs.isAlive(found->entity)) {
            throw std::runtime_error(
                "destroy target has no live runtime entity binding");
        }
        return found->entity;
    }

    void prepareSpawn(std::span<const ResolvedObject> scene_objects,
                      const ResolvedObject &object) {
        std::vector<SpawnLoad> loads;
        std::vector<ComponentId> ids;
        loads.reserve(object.components.size() + 1);
        ids.reserve(object.components.size() + 1);
        bool has_local = false;
        bool has_transform = false;
        for (const auto &component_view : object.components) {
            const auto &component_name = component_view.name;
            if (component_name == "light" || component_name == "collider" ||
                component_name == "behavior") {
                continue;
            }
            has_local = has_local || component_name == "localtransform";
            has_transform = has_transform || component_name == "transform";
            const auto id = components.getComponentIdByName(component_name);
            loads.push_back(SpawnLoad{
                .name = component_name,
                .id = id,
                .authored = component_view.effective_json,
                .codec = component_view.runtime_codec,
                .decoded = component_view.runtime_codec != nullptr
                               ? component_view.requireRuntimeValue()
                               : ComponentCodecValue{},
            });
            ids.push_back(id);
        }

        const bool has_child = object.name && std::any_of(
            scene_objects.begin(), scene_objects.end(),
            [&](const auto &candidate) {
                return candidate.parent && *candidate.parent == *object.name;
            });
        const bool hierarchy = object.parent.has_value() || has_child;
        if (hierarchy && !has_transform) {
            throw std::runtime_error(
                "hierarchy object spawn requires a transform component");
        }
        // GP-1: a spawned object with a transform carries a localtransform,
        // as a loaded one does, whether or not it has a parent or a child.
        // It is runtime state only and never enters the authored object.
        if (has_transform && !has_local) {
            const auto id = components.getComponentIdByName("localtransform");
            loads.push_back(SpawnLoad{
                .name = "localtransform",
                .id = id,
                .injected_local = true,
            });
            ids.push_back(id);
        }

        std::optional<EntityId> parent_entity;
        const TransformComponent *parent_world = nullptr;
        if (object.parent) {
            const auto parent = std::find_if(
                scene_objects.begin(), scene_objects.end(),
                [&](const auto &candidate) {
                    return candidate.name && *candidate.name == *object.parent;
                });
            if (parent == scene_objects.end()) {
                throw std::runtime_error("spawn parent is absent");
            }
            const auto runtime = std::find_if(
                bindings.begin(), bindings.end(), [&](const auto &binding) {
                    return binding.authoring_object_id ==
                           parent->authoring_object_id;
                });
            if (runtime == bindings.end()) {
                throw std::runtime_error("spawn parent has no runtime binding");
            }
            parent_entity = runtime->entity;
            parent_world = ecs.tryComponent<TransformComponent>(*parent_entity);
            if (parent_world == nullptr) {
                throw std::runtime_error("spawn parent has no runtime transform");
            }
        }

        const auto model_load = std::find_if(
            loads.begin(), loads.end(), [](const auto &load) {
                return load.name == "simplemodelview";
            });
        if (model_load != loads.end()) {
            if (models == nullptr || renderer == nullptr) {
                throw std::runtime_error(
                    "editor_projection_participant_missing@1: renderer");
            } else {
                const auto &data =
                    std::any_cast<const SimpleModelViewCodecData &>(
                        model_load->decoded);
                auto &model = models->getModelTemplateByName(data.model);
                staged_model.emplace(renderer->stageModelInstance(model));
                renderer->setStagedAuthoringObject(*staged_model, prepared_target.value);
                staged_model_id = staged_model->id();

                glm::mat4 matrix{1.0F};
                const auto transform_load = std::find_if(
                    loads.begin(), loads.end(), [](const auto &load) {
                        return load.name == "transform";
                    });
                if (transform_load != loads.end()) {
                    TransformComponent world{};
                    LocalTransformComponent local{};
                    TransformCodecTarget target{
                        .world = &world,
                        .local = &local,
                        .parent_world = parent_world,
                    };
                    transform_load->codec->applyRuntime(
                        transform_load->decoded, &target);
                    matrix = glm::translate(glm::mat4{1.0F}, world.pos) *
                             glm::toMat4(world.rotation) *
                             glm::scale(glm::mat4{1.0F}, world.scale);
                }
                renderer->setStagedModelMatrix(*staged_model, matrix);
            }
        }

        entity.emplace(ECSEntityMutation::prepareCreate(
            ecs, ids, [&](std::span<void *> values) {
                TransformComponent *world = nullptr;
                LocalTransformComponent *local = nullptr;
                const SpawnLoad *transform_load = nullptr;
                for (std::size_t index = 0; index < loads.size(); ++index) {
                    const auto &load = loads[index];
                    if (load.name == "transform") {
                        world = static_cast<TransformComponent *>(values[index]);
                        transform_load = &load;
                        continue;
                    }
                    if (load.name == "localtransform") {
                        local = static_cast<LocalTransformComponent *>(values[index]);
                        if (!load.injected_local) {
                            components.loadByJson(values[index], load.authored);
                        }
                        continue;
                    }
                    if (load.name == "simplemodelview") {
                        load.codec->applyRuntime(load.decoded, values[index]);
                        auto &model = *static_cast<SimpleModelViewComponent *>(
                            values[index]);
                        model.model_instance_id = staged_model_id;
                        model.authoring_object_id = prepared_target.value;
                        model.dirty = 0;
                        continue;
                    }
                    if (load.codec == nullptr ||
                        load.codec->runtime_kind !=
                            ComponentCodecRuntimeKind::Ecs) {
                        components.loadByJson(values[index], load.authored);
                    } else {
                        load.codec->applyRuntime(load.decoded, values[index]);
                    }
                }
                if (transform_load != nullptr) {
                    // The local is written with the world: the next transform
                    // update recomputes the world from it, and a local left at
                    // its zero value would move the object there.
                    TransformCodecTarget target{
                        .world = world,
                        .local = local,
                        .parent_world = parent_world,
                    };
                    transform_load->codec->applyRuntime(
                        transform_load->decoded, &target);
                }
                if (local != nullptr && parent_entity) {
                    local->parent = *parent_entity;
                }
            }));
        if (transient) return;
        next_bindings = bindings;
        next_bindings.push_back(EditorProjectionRuntimeObjectBinding{
            .authoring_object_id = prepared_target,
            .entity = entity->entity(),
        });
        (void)bindingMap(next_bindings);
    }

    // GP-2: physics of a runtime spawn or destroy. The current bindings stay
    // as they are -- this is not a scene's re-preparation -- apart from the
    // runtime_only ones whose entity is gone, which an editor edit drops the
    // same way (appendRuntimePhysicsBindings).
    std::vector<PhysWorld::Binding> keptPhysicsBindings() const {
        std::vector<PhysWorld::Binding> next;
        next.reserve(old_physics->bindings.size() + 2U);
        for (const auto &binding : old_physics->bindings) {
            if (!binding.runtime_only) next.push_back(binding);
        }
        appendRuntimePhysicsBindings(next, *old_physics, ecs);
        return next;
    }

    void prepareTransientSpawnPhysics(const ResolvedObject &object,
                                      const std::string &identity_name) {
        const auto created = entity->entity();
        std::optional<PhysWorld::Binding> model_binding;
        if (const auto *model_component =
                findResolvedComponent(&object, "simplemodelview")) {
            const auto &data = std::any_cast<const SimpleModelViewCodecData &>(
                model_component->requireRuntimeValue());
            if (!data.model.empty()) {
                model_binding = makeModelPhysicsBinding(
                    models->getModelTemplateByName(data.model), identity_name,
                    created);
            }
        }
        const auto *collider_component = findResolvedComponent(&object, "collider");
        if (collider_component == nullptr && !model_binding) return;
        if (physics == nullptr) {
            throw std::runtime_error(
                "editor_projection_participant_missing@1: collider");
        }
        old_physics = physics->snapshotPrepared();
        auto next = keptPhysicsBindings();
        if (model_binding) {
            model_binding->runtime_only = true;
            next.push_back(std::move(*model_binding));
        }
        if (collider_component != nullptr) {
            ColliderComponent collider;
            collider_component->runtime_codec->applyRuntime(
                collider_component->requireRuntimeValue(), &collider);
            next.push_back(PhysWorld::Binding{
                .identity = {.name = identity_name},
                .metadata = phys::ColliderQueryMetadata{
                    .layer = collider.layer,
                    .mask = collider.mask,
                    .trigger = collider.trigger,
                    .one_way = collider.one_way,
                },
                .collider = collider,
                .transform_source = created,
                .runtime_only = true,
            });
        }
        const std::array unpublished{created};
        next_physics = physics->prepareBindings(std::move(next), unpublished);
    }

    void prepareTransientDestroyPhysics(EntityId target) {
        if (physics == nullptr) return;
        old_physics = physics->snapshotPrepared();
        auto next = keptPhysicsBindings();
        const auto removed = std::erase_if(next, [&](const auto &binding) {
            const auto *source =
                std::get_if<GameObjectId>(&binding.transform_source);
            return binding.runtime_only && source != nullptr &&
                   *source == target;
        });
        if (removed == 0) {
            old_physics.reset();
            return;
        }
        next_physics = physics->prepareBindings(std::move(next));
    }

    void prepareDestroy() {
        const auto runtime = requireBoundEntity();
        entity.emplace(ECSEntityMutation::prepareDestroy(ecs, runtime));
        next_bindings = bindings;
        std::erase_if(next_bindings, [&](const auto &binding) {
            return binding.authoring_object_id == prepared_target;
        });
    }

    void prepareSpecials(const EditorProjectionPrepareContext &context,
                         const ResolvedSceneView *base_scene,
                         const ResolvedSceneView *next_scene) {
        const auto sceneContains = [](const ResolvedSceneView *scene,
                                      std::string_view component_name) {
            return scene != nullptr &&
                   std::any_of(scene->objects.begin(), scene->objects.end(),
                               [&](const auto &object) {
                                   return findResolvedComponent(
                                              &object, component_name) !=
                                          nullptr;
                               });
        };
        const auto requireConnected = [&](bool connected,
                                          std::string_view component_name) {
            if (!connected &&
                (sceneContains(base_scene, component_name) ||
                 sceneContains(next_scene, component_name))) {
                throw std::runtime_error(
                    "editor_projection_participant_missing@1: " +
                    (component_name == "simplemodelview"
                         ? std::string{"renderer"}
                         : std::string{component_name}));
            }
        };
        if (require_connected_participants) {
            requireConnected(models != nullptr && renderer != nullptr,
                             "simplemodelview");
            requireConnected(camera != nullptr, "camera");
            requireConnected(lights != nullptr, "light");
            requireConnected(physics != nullptr, "collider");
        }

        if (camera != nullptr) {
            old_camera = camera->snapshotPrepared();
            next_camera = camera->prepareSceneCameras(
                scene_id, context.next_resolved);
        }
        if (lights != nullptr) {
            old_lights = lights->snapshotPrepared();
            next_lights = LightContainer::prepareLoad(
                prepareLightEntries(context.next_resolved, scene_id));
        }
        if (physics == nullptr) return;

        old_physics = physics->snapshotPrepared();
        std::unordered_map<std::string, const PhysWorld::Binding *> old_by_name;
        for (const auto &binding : old_physics->bindings) {
            old_by_name.emplace(binding.identity.name, &binding);
        }
        const auto entities = bindingMap(next_bindings);
        std::vector<PhysWorld::Binding> next;
        if (next_scene != nullptr) {
            next.reserve(next_scene->objects.size());
            for (const auto &object : next_scene->objects) {
                const auto *collider_component =
                    findResolvedComponent(&object, "collider");
                auto mesh_transform = std::variant<GameObjectId, PhysWorldTransform>{resolvedPhysTransform(object)};
                if (const auto runtime = entities.find(object.authoring_object_id.value);
                    runtime != entities.end() && findResolvedComponent(&object, "transform") != nullptr)
                    mesh_transform = runtime->second;
                appendModelPhysicsBinding(next, object, runtimeObjectIdentityName(
                    scene_id, object.authoring_object_id, object.name.value_or(std::string{})),
                    std::move(mesh_transform), old_by_name);
                if (collider_component == nullptr) continue;
                const auto identity_name = runtimeObjectIdentityName(
                    scene_id, object.authoring_object_id,
                    object.name.value_or(std::string{}));
                ColliderComponent collider;
                collider_component->runtime_codec->applyRuntime(
                    collider_component->requireRuntimeValue(), &collider);
                PhysWorld::Binding binding{
                    .identity = {.name = identity_name},
                    .collider = collider,
                    .transform_source = resolvedPhysTransform(object),
                };
                if (const auto old = old_by_name.find(identity_name);
                    old != old_by_name.end()) {
                    binding.identity = old->second->identity;
                    binding.metadata = old->second->metadata;
                }
                const auto runtime =
                    entities.find(object.authoring_object_id.value);
                if (runtime != entities.end() &&
                    findResolvedComponent(&object, "transform") != nullptr) {
                    binding.transform_source = runtime->second;
                }
                next.push_back(std::move(binding));
            }
        }
        appendRuntimePhysicsBindings(next, *old_physics, ecs);
        const std::array unpublished{entity->kind() ==
                                             ECSEntityMutationKind::create
                                         ? entity->entity()
                                         : invalidEntityId};
        next_physics = physics->prepareBindings(
            std::move(next),
            entity->kind() == ECSEntityMutationKind::create
                ? std::span<const GameObjectId>{unpublished}
                : std::span<const GameObjectId>{});
    }
};

SceneObjectProjectionAdapter::SceneObjectProjectionAdapter(
    ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
    ModelAssetContainer &models, PolygonInstanceContainer &renderer,
    Camera &camera, LightContainer &lights, PhysWorld &physics,
    std::string scene_id, AuthoringObjectId target_object,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings)
    : impl_(std::make_unique<Impl>(Impl{
          .ecs = ecs,
          .components = components,
          .models = &models,
          .renderer = &renderer,
          .camera = &camera,
          .lights = &lights,
          .physics = &physics,
          .scene_id = std::move(scene_id),
          .target_object = target_object,
          .bindings = std::move(bindings),
      })) {
    (void)bindingMap(impl_->bindings);
    impl_->adapter_name = "object_lifecycle." + impl_->scene_id + "." +
                          (impl_->target_object.value == 0
                               ? std::string{"auto"}
                               : std::to_string(impl_->target_object.value));
}

SceneObjectProjectionAdapter::SceneObjectProjectionAdapter(
    ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
    ModelAssetContainer *models, PolygonInstanceContainer *renderer,
    Camera *camera, LightContainer *lights, PhysWorld *physics,
    std::string scene_id, AuthoringObjectId target_object,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings,
    SceneObjectProjectionParticipants participants)
    : impl_(std::make_unique<Impl>(Impl{
          .ecs = ecs,
          .components = components,
          .models = participants.renderer ? models : nullptr,
          .renderer = participants.renderer ? renderer : nullptr,
          .camera = participants.camera ? camera : nullptr,
          .lights = participants.light ? lights : nullptr,
          .physics = participants.collider ? physics : nullptr,
          .require_connected_participants = true,
          .scene_id = std::move(scene_id),
          .target_object = target_object,
          .bindings = std::move(bindings),
      })) {
    (void)bindingMap(impl_->bindings);
    impl_->adapter_name = "object_lifecycle." + impl_->scene_id + "." +
                          (impl_->target_object.value == 0
                               ? std::string{"auto"}
                               : std::to_string(impl_->target_object.value));
}

SceneObjectProjectionAdapter::SceneObjectProjectionAdapter(
    ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
    ModelAssetContainer &models, PolygonInstanceContainer &renderer,
    Camera &camera, LightContainer &lights, PhysWorld &physics,
    std::string scene_id, AuthoringObjectId target_object,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings,
    SceneObjectProjectionParticipants participants)
    : impl_(std::make_unique<Impl>(Impl{
          .ecs = ecs,
          .components = components,
          .models = participants.renderer ? &models : nullptr,
          .renderer = participants.renderer ? &renderer : nullptr,
          .camera = participants.camera ? &camera : nullptr,
          .lights = participants.light ? &lights : nullptr,
          .physics = participants.collider ? &physics : nullptr,
          .require_connected_participants = false,
          .scene_id = std::move(scene_id),
          .target_object = target_object,
          .bindings = std::move(bindings),
      })) {
    (void)bindingMap(impl_->bindings);
    impl_->adapter_name = "object_lifecycle." + impl_->scene_id + "." +
                          (impl_->target_object.value == 0
                               ? std::string{"auto"}
                               : std::to_string(impl_->target_object.value));
}

SceneObjectProjectionAdapter::~SceneObjectProjectionAdapter() = default;
EditorProjectionAdapterKind
SceneObjectProjectionAdapter::kind() const noexcept {
    return EditorProjectionAdapterKind::EcsArchetype;
}
std::string_view SceneObjectProjectionAdapter::name() const noexcept {
    return impl_->adapter_name;
}
EditorProjectionPublicationMode
SceneObjectProjectionAdapter::publicationMode() const noexcept {
    return EditorProjectionPublicationMode::InverseToken;
}
std::span<const EditorProjectionRuntimeObjectBinding>
SceneObjectProjectionAdapter::runtimeBindings() const noexcept {
    return impl_->bindings;
}

SceneObjectProjectionAdapter::SceneObjectProjectionAdapter(
    ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
    std::string scene_id, AuthoringObjectId target_object,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings)
    : impl_(std::make_unique<Impl>(Impl{
          .ecs = ecs,
          .components = components,
          .require_connected_participants = true,
          .scene_id = std::move(scene_id),
          .target_object = target_object,
          .bindings = std::move(bindings),
      })) {
    (void)bindingMap(impl_->bindings);
    impl_->adapter_name = "object_lifecycle.headless." + impl_->scene_id + "." +
                          (impl_->target_object.value == 0
                               ? std::string{"auto"}
                               : std::to_string(impl_->target_object.value));
}
void SceneObjectProjectionAdapter::connectRuntimeBindings(SceneLoader &loader) noexcept {
    impl_->scene_loader = &loader;
}

std::span<const EditorProjectionRuntimeObjectBinding>
SceneObjectProjectionAdapter::preparedRuntimeBindings() const noexcept {
    // ECS archetype inverse tokens are published during the prepare walk to
    // release the core mutation guard.  Preserve "prepared" semantics across
    // that early swap so later adapters (notably behavior reconciliation) see
    // the post-lifecycle binding set in both phases.
    return impl_->bindings_published
               ? std::span<const EditorProjectionRuntimeObjectBinding>{
                     impl_->bindings}
               : std::span<const EditorProjectionRuntimeObjectBinding>{
                     impl_->next_bindings};
}
AuthoringObjectId
SceneObjectProjectionAdapter::preparedTargetObject() const noexcept {
    return impl_->prepared_target;
}

void SceneObjectProjectionAdapter::prepareTransientSpawn(
    const ResolvedObject &object, const std::string &identity_name) {
    if (impl_->entity) {
        throw std::logic_error("scene object projection is already prepared");
    }
    if (object.parent) {
        throw std::logic_error("a runtime spawn has no parent");
    }
    for (const auto &component : object.components) {
        if (component.name == "light" || component.name == "camera") {
            throw std::logic_error(
                "a runtime spawn cannot carry a light or a camera");
        }
    }
    if (findResolvedComponent(&object, "simplemodelview") != nullptr &&
        (impl_->models == nullptr || impl_->renderer == nullptr)) {
        throw std::runtime_error(
            "editor_projection_participant_missing@1: renderer");
    }
    impl_->transient = true;
    impl_->prepared_target = AuthoringObjectId{};
    impl_->next_bindings.clear();
    impl_->prepareSpawn({}, object);
    impl_->prepareTransientSpawnPhysics(object, identity_name);
}

void SceneObjectProjectionAdapter::prepareTransientDestroy(EntityId target) {
    if (impl_->entity) {
        throw std::logic_error("scene object projection is already prepared");
    }
    if (!impl_->ecs.isAlive(target)) {
        throw std::runtime_error(
            "destroy target has no live runtime entity binding");
    }
    impl_->transient = true;
    impl_->prepared_target = AuthoringObjectId{};
    impl_->next_bindings.clear();
    impl_->entity.emplace(ECSEntityMutation::prepareDestroy(impl_->ecs, target));
    impl_->prepareTransientDestroyPhysics(target);
}

EntityId SceneObjectProjectionAdapter::preparedEntity() const noexcept {
    return impl_->entity ? impl_->entity->entity() : invalidEntityId;
}

void SceneObjectProjectionAdapter::prepare(
    const EditorProjectionPrepareContext &context) {
    impl_->next_bindings.clear();
    const auto *base_scene =
        findResolvedScene(context.base_resolved, impl_->scene_id);
    const auto *next_scene =
        findResolvedScene(context.next_resolved, impl_->scene_id);
    impl_->prepared_target = impl_->target_object;
    if (impl_->prepared_target.value == 0) {
        std::vector<AuthoringObjectId> difference;
        if (base_scene != nullptr) {
            for (const auto &object : base_scene->objects) {
                if (findResolvedObject(next_scene, object.authoring_object_id) == nullptr) {
                    difference.push_back(object.authoring_object_id);
                }
            }
        }
        if (next_scene != nullptr) {
            for (const auto &object : next_scene->objects) {
                if (findResolvedObject(base_scene, object.authoring_object_id) == nullptr) {
                    difference.push_back(object.authoring_object_id);
                }
            }
        }
        if (difference.size() != 1) {
            throw std::runtime_error(
                "automatic scene object projection requires exactly one spawn or destroy");
        }
        impl_->prepared_target = difference.front();
    }
    const auto *base_object =
        findResolvedObject(base_scene, impl_->prepared_target);
    const auto *next_object =
        findResolvedObject(next_scene, impl_->prepared_target);
    if ((base_object == nullptr) == (next_object == nullptr)) {
        throw std::runtime_error(
            "scene object projection target is not a spawn or destroy");
    }
    const auto *lifecycle_object =
        next_object != nullptr ? next_object : base_object;
    if (findResolvedComponent(lifecycle_object, "simplemodelview") != nullptr &&
        (impl_->models == nullptr || impl_->renderer == nullptr)) {
        throw std::runtime_error(
            "editor_projection_participant_missing@1: renderer");
    }
    if (next_object != nullptr) {
        if (next_scene == nullptr) {
            throw std::logic_error("spawn scene query disappeared");
        }
        impl_->prepareSpawn(next_scene->objects, *next_object);
    } else {
        impl_->prepareDestroy();
    }
    impl_->prepareSpecials(context, base_scene, next_scene);
    if (impl_->scene_loader) {
        // Earlier lifecycle tokens may already have published during prepare.
        // Apply this one object's change to the current canonical bindings.
        const auto current = impl_->scene_loader->runtimeObjectBindings();
        impl_->loader_bindings.assign(current.begin(), current.end());
        std::erase_if(impl_->loader_bindings, [&](const auto &binding) {
            return binding.authoring_object_id == impl_->prepared_target;
        });
        const auto replacement = std::find_if(impl_->next_bindings.begin(), impl_->next_bindings.end(),
            [&](const auto &binding) { return binding.authoring_object_id == impl_->prepared_target; });
        if (replacement != impl_->next_bindings.end())
            impl_->loader_bindings.push_back({replacement->authoring_object_id, replacement->entity});
    }
}

void SceneObjectProjectionAdapter::publish() noexcept {
    if (impl_->staged_model) {
        impl_->renderer->publishModelInstance(std::move(*impl_->staged_model));
        impl_->staged_model.reset();
        impl_->staged_model_published = true;
    }
    impl_->entity->publish();
    if (impl_->next_camera) {
        impl_->camera->publishPrepared(std::move(*impl_->next_camera));
        impl_->camera_published = true;
    }
    if (impl_->next_lights) {
        impl_->lights->publishPrepared(std::move(*impl_->next_lights));
        impl_->lights_published = true;
    }
    if (impl_->next_physics) {
        impl_->physics->publishPrepared(std::move(*impl_->next_physics));
        impl_->physics_published = true;
    }
    impl_->bindings.swap(impl_->next_bindings);
    if (impl_->scene_loader) impl_->scene_loader->publishRuntimeObjectBindings(impl_->loader_bindings);
    impl_->bindings_published = true;
}

void SceneObjectProjectionAdapter::rollback() noexcept {
    if (impl_->bindings_published) {
        if (impl_->scene_loader) impl_->scene_loader->publishRuntimeObjectBindings(impl_->loader_bindings);
        impl_->bindings.swap(impl_->next_bindings);
    }
    if (impl_->physics_published) {
        impl_->physics->publishPrepared(std::move(*impl_->old_physics));
    }
    if (impl_->lights_published) {
        impl_->lights->publishPrepared(std::move(*impl_->old_lights));
    }
    if (impl_->camera_published) {
        impl_->camera->publishPrepared(std::move(*impl_->old_camera));
    }
    if (impl_->entity) impl_->entity->rollback();
    impl_->entity.reset();
    impl_->staged_model.reset();
    impl_->staged_model_id.reset();
    impl_->old_camera.reset();
    impl_->next_camera.reset();
    impl_->old_lights.reset();
    impl_->next_lights.reset();
    impl_->old_physics.reset();
    impl_->next_physics.reset();
    impl_->next_bindings.clear();
    impl_->staged_model_published = false;
    impl_->camera_published = false;
    impl_->lights_published = false;
    impl_->physics_published = false;
    impl_->bindings_published = false;
}

void SceneObjectProjectionAdapter::finish() noexcept {
    if (impl_->entity) impl_->entity->finish();
    impl_->entity.reset();
    impl_->staged_model.reset();
    impl_->staged_model_id.reset();
    impl_->old_camera.reset();
    impl_->next_camera.reset();
    impl_->old_lights.reset();
    impl_->next_lights.reset();
    impl_->old_physics.reset();
    impl_->next_physics.reset();
    impl_->next_bindings.clear();
    impl_->staged_model_published = false;
    impl_->camera_published = false;
    impl_->lights_published = false;
    impl_->physics_published = false;
    impl_->bindings_published = false;
}

struct BehaviorSceneProjectionAdapter::Impl {
    BehaviorAttachmentArena &arena;
    std::string scene_id;
    std::string adapter_name;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings;
    std::vector<SceneObjectProjectionAdapter *> lifecycle_adapters;
    OriginTransitionMap origin_transition;
    BehaviorRegistryAvailability availability =
        BehaviorRegistryAvailability::active;
    std::unique_ptr<PreparedBehaviorSceneReconciliation> prepared;
};

BehaviorSceneProjectionAdapter::BehaviorSceneProjectionAdapter(
    BehaviorAttachmentArena &arena, std::string scene_id,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings,
    std::vector<SceneObjectProjectionAdapter *> lifecycle_adapters)
    : BehaviorSceneProjectionAdapter(
          arena, std::move(scene_id), std::move(bindings),
          std::move(lifecycle_adapters), {},
          BehaviorRegistryAvailability::active) {}

BehaviorSceneProjectionAdapter::BehaviorSceneProjectionAdapter(
    BehaviorAttachmentArena &arena, std::string scene_id,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings,
    std::vector<SceneObjectProjectionAdapter *> lifecycle_adapters,
    OriginTransitionMap origin_transition,
    BehaviorRegistryAvailability availability)
    : impl_{std::make_unique<Impl>(Impl{
          .arena = arena,
          .scene_id = std::move(scene_id),
          .adapter_name = {},
          .bindings = std::move(bindings),
          .lifecycle_adapters = std::move(lifecycle_adapters),
          .origin_transition = std::move(origin_transition),
          .availability = availability,
      })} {
    (void)bindingMap(impl_->bindings);
    impl_->adapter_name = "behavior.scene." + impl_->scene_id;
}

BehaviorSceneProjectionAdapter::~BehaviorSceneProjectionAdapter() = default;
EditorProjectionAdapterKind
BehaviorSceneProjectionAdapter::kind() const noexcept {
    return EditorProjectionAdapterKind::BehaviorAttachment;
}
std::string_view BehaviorSceneProjectionAdapter::name() const noexcept {
    return impl_->adapter_name;
}
EditorProjectionPublicationMode
BehaviorSceneProjectionAdapter::publicationMode() const noexcept {
    return EditorProjectionPublicationMode::InverseToken;
}

void BehaviorSceneProjectionAdapter::prepare(
    const EditorProjectionPrepareContext &context) {
    auto next_bindings = impl_->bindings;
    for (const auto *lifecycle : impl_->lifecycle_adapters) {
        if (lifecycle == nullptr) {
            throw std::logic_error(
                "behavior scene projection has a null lifecycle adapter");
        }
        const auto target = lifecycle->preparedTargetObject();
        const auto staged = lifecycle->preparedRuntimeBindings();
        const auto replacement = std::find_if(
            staged.begin(), staged.end(), [&](const auto &binding) {
                return binding.authoring_object_id == target;
            });
        const auto current = std::find_if(
            next_bindings.begin(), next_bindings.end(),
            [&](const auto &binding) {
                return binding.authoring_object_id == target;
            });
        if (replacement == staged.end()) {
            if (current != next_bindings.end()) next_bindings.erase(current);
        } else if (current == next_bindings.end()) {
            next_bindings.push_back(*replacement);
        } else {
            *current = *replacement;
        }
    }

    const auto *scene = findResolvedScene(context.next_resolved,
                                          impl_->scene_id);
    std::vector<BoundSceneBehaviorAttachment> desired;
    if (scene != nullptr) {
        auto prepared = prepareResolvedSceneBehaviorAttachments(
            scene->objects, impl_->availability);
        desired.reserve(prepared.size());
        for (auto &attachment : prepared) {
            const auto object = std::find_if(
                scene->objects.begin(), scene->objects.end(),
                [&](const auto &candidate) {
                    return candidate.authoring_object_index ==
                           attachment.object_index;
                });
            if (object == scene->objects.end()) {
                throw std::logic_error(
                    "prepared behavior lost its resolved object");
            }
            const auto binding = std::find_if(
                next_bindings.begin(), next_bindings.end(),
                [&](const auto &candidate) {
                    return candidate.authoring_object_id ==
                           object->authoring_object_id;
                });
            if (binding == next_bindings.end()) {
                throw std::runtime_error(
                    "resolved behavior object has no staged runtime binding");
            }
            desired.push_back(BoundSceneBehaviorAttachment{
                .prepared = std::move(attachment),
                .entity = binding->entity,
            });
        }
    }
    impl_->prepared =
        impl_->arena.prepareSceneReconciliation(
            std::move(desired), impl_->origin_transition);
}

void BehaviorSceneProjectionAdapter::publish() noexcept {
    if (impl_->prepared) impl_->prepared->publish();
}
void BehaviorSceneProjectionAdapter::rollback() noexcept {
    if (impl_->prepared) impl_->prepared->rollback();
    impl_->prepared.reset();
}
void BehaviorSceneProjectionAdapter::finish() noexcept {
    if (impl_->prepared) impl_->prepared->finish();
    impl_->prepared.reset();
}

struct EcsCodecProjectionAdapter::Impl {
    struct Prepared {
        const ComponentCodec *codec = nullptr;
        ComponentEditorSwapToken token;
    };

    ECSCoreTemplatePublic &ecs;
    std::string scene_id;
    std::string adapter_name;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings;
    std::vector<ComponentCodec> codecs;
    std::vector<Prepared> prepared;
};

EcsCodecProjectionAdapter::EcsCodecProjectionAdapter(
    ECSCoreTemplatePublic &ecs, std::string scene_id,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings,
    std::vector<ComponentCodec> codecs)
    : impl_(std::make_unique<Impl>(Impl{
          .ecs = ecs,
          .scene_id = std::move(scene_id),
          .adapter_name = {},
          .bindings = std::move(bindings),
          .codecs = std::move(codecs),
      })) {
    (void)bindingMap(impl_->bindings);
    impl_->adapter_name = "codec.ecs." + impl_->scene_id;
}

EcsCodecProjectionAdapter::~EcsCodecProjectionAdapter() = default;
EditorProjectionAdapterKind EcsCodecProjectionAdapter::kind() const noexcept {
    return EditorProjectionAdapterKind::EcsExistingValue;
}
std::string_view EcsCodecProjectionAdapter::name() const noexcept {
    return impl_->adapter_name;
}
EditorProjectionPublicationMode
EcsCodecProjectionAdapter::publicationMode() const noexcept {
    return EditorProjectionPublicationMode::StagedNoexcept;
}

void EcsCodecProjectionAdapter::prepare(
    const EditorProjectionPrepareContext &context) {
    impl_->prepared.clear();
    const auto *base_scene =
        findResolvedScene(context.base_resolved, impl_->scene_id);
    const auto *next_scene =
        findResolvedScene(context.next_resolved, impl_->scene_id);
    for (const auto &binding : impl_->bindings) {
        const auto *base_object =
            findResolvedObject(base_scene, binding.authoring_object_id);
        const auto *next_object =
            findResolvedObject(next_scene, binding.authoring_object_id);
        if (base_object == nullptr || next_object == nullptr) continue;

        for (const auto &codec : impl_->codecs) {
            if (codec.editor_policy != EditorProjectionPolicy::EcsCodec) {
                continue;
            }
            const auto *base_component =
                findResolvedComponent(base_object, codec.name);
            const auto *next_component =
                findResolvedComponent(next_object, codec.name);
            if (base_component == nullptr || next_component == nullptr ||
                base_component->effective_json ==
                    next_component->effective_json) {
                continue;
            }
            impl_->prepared.push_back(Impl::Prepared{
                .codec = &codec,
                .token = codec.prepareEditorSwap(
                    &impl_->ecs, binding.entity.index,
                    binding.entity.generation,
                    next_component->requireRuntimeValue()),
            });
        }
    }
}

void EcsCodecProjectionAdapter::publish() noexcept {
    for (auto &token : impl_->prepared) {
        token.codec->swapEditorToken(token.token,
                                     EditorCodecSwapPhase::Publish);
    }
}
void EcsCodecProjectionAdapter::rollback() noexcept {
    for (auto it = impl_->prepared.rbegin(); it != impl_->prepared.rend(); ++it) {
        it->codec->swapEditorToken(it->token,
                                   EditorCodecSwapPhase::Rollback);
    }
    impl_->prepared.clear();
}
void EcsCodecProjectionAdapter::finish() noexcept {
    for (auto &token : impl_->prepared) {
        token.codec->swapEditorToken(token.token,
                                     EditorCodecSwapPhase::Finish);
    }
    impl_->prepared.clear();
}

struct RendererModelProjectionAdapter::Impl {
    struct Prepared {
        ECSCoreTemplatePublic::PreparedComponentSwap<SimpleModelViewComponent>
            ecs_value;
        ModelInstanceId next_id{};
        std::optional<ModelInstanceId> old_id;
    };

    ECSCoreTemplatePublic &ecs;
    ModelAssetContainer &models;
    PolygonInstanceContainer &renderer;
    std::string scene_id;
    std::string adapter_name;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings;
    std::vector<Prepared> prepared;
    std::optional<PolygonInstanceContainer::PreparedSceneState> candidate;
    ProceduralGeometryContainer *procedural = nullptr;
    std::optional<ProceduralGeometryContainer::PreparedSceneState> procedural_candidate;
    SceneLoadPhaseTrace procedural_trace;
    bool published = false;
};

RendererModelProjectionAdapter::RendererModelProjectionAdapter(
    ECSCoreTemplatePublic &ecs, ModelAssetContainer &models,
    PolygonInstanceContainer &renderer, std::string scene_id,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings)
    : impl_(std::make_unique<Impl>(Impl{
          .ecs = ecs,
          .models = models,
          .renderer = renderer,
          .scene_id = std::move(scene_id),
          .adapter_name = {},
          .bindings = std::move(bindings),
      })) {
    (void)bindingMap(impl_->bindings);
    impl_->adapter_name = "codec.simplemodelview." + impl_->scene_id;
}

RendererModelProjectionAdapter::~RendererModelProjectionAdapter() = default;
EditorProjectionAdapterKind
RendererModelProjectionAdapter::kind() const noexcept {
    return EditorProjectionAdapterKind::RendererModel;
}
std::string_view RendererModelProjectionAdapter::name() const noexcept {
    return impl_->adapter_name;
}
EditorProjectionPublicationMode
RendererModelProjectionAdapter::publicationMode() const noexcept {
    return EditorProjectionPublicationMode::InverseToken;
}

void RendererModelProjectionAdapter::prepare(
    const EditorProjectionPrepareContext &context) {
    impl_->prepared.clear();
    impl_->candidate = impl_->renderer.snapshotPreparedScene();
    impl_->procedural = FastModuleContainer::tryGet<ProceduralGeometryContainer>();
    impl_->procedural_candidate.reset();
    impl_->published = false;
    const auto *base_scene =
        findResolvedScene(context.base_resolved, impl_->scene_id);
    const auto *next_scene =
        findResolvedScene(context.next_resolved, impl_->scene_id);
    for (const auto &binding : impl_->bindings) {
        const auto *base_object =
            findResolvedObject(base_scene, binding.authoring_object_id);
        const auto *next_object =
            findResolvedObject(next_scene, binding.authoring_object_id);
        const auto *base_component =
            findResolvedComponent(base_object, "simplemodelview");
        const auto *next_component =
            findResolvedComponent(next_object, "simplemodelview");
        if (base_component == nullptr || next_component == nullptr ||
            base_component->effective_json == next_component->effective_json) {
            continue;
        }
        auto *live = impl_->ecs.tryComponent<SimpleModelViewComponent>(
            binding.entity);
        if (live == nullptr) {
            throw std::runtime_error(
                "simplemodelview runtime component is absent on bound entity");
        }
        const auto &data =
            std::any_cast<const SimpleModelViewCodecData &>(
                next_component->requireRuntimeValue());
        if (data.model == live->model_name) continue;

        auto &model = impl_->models.getModelTemplateByName(data.model);
        auto staged = impl_->renderer.stagePreparedSceneModel(*impl_->candidate, model);
        impl_->renderer.setStagedAuthoringObject(staged, binding.authoring_object_id.value);
        const auto next_id = staged.id();
        impl_->renderer.publishPreparedSceneModel(*impl_->candidate, std::move(staged));
        auto next_value = *live;
        next_value.model_name = data.model;
        next_value.dirty = 0;
        next_value.model_instance_id = next_id;
        impl_->prepared.push_back(Impl::Prepared{
            .ecs_value = impl_->ecs.prepareComponentSwap(binding.entity,
                                                         next_value),
            .next_id = next_id,
            .old_id = live->model_instance_id,
        });
    }
    // Retiring instances must be absent from the candidate inventory before
    // its ray plan is resolved. Their live storage remains in the inverse.
    for (const auto &value : impl_->prepared) {
        if (!value.old_id) continue;
        std::erase_if(impl_->candidate->draw_inventory, [&](const auto &item) {
            return item.stable_identity.instance == *value.old_id;
        });
    }
    if (next_scene) {
        // Component migration retains removed model storage until finish so
        // rollback can restore it. Its geometry must already leave this plan.
        std::erase_if(impl_->candidate->draw_inventory, [&](const auto &item) {
            const auto object = impl_->candidate->slots.ray_authoring_object_ids.at(
                item.stable_identity.instance.index);
            if (object == 0) return false;
            return findResolvedComponent(
                findResolvedObject(next_scene, AuthoringObjectId{object}),
                "simplemodelview") == nullptr;
        });
        const auto next_procedural = proceduralComponents(next_scene);
        if (proceduralComponents(base_scene) != next_procedural) {
            if (!impl_->procedural)
                throw std::runtime_error("editor_projection_participant_missing@1: procedural_geometry");
            const auto generation = impl_->procedural->allocationGeneration();
            if (generation == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("procedural allocation generation exhausted");
            AssetDataFormatDocument assets;
            if (!next_procedural.empty())
                assets = parseAssetDataFormatJson(Json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson()));
            impl_->procedural_candidate = impl_->procedural->prepareScene(assets, *next_scene,
                FastModuleContainer::tryGet<VulkanManageCore>(), generation + 1,
                impl_->procedural_trace);
        }
        const auto procedurals = impl_->procedural
            ? impl_->procedural->preparedRayInstances(*next_scene,
                impl_->procedural_candidate ? &*impl_->procedural_candidate : nullptr)
            : std::vector<RayQueryGeometryInstanceSnapshot>{};
        impl_->renderer.prepareRayScenePlan(*impl_->candidate,
            impl_->candidate->ray_scene_plan ? impl_->candidate->ray_scene_plan->project : nullptr,
            next_scene, procedurals);
    }
}

void RendererModelProjectionAdapter::publish() noexcept {
    if (impl_->procedural_candidate)
        impl_->procedural->publishPrepared(*impl_->procedural_candidate, impl_->procedural_trace);
    if (impl_->candidate) {
        impl_->renderer.publishPreparedScene(std::move(*impl_->candidate));
        impl_->published = true;
    }
    for (auto &value : impl_->prepared) {
        value.ecs_value.publish();
    }
}
void RendererModelProjectionAdapter::rollback() noexcept {
    for (auto it = impl_->prepared.rbegin(); it != impl_->prepared.rend(); ++it) {
        it->ecs_value.rollback();
    }
    if (impl_->published) impl_->renderer.publishPreparedScene(std::move(*impl_->candidate));
    if (impl_->procedural_candidate) impl_->procedural->rollbackPrepared(*impl_->procedural_candidate);
    impl_->procedural_candidate.reset();
    impl_->candidate.reset();
    impl_->published = false;
    impl_->prepared.clear();
}
void RendererModelProjectionAdapter::finish() noexcept {
    for (auto &value : impl_->prepared) {
        value.ecs_value.finish();
        if (value.old_id) {
            (void)impl_->renderer.removeModelInstance(*value.old_id);
        }
    }
    if (impl_->procedural_candidate)
        impl_->procedural->enqueueRetirement(std::move(*impl_->procedural_candidate), impl_->procedural_trace);
    impl_->procedural_candidate.reset();
    impl_->prepared.clear();
    impl_->candidate.reset();
    impl_->published = false;
}

template <class Policy>
struct SnapshotProjectionAdapter<Policy>::Impl : Policy {
    typename Policy::Target &target;
    std::string scene_id;
    std::string adapter_name;
    std::optional<typename Policy::Prepared> old_state;
    std::optional<typename Policy::Prepared> next_state;
    bool published = false;

    Impl(typename Policy::Target &target, std::string scene_id,
         Policy policy = {})
        : Policy(std::move(policy)), target(target),
          scene_id(std::move(scene_id)),
          adapter_name(std::string(Policy::name_prefix) + this->scene_id) {}
};

template <class Policy>
SnapshotProjectionAdapter<Policy>::SnapshotProjectionAdapter(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

template <class Policy>
SnapshotProjectionAdapter<Policy>::~SnapshotProjectionAdapter() = default;

template <class Policy>
EditorProjectionAdapterKind
SnapshotProjectionAdapter<Policy>::kind() const noexcept {
    return Policy::adapter_kind;
}

template <class Policy>
std::string_view SnapshotProjectionAdapter<Policy>::name() const noexcept {
    return impl_->adapter_name;
}

template <class Policy>
EditorProjectionPublicationMode
SnapshotProjectionAdapter<Policy>::publicationMode() const noexcept {
    return Policy::publication_mode;
}

template <class Policy>
void SnapshotProjectionAdapter<Policy>::prepare(
    const EditorProjectionPrepareContext &context) {
    Policy::prepare(*impl_, context);
    impl_->published = false;
}

template <class Policy>
void SnapshotProjectionAdapter<Policy>::publish() noexcept {
    impl_->target.publishPrepared(std::move(*impl_->next_state));
    impl_->published = true;
}

template <class Policy>
void SnapshotProjectionAdapter<Policy>::rollback() noexcept {
    if (impl_->published) {
        impl_->target.publishPrepared(std::move(*impl_->old_state));
    }
    SnapshotProjectionAdapter::finish();
}

template <class Policy>
void SnapshotProjectionAdapter<Policy>::finish() noexcept {
    impl_->old_state.reset();
    impl_->next_state.reset();
    impl_->published = false;
}

struct CameraProjectionPolicy {
    using Target = Camera;
    using Prepared = Camera::PreparedSceneState;

    static constexpr auto adapter_kind = EditorProjectionAdapterKind::Camera;
    static constexpr auto publication_mode =
        EditorProjectionPublicationMode::StagedNoexcept;
    static constexpr std::string_view name_prefix = "codec.camera.";

    template <class State>
    static void prepare(State &state,
                        const EditorProjectionPrepareContext &context) {
        state.old_state = state.target.snapshotPrepared();
        state.next_state = state.target.prepareSceneCameras(
            state.scene_id, context.next_resolved);
    }
};

struct LightProjectionPolicy {
    using Target = LightContainer;
    using Prepared = LightContainer::PreparedLoad;

    static constexpr auto adapter_kind = EditorProjectionAdapterKind::Light;
    static constexpr auto publication_mode =
        EditorProjectionPublicationMode::InverseToken;
    static constexpr std::string_view name_prefix = "codec.light.";

    template <class State>
    static void prepare(State &state,
                        const EditorProjectionPrepareContext &context) {
        auto entries = prepareLightEntries(context.next_resolved,
                                           state.scene_id);
        state.old_state = state.target.snapshotPrepared();
        state.next_state = LightContainer::prepareLoad(entries);
    }
};

struct ColliderProjectionPolicy {
    using Target = PhysWorld;
    using Prepared = PhysWorld::PreparedState;

    static constexpr auto adapter_kind = EditorProjectionAdapterKind::PhysWorld;
    static constexpr auto publication_mode =
        EditorProjectionPublicationMode::InverseToken;
    static constexpr std::string_view name_prefix = "codec.collider.";

    ECSCoreTemplatePublic *ecs = nullptr;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings;

    template <class State>
    static void prepare(State &state,
                        const EditorProjectionPrepareContext &context) {
        ECSCoreTemplatePublic &ecs_core = *state.ecs;
        const auto entities = bindingMap(state.bindings);
        state.old_state = state.target.snapshotPrepared();
        std::unordered_map<std::string, const PhysWorld::Binding *> old_by_name;
        old_by_name.reserve(state.old_state->bindings.size());
        for (const auto &binding : state.old_state->bindings) {
            old_by_name.emplace(binding.identity.name, &binding);
        }

        std::vector<PhysWorld::Binding> next_bindings;
        const auto *scene =
            findResolvedScene(context.next_resolved, state.scene_id);
        if (scene != nullptr) {
            for (const auto &object : scene->objects) {
                const auto *component =
                    findResolvedComponent(&object, "collider");
                auto mesh_transform = std::variant<GameObjectId, PhysWorldTransform>{resolvedPhysTransform(object)};
                if (const auto runtime = entities.find(object.authoring_object_id.value);
                    runtime != entities.end() && findResolvedComponent(&object, "transform") != nullptr)
                    mesh_transform = runtime->second;
                appendModelPhysicsBinding(next_bindings, object, runtimeObjectIdentityName(
                    state.scene_id, object.authoring_object_id, object.name.value_or(std::string{})),
                    std::move(mesh_transform), old_by_name);
                if (component == nullptr) continue;
                const auto identity_name = runtimeObjectIdentityName(
                    state.scene_id, object.authoring_object_id,
                    object.name.value_or(std::string{}));
                ColliderComponent collider;
                component->runtime_codec->applyRuntime(
                    component->requireRuntimeValue(), &collider);

                PhysWorld::Binding binding{
                    .identity = {.name = identity_name},
                    .collider = collider,
                    .transform_source = PhysWorldTransform{},
                };
                if (const auto old = old_by_name.find(identity_name);
                    old != old_by_name.end()) {
                    binding.identity = old->second->identity;
                    binding.metadata = old->second->metadata;
                }
                const auto entity = entities.find(object.authoring_object_id.value);
                if (entity != entities.end() &&
                    ecs_core.tryComponent<TransformComponent>(entity->second) !=
                        nullptr) {
                    binding.transform_source = entity->second;
                } else if (const auto *transform =
                               findResolvedComponent(&object, "transform")) {
                    const auto &trs =
                        std::any_cast<const TransformCodecData &>(
                            transform->requireRuntimeValue());
                    binding.transform_source = PhysWorldTransform{
                        .pos = trs.pos,
                        .rotation = trs.rotation,
                        .scale = trs.scale,
                    };
                }
                next_bindings.push_back(std::move(binding));
            }
        }
        appendRuntimePhysicsBindings(next_bindings, *state.old_state, ecs_core);
        state.next_state =
            state.target.prepareBindings(std::move(next_bindings));
    }
};

template class SnapshotProjectionAdapter<CameraProjectionPolicy>;
template class SnapshotProjectionAdapter<LightProjectionPolicy>;
template class SnapshotProjectionAdapter<ColliderProjectionPolicy>;

CameraProjectionAdapter::CameraProjectionAdapter(Camera &camera,
                                                 std::string scene_id)
    : SnapshotProjectionAdapter<CameraProjectionPolicy>(
          std::make_unique<Impl>(camera, std::move(scene_id))) {}

LightProjectionAdapter::LightProjectionAdapter(LightContainer &lights,
                                               std::string scene_id)
    : SnapshotProjectionAdapter<LightProjectionPolicy>(
          std::make_unique<Impl>(lights, std::move(scene_id))) {}

ColliderProjectionAdapter::ColliderProjectionAdapter(
    ECSCoreTemplatePublic &ecs, PhysWorld &physics, std::string scene_id,
    std::vector<EditorProjectionRuntimeObjectBinding> bindings)
    : SnapshotProjectionAdapter<ColliderProjectionPolicy>(
          std::make_unique<Impl>(
              physics, std::move(scene_id),
              ColliderProjectionPolicy{.ecs = &ecs,
                                       .bindings = std::move(bindings)})) {
    (void)bindingMap(impl_->bindings);
}

EditorProjectionCallbackAdapter makeBehaviorAttachmentProjectionJunction(
    std::string name, void *context,
    EditorProjectionCallbackAdapter::Prepare prepare,
    EditorProjectionCallbackAdapter::NoexceptAction publish,
    EditorProjectionCallbackAdapter::NoexceptAction rollback,
    EditorProjectionCallbackAdapter::NoexceptAction finish) noexcept {
    return EditorProjectionCallbackAdapter{
        EditorProjectionAdapterKind::BehaviorAttachment, std::move(name),
        EditorProjectionPublicationMode::InverseToken, context, prepare,
        publish, rollback, finish};
}

} // namespace Pelican
