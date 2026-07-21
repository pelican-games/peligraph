#define GLM_ENABLE_EXPERIMENTAL
#include "editorruntimefactory.hpp"

#include "editorcommandservice.hpp"
#include "../appflow/enginetime.hpp"
#include "../asset/model.hpp"
#include "../container.hpp"
#include "../ecs/archetypemigration.hpp"
#include "../ecs/componentinfo.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../gamelogic/gamelogicreload.hpp"
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
#include "../userpublic/details/event/registerer.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/rendertiming.hpp"
#include "../watch/reloadgate.hpp"
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

EditorGateObservation internal::applyBehaviorEditConcurrencyGate(
    EditorGateObservation observation, bool behavior_callback_active,
    bool game_logic_reload_active, bool reload_reconciling) noexcept {
    if (behavior_callback_active || game_logic_reload_active ||
        reload_reconciling) {
        observation.reasons |= editorGateReasonBit(
            EditorGateReason::reload_scene_transition);
    }
    return observation;
}

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
    EngineTime &engine_time;
    DeletionQueue *deletion_queue;
    RenderTiming *render_timing;
    BehaviorAttachmentArena &behavior_arena;
    watch::ReloadGate *reload_gate;
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
        GET_MODULE(EngineTime),
        FastModuleContainer::tryGet<DeletionQueue>(),
        FastModuleContainer::tryGet<RenderTiming>(),
        GET_MODULE(BehaviorAttachmentArena),
        FastModuleContainer::tryGet<watch::ReloadGate>(),
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

struct BehaviorProjectionJunctionState {
    BehaviorAttachmentArena &arena;
    std::vector<BehaviorAttachmentEdit> edits;
    std::unique_ptr<PreparedBehaviorAttachmentEdits> prepared;

    static void prepare(void *context,
                        const EditorProjectionPrepareContext &) {
        auto &state = *static_cast<BehaviorProjectionJunctionState *>(context);
        state.prepared = state.arena.prepareEditorEdits(state.edits);
    }
    static void publish(void *context) noexcept {
        auto &state = *static_cast<BehaviorProjectionJunctionState *>(context);
        if (state.prepared) state.prepared->publish();
    }
    static void rollback(void *context) noexcept {
        auto &state = *static_cast<BehaviorProjectionJunctionState *>(context);
        if (state.prepared) state.prepared->rollback();
        state.prepared.reset();
    }
    static void finish(void *context) noexcept {
        auto &state = *static_cast<BehaviorProjectionJunctionState *>(context);
        if (state.prepared) state.prepared->finish();
        state.prepared.reset();
    }
};

BehaviorAttachmentIdentity behaviorIdentity(
    const nlohmann::ordered_json &operation) {
    return BehaviorAttachmentIdentity{
        .handle = operation.contains("attachment_handle")
                      ? BehaviorAttachmentHandle{
                            operation.at("attachment_handle").get<std::uint64_t>()}
                      : invalidBehaviorAttachmentHandle,
        .attachment_seq = operation.value("attachment_seq", std::uint64_t{}),
    };
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
    std::vector<BehaviorAttachmentEdit> behavior_edits;
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
            if (component == "behavior") {
                const auto object_id = AuthoringObjectId{
                    operation.at("object_id").get<std::uint64_t>()};
                if (const auto entity = boundEditorEntity(runtime_bindings, object_id)) {
                    const auto &authored = operation.at("authored_component");
                    const auto stable_name =
                        authored.at("type").get<std::string>();
                    const nlohmann::json params =
                        authored.contains("params")
                            ? nlohmann::json{authored.at("params")}
                            : nlohmann::json::object();
                    behavior_edits.push_back(BehaviorAttachmentEdit{
                        .kind = BehaviorAttachmentEditKind::set_params,
                        .entity = *entity,
                        .component_index = operation.at("attachment_index")
                                               .get<std::size_t>(),
                        .identity = behaviorIdentity(operation),
                        .stable_name = stable_name,
                        .canonical_params =
                            internal::canonicalizeBehaviorParams(stable_name,
                                                                 params),
                        .raw_component = authored,
                    });
                }
            } else if (component == "transform") transform_scenes.insert(scene_id);
            else if (component == "simplemodelview") renderer_scenes.insert(scene_id);
            else if (component == "animation" || component == "sprite_view")
                ecs_codec_scenes.insert(scene_id);
            else if (component == "camera") camera_scenes.insert(scene_id);
            else if (component == "light") light_scenes.insert(scene_id);
            else if (component == "collider") collider_scenes.insert(scene_id);
        } else if (op == "add_component" || op == "remove_component") {
            const auto component = operation.at("component_slot").get<std::string>();
            const auto object_id = AuthoringObjectId{
                operation.at("object_id").get<std::uint64_t>()};
            const auto entity = boundEditorEntity(runtime_bindings, object_id);
            const auto component_index = operation.at("component_index")
                                             .get<std::size_t>();
            if (component == "behavior") {
                if (entity) {
                    if (op == "add_component") {
                        const auto &authored = operation.at("component");
                        behavior_edits.push_back(BehaviorAttachmentEdit{
                            .kind = BehaviorAttachmentEditKind::attach,
                            .entity = *entity,
                            .component_index = component_index,
                            .identity = behaviorIdentity(operation),
                            .stable_name = authored.at("type").get<std::string>(),
                            .canonical_params = authored.at("params").dump(),
                            .raw_component = authored,
                        });
                    } else {
                        behavior_edits.push_back(BehaviorAttachmentEdit{
                            .kind = BehaviorAttachmentEditKind::remove,
                            .entity = *entity,
                            .component_index = operation.at("attachment_index")
                                                   .get<std::size_t>(),
                            .identity = behaviorIdentity(operation),
                        });
                    }
                }
                continue;
            }
            if (entity) {
                behavior_edits.push_back(BehaviorAttachmentEdit{
                    .kind = op == "add_component"
                                ? BehaviorAttachmentEditKind::insert_component
                                : BehaviorAttachmentEditKind::remove_component,
                    .entity = *entity,
                    .component_index = component_index,
                });
            }
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

    std::unique_ptr<BehaviorProjectionJunctionState> behavior_state;
    if (!behavior_edits.empty()) {
        behavior_state = std::make_unique<BehaviorProjectionJunctionState>(
            BehaviorProjectionJunctionState{
                .arena = modules.behavior_arena,
                .edits = std::move(behavior_edits),
            });
        auto junction = makeBehaviorAttachmentProjectionJunction(
            "behavior_attachment.batch", behavior_state.get(),
            &BehaviorProjectionJunctionState::prepare,
            &BehaviorProjectionJunctionState::publish,
            &BehaviorProjectionJunctionState::rollback,
            &BehaviorProjectionJunctionState::finish);
        adapters.push_back(
            std::make_unique<EditorProjectionCallbackAdapter>(std::move(junction)));
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
    bool reload_reconciling = false;
    if (modules.reload_service != nullptr) {
        const auto state = modules.reload_service->statusJson().value(
            "state", std::string{});
        reload_reconciling = state == "reconciling";
    }
    return internal::applyBehaviorEditConcurrencyGate(
        {.reasons = reasons,
         .transition_epoch = editorTransitionEpoch(modules)},
        internal::behaviorCallbackActive(), isGameLogicReloadInProgress(),
        reload_reconciling);
}

EditorRuntimeObjectState queryEditorRuntime(const EditorRuntimeModules &modules,
                                            std::span<const EditorProjectionRuntimeObjectBinding> bindings,
                                            const AuthoringSceneView &scene,
                                            const AuthoringObjectView &object) {
    EditorRuntimeObjectState result;
    result.component_runtime_json.resize(object.components.size());
    result.component_pending.resize(object.components.size(), false);
    result.behavior_attachments.resize(object.components.size());
    if (scene.scene_id != modules.scene_loader.currentScene()) return result;
    const auto entity_id = boundEditorEntity(bindings, object.authoring_object_id);
    if (!entity_id) return result;
    result.entity_id = entity_id;

    auto &ecs = modules.ecs_core.getTemplatePublicModule();
    auto &component_info = modules.component_info;
    const auto behavior_attachments = modules.behavior_arena.snapshot();
    for (std::size_t index = 0; index < object.components.size(); ++index) {
        const auto name = object.components[index].authoredJson().at("name").get<std::string>();
        if (name == "behavior") {
            const auto found = std::find_if(
                behavior_attachments.begin(), behavior_attachments.end(),
                [&](const BehaviorAttachmentInfo &attachment) {
                    return attachment.entity == *entity_id &&
                           attachment.component_index == index;
                });
            if (found != behavior_attachments.end()) {
                result.component_pending[index] = found->pending;
                result.behavior_attachments[index] =
                    EditorRuntimeBehaviorAttachmentState{
                        .handle = found->handle.value,
                        .attachment_seq = found->attachment_seq,
                        .owner = found->owner,
                        .owner_generation =
                            internal::registrationOwnerGeneration(found->owner),
                        .pending = found->pending,
                        .active = found->active,
                    };
                if (!found->pending) {
                    result.component_runtime_json[index] =
                        nlohmann::ordered_json{
                            {"name", "behavior"},
                            {"type", found->stable_name},
                            {"params", nlohmann::ordered_json::parse(
                                           found->canonical_params)},
                        };
                }
            }
            continue;
        }
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

    nlohmann::ordered_json previewSharedState() const {
        using Json = nlohmann::ordered_json;
        const auto vec3 = [](auto value) {
            return Json::array({value.x, value.y, value.z});
        };
        const auto matrix = [](const glm::mat4 &value) {
            auto result = Json::array();
            for (glm::length_t row = 0; row < 4; ++row) {
                auto values = Json::array();
                for (glm::length_t column = 0; column < 4; ++column) {
                    values.push_back(value[column][row]);
                }
                result.push_back(std::move(values));
            }
            return result;
        };

        const auto &document = modules.project_config.sceneDocument();
        auto runtime_objects = Json::array();
        for (const auto &scene : document.query()) {
            for (const auto &object : scene.objects) {
                const auto state = queryEditorRuntime(modules, runtime_bindings,
                                                      scene, object);
                auto components = Json::array();
                for (std::size_t index = 0;
                     index < state.component_runtime_json.size(); ++index) {
                    components.push_back({
                        {"index", index},
                        {"value", state.component_runtime_json[index]
                                      ? Json(*state.component_runtime_json[index])
                                      : Json(nullptr)},
                        {"pending", index < state.component_pending.size()
                                        ? state.component_pending[index] : false},
                    });
                }
                runtime_objects.push_back({
                    {"authoring_object_id", object.authoring_object_id.value},
                    {"entity", state.entity_id
                                   ? Json{{"index", state.entity_id->index},
                                          {"generation", state.entity_id->generation}}
                                   : Json(nullptr)},
                    {"components", std::move(components)},
                });
            }
        }

        const auto ecs_snapshot =
            modules.ecs_core.getTemplatePublicModule().isolationSnapshot();
        auto ecs_entity_slots = Json::array();
        for (std::size_t index = 0; index < ecs_snapshot.entity_slots.size(); ++index) {
            const auto &slot = ecs_snapshot.entity_slots[index];
            ecs_entity_slots.push_back({
                {"index", index}, {"live", slot.live},
                {"generation", slot.generation},
                {"chunk_index", slot.chunk_index ? Json(*slot.chunk_index) : Json(nullptr)},
                {"array_index", slot.array_index ? Json(*slot.array_index) : Json(nullptr)},
            });
        }
        auto ecs_chunks = Json::array();
        for (std::size_t index = 0; index < ecs_snapshot.chunks.size(); ++index) {
            const auto &chunk = ecs_snapshot.chunks[index];
            ecs_chunks.push_back({
                {"index", index}, {"count", chunk.count}, {"mask", chunk.mask},
                {"component_indices", chunk.component_indices},
                {"component_versions", chunk.component_versions},
            });
        }

        const auto camera = modules.camera.snapshotPrepared();
        const auto physics = modules.physics.snapshotPrepared();
        auto physics_bindings = Json::array();
        for (const auto &binding : physics.bindings) {
            physics_bindings.push_back({
                {"collider_id", binding.identity.collider_id.value},
                {"name", binding.identity.name},
                {"shape", binding.collider.shape},
                {"pos", Json::array({binding.collider.pos.x,
                                      binding.collider.pos.y,
                                      binding.collider.pos.z})},
                {"rotation", Json::array({binding.collider.rotation.x,
                                           binding.collider.rotation.y,
                                           binding.collider.rotation.z,
                                           binding.collider.rotation.w})},
                {"radius", binding.collider.radius},
                {"half_extents", Json::array({binding.collider.half_extents.x,
                                               binding.collider.half_extents.y,
                                               binding.collider.half_extents.z})},
                {"half_height", binding.collider.half_height},
                {"layer", binding.collider.layer}, {"mask", binding.collider.mask},
                {"trigger", binding.collider.trigger}, {"one_way", binding.collider.one_way},
            });
        }

        const auto lights = modules.lights.snapshotPrepared();
        auto light_state = Json{{"directional", Json::array()},
                                {"point", Json::array()},
                                {"spot", Json::array()}};
        for (const auto &light : lights.directional_lights) {
            light_state["directional"].push_back({
                {"name", light.name}, {"direction", vec3(light.direction)},
                {"intensity", light.intensity}, {"color", vec3(light.color)}});
        }
        for (const auto &light : lights.point_lights) {
            light_state["point"].push_back({
                {"name", light.name}, {"position", vec3(light.position)},
                {"intensity", light.intensity}, {"color", vec3(light.color)}});
        }
        for (const auto &light : lights.spot_lights) {
            light_state["spot"].push_back({
                {"name", light.name}, {"position", vec3(light.position)},
                {"direction", vec3(light.direction)}, {"intensity", light.intensity},
                {"inner", light.innerConeAngle}, {"outer", light.outerConeAngle},
                {"color", vec3(light.color)}});
        }

        auto behavior_state = Json::array();
        for (const auto &entry : modules.behavior_arena.snapshot()) {
            behavior_state.push_back({
                {"handle", entry.handle.value}, {"attachment_seq", entry.attachment_seq},
                {"entity", {{"index", entry.entity.index},
                            {"generation", entry.entity.generation}}},
                {"component_index", entry.component_index},
                {"stable_name", entry.stable_name},
                {"canonical_params", entry.canonical_params},
                {"owner", entry.owner}, {"pending", entry.pending},
                {"active", entry.active},
            });
        }
        const auto isolation = modules.behavior_arena.isolationState();
        const auto behavior_isolation = Json{
            {"next_handle", isolation.next_handle},
            {"next_attachment_seq", isolation.next_attachment_seq},
            {"deferred_mutation_count", isolation.deferred_mutation_count},
            {"callback_depth", isolation.callback_depth},
            {"callback_owner", isolation.callback_owner},
        };

        const auto event_snapshot =
            internal::getEventRegisterer().isolationSnapshot();
        const auto event_entries = [](const auto &entries) {
            auto result = Json::array();
            for (const auto &entry : entries) {
                result.push_back({
                    {"name", entry.name}, {"type_hash", entry.type_hash},
                    {"owner", entry.owner},
                    {"payload_identity", entry.payload_identity},
                });
            }
            return result;
        };

        const auto reload = modules.reload_gate
                                ? modules.reload_gate->snapshot()
                                : watch::ReloadGateSnapshot{};
        return {
            {"authored_semantic_bytes", document.rawJson().dump()},
            {"scene_revision", document.revision().value},
            {"ecs_values_identity", std::move(runtime_objects)},
            {"ecs_versions_tick_entity_free_list",
             {{"global_tick", ecs_snapshot.global_tick},
              {"free_indices", ecs_snapshot.free_indices},
              {"entity_slots", std::move(ecs_entity_slots)},
              {"chunks", std::move(ecs_chunks)},
              {"live_count", modules.ecs_core.getTemplatePublicModule().liveCount()}}},
            {"light", std::move(light_state)},
            {"phys", {{"next_collider_id", physics.next_collider_id_value},
                       {"bindings", std::move(physics_bindings)}}},
            {"renderer", modules.renderer.previewIsolationStateJson()},
            {"polygon", {{"instances", modules.polygon_instances.instanceCountForTesting()},
                          {"slots", modules.polygon_instances.slotCountForTesting()},
                          {"history_advance", modules.polygon_instances.temporalHistoryAdvanceCountForTesting()},
                          {"material_override_entries", modules.polygon_instances.materialOverrideStorageEntryCountForTesting()},
                          {"material_absolute_override_entries", modules.polygon_instances.materialAbsoluteOverrideStorageEntryCountForTesting()}}},
            {"camera", {{"position", vec3(camera.pos)}, {"direction", vec3(camera.dir)},
                        {"up", vec3(camera.up)}, {"projection", matrix(camera.projection_matrix)},
                        {"viewport", Json::array({camera.viewport_width, camera.viewport_height})},
                        {"active", camera.active_camera_name},
                        {"discontinuity_revision", camera.discontinuity_revision}}},
            {"deletion", modules.deletion_queue
                 ? Json{{"epoch", modules.deletion_queue->currentFrameForTesting()},
                        {"pending", modules.deletion_queue->pendingCountForTesting()}}
                 : Json(nullptr)},
            {"timing", modules.render_timing
                 ? Json{{"status", modules.render_timing->statusJson()},
                        {"pending", modules.render_timing->pendingRangeCountForTesting()},
                        {"pool_generation", modules.render_timing->queryPoolCreateCountForTesting()}}
                 : Json(nullptr)},
            {"behavior", {{"attachments", std::move(behavior_state)},
                           {"isolation", std::move(behavior_isolation)}}},
            {"event_trace", {{"pending", event_entries(event_snapshot.pending)},
                              {"deliver_now", event_entries(event_snapshot.deliver_now)},
                              {"payload_load_calls", event_snapshot.payload_load_calls},
                              {"catalog_frozen", event_snapshot.catalog_frozen}}},
            {"engine_time", {{"now", modules.engine_time.now()},
                             {"delta", modules.engine_time.dt()},
                             {"frame_index", modules.engine_time.frameIndex()},
                             {"set_revision", modules.engine_time.timeSetRevision()}}},
            {"reload_generation", {{"enabled", reload.enabled},
                                   {"epoch", reload.epoch}, {"reason", reload.reason},
                                   {"service", modules.reload_service
                                       ? Json(modules.reload_service->statusJson()) : Json(nullptr)}}},
        };
    }
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
            .allocate_behavior_attachment =
                [runtime](std::uint64_t commit_seq, std::size_t command_index,
                          std::size_t attachment_index) {
                    const auto identity = runtime->modules.behavior_arena
                                              .reserveRuntimeAttachmentIdentity(
                                                  commit_seq, command_index,
                                                  attachment_index);
                    return EditorBehaviorAttachmentIdentity{
                        .handle = identity.handle.value,
                        .attachment_seq = identity.attachment_seq,
                    };
                },
            .resolve_behavior_attachment =
                [runtime](AuthoringObjectId object_id,
                          std::size_t attachment_index)
                    -> std::optional<EditorBehaviorAttachmentIdentity> {
                    const auto entity = boundEditorEntity(
                        runtime->runtime_bindings, object_id);
                    if (!entity) return std::nullopt;
                    const auto attachment = runtime->modules.behavior_arena
                                                .findEditorAttachment(
                                                    *entity, attachment_index);
                    if (!attachment) return std::nullopt;
                    return EditorBehaviorAttachmentIdentity{
                        .handle = attachment->handle.value,
                        .attachment_seq = attachment->attachment_seq,
                    };
                },
            .install_commit_hook = true,
        },
        .preview = EditorPreviewServiceDependencies{
            .document = [runtime]() -> const AuthoringSceneDocument & {
                return runtime->modules.project_config.sceneDocument();
            },
            .preview_graph = [runtime]() -> const PreviewGraphProgram & {
                return runtime->modules.renderer.previewGraphProgram();
            },
            .xr_active = [runtime] { return runtime->modules.launch_config.xr_active; },
            .engine_time = [runtime] {
                return PreviewEngineTimeSnapshot{
                    .time = runtime->modules.engine_time.now(),
                    .delta = runtime->modules.engine_time.dt(),
                    .frame_index = runtime->modules.engine_time.frameIndex(),
                };
            },
            .shared_state_snapshot = [runtime] {
                return runtime->previewSharedState();
            },
        },
        .import_scene_snapshot =
            [runtime](std::string_view bytes,
                      std::string_view current_scene_id) {
                std::vector<EditorProjectionRuntimeObjectBinding>
                    next_runtime_bindings;
                const auto revision =
                    runtime->modules.project_config.importSceneDocument(
                        bytes, [&] {
                            runtime->modules.scene_loader.load(
                                std::string{current_scene_id});
                            next_runtime_bindings =
                                collectEditorRuntimeBindings(runtime->modules);
                        });
                runtime->runtime_bindings.swap(next_runtime_bindings);
                return revision;
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
