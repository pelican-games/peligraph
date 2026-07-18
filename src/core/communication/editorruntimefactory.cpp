#define GLM_ENABLE_EXPERIMENTAL
#include "editorruntimefactory.hpp"

#include "editorcommandservice.hpp"
#include "../asset/model.hpp"
#include "../container.hpp"
#include "../ecs/archetypemigration.hpp"
#include "../ecs/componentinfo.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../launchconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/editorprojectionadapters.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/scene.hpp"
#include "../os/inputsequence.hpp"
#include "../phys/physworld.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../vkcore/renderer.hpp"
#include "../watch/reloadservice.hpp"

#include <algorithm>
#include <any>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Pelican {
namespace {

struct EditorRuntimeModules {
    ECSCore &ecs_core;
    ComponentInfoManager &component_info;
    ModelAssetContainer &models;
    PolygonInstanceContainer &polygon_instances;
    Camera &camera;
    LightContainer &lights;
    PhysWorld &physics;
    PathResolver &path_resolver;
    ProjectBasicConfig &project_config;
    SceneLoader &scene_loader;
    InputSequenceRuntime &input_sequence;
    EngineLaunchConfig &launch_config;
    watch::ReloadService *reload_service;
    Renderer &renderer;
};

EditorRuntimeModules resolveEditorRuntimeModules() {
    return {
        GET_MODULE(ECSCore),
        GET_MODULE(ComponentInfoManager),
        GET_MODULE(ModelAssetContainer),
        GET_MODULE(PolygonInstanceContainer),
        GET_MODULE(Camera),
        GET_MODULE(LightContainer),
        GET_MODULE(PhysWorld),
        GET_MODULE(PathResolver),
        GET_MODULE(ProjectBasicConfig),
        GET_MODULE(SceneLoader),
        GET_MODULE(InputSequenceRuntime),
        GET_MODULE(EngineLaunchConfig),
        FastModuleContainer::tryGet<watch::ReloadService>(),
        GET_MODULE(Renderer),
    };
}

std::vector<EditorProjectionRuntimeObjectBinding>
collectEditorRuntimeBindings(const EditorRuntimeModules &modules) {
    std::vector<EditorProjectionRuntimeObjectBinding> result;
    const auto scenes = modules.project_config.sceneDocument().query();
    const auto scene = std::find_if(scenes.begin(), scenes.end(),
                                    [&](const auto &candidate) {
                                        return candidate.scene_id ==
                                               modules.scene_loader.currentScene();
                                    });
    if (scene == scenes.end()) return result;
    for (const auto &object : scene->objects) {
        if (!object.name) continue;
        const auto entity = modules.scene_loader.objectId(*object.name);
        if (entity) result.push_back({object.authoring_object_id, *entity});
    }
    return result;
}

std::optional<EntityId> boundEditorEntity(
    std::span<const EditorProjectionRuntimeObjectBinding> bindings,
    AuthoringObjectId object_id) {
    const auto found = std::find_if(bindings.begin(), bindings.end(),
                                    [&](const auto &binding) {
                                        return binding.authoring_object_id == object_id;
                                    });
    return found == bindings.end() ? std::nullopt
                                   : std::optional{found->entity};
}

class SimpleModelMigrationParticipant final
    : public ECSArchetypeMigrationAdapter {
    ModelAssetContainer &models_;
    PolygonInstanceContainer &renderer_;
    ECSArchetypeMigrationKind original_kind_;
    std::optional<StagedModelInstance> staged_;
    std::optional<ModelInstanceId> next_id_;
    std::optional<ModelInstanceId> old_id_;
    bool published_ = false;

  public:
    SimpleModelMigrationParticipant(ModelAssetContainer &models,
                                    PolygonInstanceContainer &renderer,
                                    ECSArchetypeMigrationKind kind)
        : models_{models}, renderer_{renderer}, original_kind_{kind} {}

    void prepare(const ECSArchetypeMigrationPrepareContext &context) override {
        if (context.kind == ECSArchetypeMigrationKind::add) {
            auto *component = static_cast<SimpleModelViewComponent *>(
                context.staged_component);
            auto &model = models_.getModelTemplateByName(component->model_name);
            staged_.emplace(renderer_.stageModelInstance(model));
            glm::mat4 matrix{1.0F};
            if (const auto *transform =
                    context.core.tryComponent<TransformComponent>(context.entity)) {
                matrix = glm::translate(glm::mat4{1.0F}, transform->pos) *
                         glm::toMat4(transform->rotation) *
                         glm::scale(glm::mat4{1.0F}, transform->scale);
            }
            renderer_.setStagedModelMatrix(*staged_, matrix);
            next_id_ = staged_->id();
            component->model_instance_id = next_id_;
        } else {
            const auto *component = static_cast<const SimpleModelViewComponent *>(
                context.live_component);
            old_id_ = component->model_instance_id;
        }
    }

    void rollback(const ECSArchetypeMigrationPrepareContext &) noexcept override {
        staged_.reset();
        next_id_.reset();
        old_id_.reset();
        published_ = false;
    }

    void publish(const ECSArchetypeMigrationPublishContext &context) noexcept override {
        if (original_kind_ == ECSArchetypeMigrationKind::add) {
            if (context.kind == ECSArchetypeMigrationKind::add && staged_) {
                renderer_.publishModelInstance(std::move(*staged_));
                staged_.reset();
                published_ = true;
            } else if (context.kind == ECSArchetypeMigrationKind::remove &&
                       published_ && next_id_) {
                (void)renderer_.removeModelInstance(*next_id_);
                published_ = false;
            }
        }
    }

    void finish() noexcept {
        if (original_kind_ == ECSArchetypeMigrationKind::remove && old_id_) {
            (void)renderer_.removeModelInstance(*old_id_);
        }
        staged_.reset();
        next_id_.reset();
        old_id_.reset();
        published_ = false;
    }
};

class EditorEcsMigrationProjectionAdapter final
    : public EditorProjectionAdapter {
    ECSCoreTemplatePublic &ecs_;
    ComponentInfoManager &components_;
    ModelAssetContainer &models_;
    PolygonInstanceContainer &renderer_;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings_;
    AuthoringObjectId object_id_{};
    std::string component_name_;
    nlohmann::ordered_json operation_;
    ECSArchetypeMigrationKind kind_ = ECSArchetypeMigrationKind::add;
    std::string name_;
    std::optional<ECSArchetypeMigrationToken> token_;
    std::optional<SimpleModelMigrationParticipant> simple_model_;

  public:
    EditorEcsMigrationProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        ModelAssetContainer &models, PolygonInstanceContainer &renderer,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        nlohmann::ordered_json operation, std::size_t operation_index)
        : ecs_{ecs}, components_{components}, models_{models}, renderer_{renderer},
          bindings_{std::move(bindings)},
          object_id_{operation.at("object_id").get<std::uint64_t>()},
          component_name_{operation.at("component_slot").get<std::string>()},
          operation_{std::move(operation)},
          kind_{operation_.at("op") == "add_component"
                    ? ECSArchetypeMigrationKind::add
                    : ECSArchetypeMigrationKind::remove},
          name_{"component_migration." + std::to_string(object_id_.value) + "." +
                component_name_ + "." + std::to_string(operation_index)} {}

    EditorProjectionAdapterKind kind() const noexcept override {
        return EditorProjectionAdapterKind::EcsArchetype;
    }
    std::string_view name() const noexcept override { return name_; }
    EditorProjectionPublicationMode publicationMode() const noexcept override {
        return EditorProjectionPublicationMode::InverseToken;
    }

    void prepare(const EditorProjectionPrepareContext &) override {
        const auto entity = boundEditorEntity(bindings_, object_id_);
        if (!entity) throw std::runtime_error("component edit target has no runtime binding");
        const auto component_id = components_.getComponentIdByName(component_name_);
        std::vector<ECSArchetypeMigrationAdapter *> participants;
        if (component_name_ == "simplemodelview") {
            simple_model_.emplace(models_, renderer_, kind_);
            participants.push_back(&*simple_model_);
        }
        if (kind_ == ECSArchetypeMigrationKind::add) {
            const auto &codec = requireComponentCodec(component_name_);
            auto decoded = codec.decodeAuthored(operation_.at("component"));
            token_.emplace(ECSArchetypeMigration::prepareAdd(
                ecs_, *entity, component_id,
                [this, codec = &codec,
                 decoded = std::move(decoded)](void *target) {
                    if (component_name_ == "transform") {
                        TransformCodecTarget transform{
                            .world = static_cast<TransformComponent *>(target)};
                        codec->applyRuntime(decoded, &transform);
                    } else if (codec->runtime_kind ==
                               ComponentCodecRuntimeKind::Ecs) {
                        codec->applyRuntime(decoded, target);
                    } else {
                        components_.loadByJson(target,
                                               operation_.at("component"));
                    }
                },
                participants));
        } else {
            token_.emplace(ECSArchetypeMigration::prepareRemove(
                ecs_, *entity, component_id, participants));
        }
    }

    void publish() noexcept override {
        if (token_) token_->publish();
    }
    void rollback() noexcept override {
        if (token_) token_->rollback();
        token_.reset();
        simple_model_.reset();
    }
    void finish() noexcept override {
        if (token_) token_->finish();
        token_.reset();
        if (simple_model_) simple_model_->finish();
        simple_model_.reset();
    }
};

class StandaloneTransformProjectionAdapter final
    : public EditorProjectionAdapter {
    using Token =
        ECSCoreTemplatePublic::PreparedComponentSwap<TransformComponent>;

    ECSCoreTemplatePublic &ecs_;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings_;
    std::set<std::string> scenes_;
    std::vector<Token> prepared_;

  public:
    StandaloneTransformProjectionAdapter(
        ECSCoreTemplatePublic &ecs,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        std::set<std::string> scenes)
        : ecs_{ecs}, bindings_{std::move(bindings)},
          scenes_{std::move(scenes)} {}

    EditorProjectionAdapterKind kind() const noexcept override {
        return EditorProjectionAdapterKind::EcsExistingValue;
    }
    std::string_view name() const noexcept override {
        return "transform_standalone";
    }
    EditorProjectionPublicationMode publicationMode() const noexcept override {
        return EditorProjectionPublicationMode::StagedNoexcept;
    }

    void prepare(const EditorProjectionPrepareContext &context) override {
        prepared_.clear();
        const auto next_scenes = context.next_document.query();
        const auto &codec = requireComponentCodec("transform");

        for (const auto &scene : next_scenes) {
            if (!scenes_.contains(scene.scene_id)) continue;

            std::unordered_map<std::string, const AuthoringObjectView *> by_name;
            for (const auto &object : scene.objects) {
                if (object.name) by_name.emplace(*object.name, &object);
            }
            std::unordered_map<std::uint64_t, TransformComponent> worlds;
            std::unordered_set<std::uint64_t> visiting;
            const auto compute_world = [&](auto &&self,
                                           const AuthoringObjectView &object)
                -> const TransformComponent & {
                if (const auto found = worlds.find(object.authoring_object_id.value);
                    found != worlds.end()) {
                    return found->second;
                }
                if (!visiting.insert(object.authoring_object_id.value).second) {
                    throw std::runtime_error(
                        "validated transform hierarchy became cyclic");
                }
                const auto component = std::find_if(
                    object.components.begin(), object.components.end(),
                    [](const auto &candidate) {
                        return candidate.authoredJson().at("name") ==
                               "transform";
                    });
                if (component == object.components.end()) {
                    throw std::runtime_error(
                        "transform hierarchy object has no authored transform");
                }
                const TransformComponent *parent_world = nullptr;
                if (object.parent) {
                    const auto parent = by_name.find(*object.parent);
                    if (parent == by_name.end()) {
                        throw std::runtime_error(
                            "transform hierarchy parent is absent");
                    }
                    parent_world = &self(self, *parent->second);
                }
                TransformComponent next{};
                TransformCodecTarget target{.world = &next,
                                            .parent_world = parent_world};
                codec.applyRuntime(
                    codec.decodeAuthored(component->authoredJson()), &target);
                visiting.erase(object.authoring_object_id.value);
                return worlds.emplace(object.authoring_object_id.value, next)
                    .first->second;
            };

            for (const auto &binding : bindings_) {
                auto *world =
                    ecs_.tryComponent<TransformComponent>(binding.entity);
                const auto *local =
                    ecs_.tryComponent<LocalTransformComponent>(binding.entity);
                if (world == nullptr || local != nullptr) continue;
                const auto object = std::find_if(
                    scene.objects.begin(), scene.objects.end(),
                    [&](const auto &candidate) {
                        return candidate.authoring_object_id ==
                               binding.authoring_object_id;
                    });
                if (object == scene.objects.end()) continue;
                prepared_.push_back(ecs_.prepareComponentSwap(
                    binding.entity, compute_world(compute_world, *object)));
            }
        }
    }

    void publish() noexcept override {
        for (auto &token : prepared_) token.publish();
    }
    void rollback() noexcept override {
        for (auto it = prepared_.rbegin(); it != prepared_.rend(); ++it) {
            it->rollback();
        }
        prepared_.clear();
    }
    void finish() noexcept override {
        for (auto &token : prepared_) token.finish();
        prepared_.clear();
    }
};

class ReparentLocalTransformProjectionAdapter final
    : public EditorProjectionAdapter {
    ECSCoreTemplatePublic &ecs_;
    ComponentInfoManager &components_;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings_;
    AuthoringObjectId object_id_{};
    std::optional<AuthoringObjectId> parent_id_;
    std::string name_;
    std::optional<ECSArchetypeMigrationToken> token_;

  public:
    ReparentLocalTransformProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        const nlohmann::ordered_json &operation, std::size_t operation_index)
        : ecs_{ecs}, components_{components}, bindings_{std::move(bindings)},
          object_id_{operation.at("object_id").get<std::uint64_t>()},
          parent_id_{operation.at("new_parent_id").is_null()
                         ? std::optional<AuthoringObjectId>{}
                         : std::optional{AuthoringObjectId{
                               operation.at("new_parent_id")
                                   .get<std::uint64_t>()}}},
          name_{"reparent_local_transform." +
                std::to_string(object_id_.value) + "." +
                std::to_string(operation_index)} {}

    EditorProjectionAdapterKind kind() const noexcept override {
        return EditorProjectionAdapterKind::EcsArchetype;
    }
    std::string_view name() const noexcept override { return name_; }
    EditorProjectionPublicationMode publicationMode() const noexcept override {
        return EditorProjectionPublicationMode::InverseToken;
    }

    void prepare(const EditorProjectionPrepareContext &context) override {
        token_.reset();
        if (!parent_id_) return;
        const auto entity = boundEditorEntity(bindings_, object_id_);
        const auto parent = boundEditorEntity(bindings_, *parent_id_);
        if (!entity || !parent) {
            throw std::runtime_error(
                "reparent target or parent has no runtime binding");
        }
        if (ecs_.tryComponent<LocalTransformComponent>(*entity) != nullptr) {
            return;
        }

        const auto scenes = context.next_document.query();
        const AuthoringObjectView *object = nullptr;
        for (const auto &scene : scenes) {
            const auto found = std::find_if(
                scene.objects.begin(), scene.objects.end(),
                [&](const auto &candidate) {
                    return candidate.authoring_object_id == object_id_;
                });
            if (found != scene.objects.end()) {
                object = &*found;
                break;
            }
        }
        if (object == nullptr) {
            throw std::runtime_error("reparent target disappeared from document");
        }
        const auto transform = std::find_if(
            object->components.begin(), object->components.end(),
            [](const auto &candidate) {
                return candidate.authoredJson().at("name") == "transform";
            });
        if (transform == object->components.end()) {
            throw std::runtime_error("reparent target has no transform");
        }
        const auto decoded = requireComponentCodec("transform").decodeAuthored(
            transform->authoredJson());
        const auto &data =
            std::any_cast<const TransformCodecData &>(decoded);
        const LocalTransformComponent next{
            .scale = data.scale,
            .rotation = data.rotation,
            .pos = data.pos,
            .parent = *parent,
        };
        token_.emplace(ECSArchetypeMigration::prepareAdd(
            ecs_, *entity,
            components_.getComponentIdByName("localtransform"),
            [next](void *target) {
                *static_cast<LocalTransformComponent *>(target) = next;
            }));
    }

    void publish() noexcept override {
        if (token_) token_->publish();
    }
    void rollback() noexcept override {
        if (token_) token_->rollback();
        token_.reset();
    }
    void finish() noexcept override {
        if (token_) token_->finish();
        token_.reset();
    }
};

std::vector<TransformProjectionBinding> makeTransformBindings(
    EditorRuntimeModules &modules,
    std::span<const EditorProjectionRuntimeObjectBinding> runtime_bindings) {
    std::vector<TransformProjectionBinding> result;
    auto &ecs = modules.ecs_core.getTemplatePublicModule();
    const auto scenes = modules.project_config.sceneDocument().query();
    for (const auto &binding : runtime_bindings) {
        for (const auto &scene : scenes) {
            const auto object = std::find_if(scene.objects.begin(), scene.objects.end(),
                                             [&](const auto &candidate) {
                                                 return candidate.authoring_object_id ==
                                                        binding.authoring_object_id;
                                             });
            if (object == scene.objects.end() || !object->name) continue;
            auto *world = ecs.tryComponent<TransformComponent>(binding.entity);
            auto *local = ecs.tryComponent<LocalTransformComponent>(binding.entity);
            if (world == nullptr || local == nullptr) continue;
            result.push_back({scene.scene_id, *object->name, binding.entity,
                              world, local});
        }
    }
    return result;
}

std::string operationSceneId(const nlohmann::ordered_json &operation,
                             std::string_view fallback) {
    if (const auto scene = operation.find("scene_id");
        scene != operation.end() && scene->is_string()) {
        return scene->get<std::string>();
    }
    if (const auto closures = operation.find("closures");
        closures != operation.end() && !closures->empty()) {
        return closures->front().at("scene_id").get<std::string>();
    }
    return std::string{fallback};
}

class EphemeralEditorProjectionTarget final
    : public EditorProjectionDocumentTarget {
    AuthoringSceneDocument document_;

  public:
    explicit EphemeralEditorProjectionTarget(
        const AuthoringSceneDocument &source)
        : document_{source.stage(
              source.rawJson(), SceneRevision{source.revision().value + 1U})} {}

    const AuthoringSceneDocument &projectionDocument() const override {
        return document_;
    }
    SceneRevision nextProjectionRevision() const override {
        return SceneRevision{document_.revision().value + 1U};
    }
    void publishProjectionDocument(
        AuthoringSceneDocument &&document) noexcept override {
        document_.swap(document);
    }
};

EditorProjectionResult executeEditorPreview(
    EditorRuntimeModules &modules,
    std::span<const EditorProjectionRuntimeObjectBinding> runtime_bindings,
    const EditorPreviewExecutionRequest &request) {
    std::vector<std::unique_ptr<EditorProjectionAdapter>> adapters;
    std::set<std::string> transform_scenes;
    std::set<std::string> light_scenes;
    const auto fallback_scene = modules.scene_loader.currentScene();
    for (const auto &operation : request.operations) {
        const auto scene_id = operationSceneId(operation, fallback_scene);
        const auto component = operation.at("component_slot").get<std::string>();
        if (component == "transform") transform_scenes.insert(scene_id);
        if (component == "light") light_scenes.insert(scene_id);
    }
    if (!transform_scenes.empty()) {
        adapters.push_back(std::make_unique<TransformProjectionAdapter>(
            makeTransformBindings(modules, runtime_bindings)));
        adapters.push_back(
            std::make_unique<StandaloneTransformProjectionAdapter>(
                modules.ecs_core.getTemplatePublicModule(),
                std::vector<EditorProjectionRuntimeObjectBinding>{
                    runtime_bindings.begin(), runtime_bindings.end()},
                transform_scenes));
    }
    for (const auto &scene : light_scenes) {
        adapters.push_back(std::make_unique<LightProjectionAdapter>(
            modules.lights, scene));
    }

    std::vector<EditorProjectionAdapter *> adapter_ptrs;
    adapter_ptrs.reserve(adapters.size());
    for (auto &adapter : adapters) adapter_ptrs.push_back(adapter.get());
    EphemeralEditorProjectionTarget target{
        modules.project_config.sceneDocument()};
    EditorProjectionTransaction transaction{
        target, target.projectionDocument().revision()};
    return transaction.commit(request.commands, adapter_ptrs);
}

EditorProjectionResult executeEditorProjection(
    EditorRuntimeModules &modules,
    std::vector<EditorProjectionRuntimeObjectBinding> &runtime_bindings,
    const EditorEditExecutionRequest &request) {
    auto &ecs = modules.ecs_core.getTemplatePublicModule();
    std::vector<std::unique_ptr<EditorProjectionAdapter>> adapters;
    struct LifecycleUpdate {
        AuthoringObjectId object_id{};
        SceneObjectProjectionAdapter *adapter = nullptr;
    };
    std::vector<LifecycleUpdate> lifecycle_updates;
    std::set<std::string> transform_scenes;
    std::set<std::string> ecs_codec_scenes;
    std::set<std::string> renderer_scenes;
    std::set<std::string> camera_scenes;
    std::set<std::string> light_scenes;
    std::set<std::string> collider_scenes;
    const auto fallback_scene = modules.scene_loader.currentScene();

    const auto addLifecycle = [&](AuthoringObjectId object_id,
                                  const std::string &scene_id) {
        auto adapter = std::make_unique<SceneObjectProjectionAdapter>(
            ecs, modules.component_info, modules.models,
            modules.polygon_instances, modules.camera, modules.lights,
            modules.physics, scene_id, object_id, runtime_bindings);
        auto *raw = adapter.get();
        adapters.push_back(std::move(adapter));
        lifecycle_updates.push_back({object_id, raw});
    };

    for (std::size_t index = 0; index < request.operations.size(); ++index) {
        const auto &operation = request.operations[index];
        const auto op = operation.at("op").get<std::string>();
        const auto scene_id = operationSceneId(operation, fallback_scene);
        if (op == "spawn") {
            addLifecycle(AuthoringObjectId{
                             operation.at("object_id").get<std::uint64_t>()},
                         scene_id);
        } else if (op == "destroy" || op == "remove_objects") {
            for (const auto &id : operation.at("object_ids")) {
                addLifecycle(AuthoringObjectId{id.get<std::uint64_t>()}, scene_id);
            }
        } else if (op == "restore_objects") {
            for (const auto &closure : operation.at("closures")) {
                addLifecycle(AuthoringObjectId{
                                 closure.at("authoring_object_id").get<std::uint64_t>()},
                             closure.at("scene_id").get<std::string>());
            }
        } else if (op == "reparent") {
            transform_scenes.insert(scene_id);
            adapters.push_back(
                std::make_unique<ReparentLocalTransformProjectionAdapter>(
                    ecs, modules.component_info, runtime_bindings, operation,
                    index));
        } else if (op == "set_component_value") {
            const auto component = operation.at("component_slot").get<std::string>();
            if (component == "transform") transform_scenes.insert(scene_id);
            else if (component == "simplemodelview") renderer_scenes.insert(scene_id);
            else if (component == "animation" || component == "sprite_view")
                ecs_codec_scenes.insert(scene_id);
            else if (component == "camera") camera_scenes.insert(scene_id);
            else if (component == "light") light_scenes.insert(scene_id);
            else if (component == "collider") collider_scenes.insert(scene_id);
        } else if (op == "add_component" || op == "remove_component") {
            const auto component = operation.at("component_slot").get<std::string>();
            const auto &codec = requireComponentCodec(component);
            if (codec.runtime_kind == ComponentCodecRuntimeKind::Ecs ||
                codec.runtime_kind == ComponentCodecRuntimeKind::Camera) {
                adapters.push_back(std::make_unique<EditorEcsMigrationProjectionAdapter>(
                    ecs, modules.component_info, modules.models,
                    modules.polygon_instances, runtime_bindings, operation, index));
            }
            if (codec.runtime_kind == ComponentCodecRuntimeKind::Camera) {
                camera_scenes.insert(scene_id);
            } else if (codec.runtime_kind == ComponentCodecRuntimeKind::Light) {
                light_scenes.insert(scene_id);
            } else if (codec.runtime_kind == ComponentCodecRuntimeKind::Collider) {
                collider_scenes.insert(scene_id);
            }
        }
    }

    if (!transform_scenes.empty()) {
        adapters.push_back(std::make_unique<TransformProjectionAdapter>(
            makeTransformBindings(modules, runtime_bindings)));
        adapters.push_back(
            std::make_unique<StandaloneTransformProjectionAdapter>(
                ecs, runtime_bindings, transform_scenes));
    }
    for (const auto &scene : ecs_codec_scenes) {
        adapters.push_back(std::make_unique<EcsCodecProjectionAdapter>(
            ecs, scene, runtime_bindings));
    }
    for (const auto &scene : renderer_scenes) {
        adapters.push_back(std::make_unique<RendererModelProjectionAdapter>(
            ecs, modules.models, modules.polygon_instances, scene,
            runtime_bindings));
    }
    for (const auto &scene : camera_scenes) {
        adapters.push_back(std::make_unique<CameraProjectionAdapter>(
            modules.camera, scene));
    }
    for (const auto &scene : light_scenes) {
        adapters.push_back(std::make_unique<LightProjectionAdapter>(
            modules.lights, scene));
    }
    for (const auto &scene : collider_scenes) {
        adapters.push_back(std::make_unique<ColliderProjectionAdapter>(
            ecs, modules.physics, scene, runtime_bindings));
    }

    std::vector<EditorProjectionAdapter *> adapter_ptrs;
    adapter_ptrs.reserve(adapters.size());
    for (auto &adapter : adapters) adapter_ptrs.push_back(adapter.get());
    ProjectBasicConfigProjectionTarget target{modules.project_config};
    EditorProjectionTransaction transaction{target, request.base_revision};
    auto result = transaction.commit(request.commands, adapter_ptrs);
    if (result.committed()) {
        for (const auto &update : lifecycle_updates) {
            const auto next = update.adapter->runtimeBindings();
            const auto replacement = std::find_if(next.begin(), next.end(),
                                                  [&](const auto &binding) {
                                                      return binding.authoring_object_id ==
                                                             update.object_id;
                                                  });
            const auto current = std::find_if(runtime_bindings.begin(),
                                              runtime_bindings.end(),
                                              [&](const auto &binding) {
                                                  return binding.authoring_object_id ==
                                                         update.object_id;
                                              });
            if (replacement == next.end()) {
                if (current != runtime_bindings.end()) runtime_bindings.erase(current);
            } else if (current == runtime_bindings.end()) {
                runtime_bindings.push_back(*replacement);
            } else {
                *current = *replacement;
            }
        }
    }
    return result;
}

std::uint64_t editorTransitionEpoch(const EditorRuntimeModules &modules) {
    std::string state = modules.scene_loader.currentScene();
    if (modules.reload_service != nullptr) {
        const auto reload = modules.reload_service->statusJson();
        state += "\n" + reload.value("state", std::string{});
        state += "\n" + std::to_string(reload.value("epoch", std::uint64_t{}));
        if (const auto runtime = reload.find("runtime"); runtime != reload.end()) {
            state += "\n" + runtime->dump();
        }
    }
    return static_cast<std::uint64_t>(std::hash<std::string>{}(state));
}

EditorGateObservation editorGateObservation(const EditorRuntimeModules &modules) {
    std::uint32_t reasons = 0;
    if (modules.launch_config.input_replay || modules.input_sequence.isReplaying()) {
        reasons |= editorGateReasonBit(EditorGateReason::replay);
    }
    if (modules.launch_config.golden_mode) {
        reasons |= editorGateReasonBit(EditorGateReason::golden);
    }
    if (modules.launch_config.strict_assets) {
        reasons |= editorGateReasonBit(EditorGateReason::strict);
    }
    if (internal::behaviorCallbackActive()) {
        reasons |= editorGateReasonBit(
            EditorGateReason::reload_scene_transition);
    }
    if (modules.reload_service != nullptr) {
        const auto state = modules.reload_service->statusJson().value(
            "state", std::string{});
        if (state == "reconciling") {
            reasons |= editorGateReasonBit(
                EditorGateReason::reload_scene_transition);
        }
    }
    return {.reasons = reasons,
            .transition_epoch = editorTransitionEpoch(modules)};
}

EditorRuntimeObjectState queryEditorRuntime(const EditorRuntimeModules &modules,
                                            std::span<const EditorProjectionRuntimeObjectBinding> bindings,
                                            const AuthoringSceneView &scene,
                                            const AuthoringObjectView &object) {
    EditorRuntimeObjectState result;
    result.component_runtime_json.resize(object.components.size());
    if (scene.scene_id != modules.scene_loader.currentScene()) return result;
    const auto entity_id = boundEditorEntity(bindings, object.authoring_object_id);
    if (!entity_id) return result;
    result.entity_id = entity_id;

    auto &ecs = modules.ecs_core.getTemplatePublicModule();
    auto &component_info = modules.component_info;
    for (std::size_t index = 0; index < object.components.size(); ++index) {
        const auto name = object.components[index].authoredJson().at("name").get<std::string>();
        const auto *codec = findComponentCodec(name);
        if (codec == nullptr || codec->runtime_kind != ComponentCodecRuntimeKind::Ecs) continue;
        const auto component_id = component_info.getComponentIdByName(name);
        auto *raw_component = ecs.tryComponentRaw(*entity_id, component_id);
        if (raw_component == nullptr) continue;
        if (name == "transform") {
            auto *world = static_cast<TransformComponent *>(raw_component);
            auto *local = ecs.tryComponent<LocalTransformComponent>(*entity_id);
            result.component_runtime_json[index] = projectTransformRuntimeJson(
                TransformCodecTarget{.world = world, .local = local});
        } else {
            result.component_runtime_json[index] =
                codec->encodeCanonical(codec->projectRuntime(raw_component));
        }
    }
    return result;
}

bool pathIsWithin(const std::filesystem::path &path, const std::filesystem::path &root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

std::vector<EditorAssetQueryResult> collectEditorAssets(const EditorRuntimeModules &modules) {
    std::vector<EditorAssetQueryResult> result;
    const auto asset_data = nlohmann::json::parse(modules.project_config.assetDataJson());
    const auto stores = modules.path_resolver.stores();
    if (!asset_data.is_object()) return result;
    for (auto category = asset_data.begin(); category != asset_data.end(); ++category) {
        if (!category.value().is_array()) continue;
        auto kind = category.key();
        if (kind.size() > 1 && kind.back() == 's') kind.pop_back();
        for (const auto &entry : category.value()) {
            if (!entry.is_object() || !entry.contains("name") || !entry.at("name").is_string() ||
                !entry.contains("path") || !entry.at("path").is_string()) {
                continue;
            }
            const auto path_text = entry.at("path").get<std::string>();
            std::filesystem::path resolved_path{path_text};
            bool exists = false;
            if (resolved_path.is_absolute()) {
                exists = std::filesystem::exists(resolved_path);
            } else {
                try {
                    const auto resolved = modules.path_resolver.resolveExistingFileReference(path_text);
                    if (const auto *path = std::get_if<std::filesystem::path>(&resolved)) {
                        resolved_path = *path;
                        exists = std::filesystem::exists(resolved_path);
                    } else if (const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved)) {
                        resolved_path = fragment->path;
                        exists = std::filesystem::exists(resolved_path);
                    } else {
                        exists = true;
                    }
                } catch (const std::exception &) {
                    exists = false;
                }
            }

            std::string store = "project";
            if (resolved_path.is_absolute()) {
                for (const auto &candidate : stores) {
                    if (pathIsWithin(resolved_path.lexically_normal(), candidate.root.lexically_normal())) {
                        store = candidate.name;
                        break;
                    }
                }
            }
            result.push_back(EditorAssetQueryResult{.id = entry.at("name").get<std::string>(),
                                                    .kind = kind,
                                                    .path = path_text,
                                                    .store = std::move(store),
                                                    .status = exists ? "loaded" : "missing"});
        }
    }
    return result;
}

struct EditorRuntimeState {
    EditorRuntimeModules modules;
    std::vector<EditorProjectionRuntimeObjectBinding> runtime_bindings;

    EditorRuntimeState()
        : modules{resolveEditorRuntimeModules()},
          runtime_bindings{collectEditorRuntimeBindings(modules)} {}
};

} // namespace

std::unique_ptr<EditorCommandService> makeEditorRuntimeService() {
    auto runtime = std::make_shared<EditorRuntimeState>();
    return std::make_unique<EditorCommandService>(EditorCommandServiceDependencies{
        .document = [runtime]() -> const AuthoringSceneDocument & {
            return runtime->modules.project_config.sceneDocument();
        },
        .current_scene_id = [runtime] {
            return runtime->modules.scene_loader.currentScene();
        },
        .runtime_query =
            [runtime](const AuthoringSceneView &scene,
                      const AuthoringObjectView &object) {
                return queryEditorRuntime(runtime->modules,
                                          runtime->runtime_bindings,
                                          scene, object);
            },
        .assets = [runtime] { return collectEditorAssets(runtime->modules); },
        .snapshot_state = [] { return EditorSnapshotState{}; },
        .edit = EditorEditRuntimeDependencies{
            .document = [runtime]() -> const AuthoringSceneDocument & {
                return runtime->modules.project_config.sceneDocument();
            },
            .current_scene_id = [runtime] {
                return runtime->modules.scene_loader.currentScene();
            },
            .execute = [runtime](const EditorEditExecutionRequest &request) {
                return executeEditorProjection(runtime->modules,
                                               runtime->runtime_bindings,
                                               request);
            },
            .execute_preview =
                [runtime](const EditorPreviewExecutionRequest &request) {
                    return executeEditorPreview(runtime->modules,
                                                runtime->runtime_bindings,
                                                request);
                },
            .preview_boundary = [runtime] {
                runtime->modules.renderer.resetTemporalHistory();
            },
            .gate = [runtime] {
                return editorGateObservation(runtime->modules);
            },
            .install_commit_hook = true,
        },
        .save_scene = [runtime] {
            if (runtime->modules.scene_loader.hasRuntimeOnlyChanges()) {
                throw EditorCommandError{
                    EditorCommandErrorCode::RuntimeOnlyData,
                    "scene has runtime-only changes; reload or commit them through the editor before saving",
                };
            }
            const auto saved = runtime->modules.project_config.saveSceneDocument();
            return SaveSceneResult{
                .scene_revision = saved.scene_revision,
                .digest = {.algorithm = "sha256", .hex = saved.digest},
                .byte_count = saved.byte_count,
                .scene_hot_reload = false,
            };
        },
    });
}

} // namespace Pelican
