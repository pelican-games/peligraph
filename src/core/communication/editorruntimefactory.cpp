#define GLM_ENABLE_EXPERIMENTAL
#include "editorruntimefactory.hpp"
#include "../registrationownerhost.hpp"

#include "editorcommandservice.hpp"
#include "editorassetqueryruntime.hpp"
#include "renderconfigeditor.hpp"
#include "proceduralasseteditor.hpp"
#include "proceduralassetpublication.hpp"
#include "../renderer/proceduralgeometrycontainer.hpp"
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
#include "../loader/renderconfigcandidate.hpp"
#include "../loader/scene.hpp"
#include "../material/materialcontainer.hpp"
#include "../os/inputsequence.hpp"
#include "../phys/physworld.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/previewgraph.hpp"
#include "../renderingpass/subgraphreplacementregistry.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../renderer/viewfamilyproviderregistry.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../userpublic/details/event/registerer.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/rendertiming.hpp"
#include "../watch/reloadgate.hpp"
#include "../watch/reloadservice.hpp"
#include "../../project/featurecompose.hpp"
#include "../../project/renderfeatureoverlay.hpp"

#include <algorithm>
#include <any>
#include <array>
#include <concepts>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Pelican {

std::uint64_t internal::EditorTransitionCounter::observe(
    std::string_view current_scene,
    const watch::ReloadTransitionFacts *facts) {
    const bool changed =
        observed_ &&
        (scene_ != current_scene || facts_.has_value() != (facts != nullptr) ||
         (facts != nullptr && !(*facts_ == *facts)));
    if (!observed_ || changed) {
        observed_ = true;
        scene_ = std::string{current_scene};
        if (facts != nullptr) {
            facts_ = *facts;
        } else {
            facts_.reset();
        }
    }
    if (changed) ++value_;
    return value_;
}

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

std::vector<internal::EditorPrefabProjectionRoute>
internal::planEditorPrefabProjectionRoutes(
    std::span<const nlohmann::ordered_json> operations,
    const ResolvedScene &resolved, std::string_view current_scene,
    BehaviorRegistryAvailability behavior_availability) {
    std::vector<EditorPrefabProjectionRoute> result;
    for (const auto &operation : operations) {
        const auto op = operation.value("op", std::string{});
        const bool parameter = op == "set_prefab_parameter" ||
                               op == "unset_prefab_parameter";
        const bool unpack = op == "unpack_prefab" ||
                            op == "restore_prefab";
        if (!parameter && !unpack) continue;
        const auto object_id = AuthoringObjectId{
            operation.at("object").get<std::uint64_t>()};
        std::string scene_id;
        for (const auto &scene : resolved.scenes()) {
            if (std::any_of(scene.objects.begin(), scene.objects.end(),
                            [object_id](const ResolvedObject &object) {
                                return object.authoring_object_id == object_id;
                            })) {
                scene_id = scene.scene_id;
                break;
            }
        }
        if (scene_id.empty()) {
            throw std::runtime_error(
                "prefab edit target is absent from resolved scene");
        }
        if (scene_id != current_scene) continue;
        EditorPrefabProjectionRoute route{
            .scene_id = scene_id,
            .full_component_projection = parameter,
            .behavior_availability = behavior_availability,
        };
        if (unpack) {
            route.origin_transition = originTransitionMapFromJson(
                operation.at("origin_transition"));
        }
        result.push_back(std::move(route));
    }
    return result;
}

std::vector<std::unique_ptr<EditorProjectionAdapter>>
internal::makeEditorPrefabBehaviorProjectionAdapters(
    BehaviorAttachmentArena &arena,
    const std::vector<EditorProjectionRuntimeObjectBinding> &runtime_bindings,
    std::span<const EditorPrefabProjectionRoute> routes,
    BehaviorRegistryAvailability fallback_availability) {
    std::set<std::string> prefab_scenes;
    std::unordered_map<std::string, OriginTransitionMap>
        prefab_origin_transitions;
    std::unordered_map<std::string, BehaviorRegistryAvailability>
        prefab_behavior_availabilities;
    for (const auto &route : routes) {
        prefab_scenes.insert(route.scene_id);
        prefab_behavior_availabilities.insert_or_assign(
            route.scene_id, route.behavior_availability);
        if (!route.origin_transition.entries.empty()) {
            prefab_origin_transitions.insert_or_assign(
                route.scene_id, route.origin_transition);
        }
    }

    std::vector<std::unique_ptr<EditorProjectionAdapter>> adapters;
    adapters.reserve(prefab_scenes.size());
    for (const auto &scene_id : prefab_scenes) {
        const auto transition = prefab_origin_transitions.find(scene_id);
        const auto availability =
            prefab_behavior_availabilities.find(scene_id);
        adapters.push_back(std::make_unique<BehaviorSceneProjectionAdapter>(
            arena, scene_id, runtime_bindings,
            std::vector<SceneObjectProjectionAdapter *>{},
            transition == prefab_origin_transitions.end()
                ? OriginTransitionMap{}
                : transition->second,
            availability == prefab_behavior_availabilities.end()
                ? fallback_availability
                : availability->second));
    }
    return adapters;
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

EditorProjectionModules editorProjectionModules(EditorRuntimeModules &modules) {
    return EditorProjectionModules{
        .ecs = &modules.ecs_core.getTemplatePublicModule(),
        .component_info = &modules.component_info,
        .models = &modules.models,
        .polygon_instances = &modules.polygon_instances,
        .camera = &modules.camera,
        .lights = &modules.lights,
        .physics = &modules.physics,
        .behavior_arena = &modules.behavior_arena,
        .scene_loader = &modules.scene_loader,
    };
}

std::vector<EditorProjectionRuntimeObjectBinding>
collectEditorRuntimeBindings(const EditorRuntimeModules &modules) {
    std::vector<EditorProjectionRuntimeObjectBinding> result;
    const auto loaded = modules.scene_loader.runtimeObjectBindings();
    result.reserve(loaded.size());
    for (const auto &binding : loaded) {
        if (binding.authoring_object_id.value == 0 ||
            binding.object_id == invalidGameObjectId) {
            throw std::logic_error(
                "scene loader exposed an incomplete editor runtime binding");
        }
        result.push_back(
            {binding.authoring_object_id, binding.object_id});
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

// A full prefab projection names every participant up front, even when the
// edited object has no component owned by that participant.  A disconnected
// participant is therefore represented by an explicit guard route: it is a
// no-op only when the targeted object is irrelevant in both the old and next
// resolved scenes, and otherwise fails during prepare with the participant's
// stable name.  Direct set routes always have a relevant component and take
// the same failure path.
class MissingProjectionParticipantGuard final
    : public EditorProjectionAdapter {
    EditorProjectionAdapterKind kind_;
    std::string participant_;
    std::string scene_id_;
    AuthoringObjectId object_id_{};
    std::string component_;
    std::string name_;

    bool relevant(const ResolvedScene &resolved) const {
        const auto *scene = resolved.findScene(scene_id_);
        if (scene == nullptr) return false;
        const auto object = std::find_if(
            scene->objects.begin(), scene->objects.end(),
            [&](const ResolvedObject &candidate) {
                return candidate.authoring_object_id == object_id_;
            });
        if (object == scene->objects.end()) return false;
        return std::any_of(
            object->components.begin(), object->components.end(),
            [&](const ResolvedComponent &candidate) {
                return candidate.name == component_;
            });
    }

  public:
    MissingProjectionParticipantGuard(
        EditorProjectionAdapterKind kind, std::string participant,
        std::string scene_id, AuthoringObjectId object_id,
        std::string component)
        : kind_{kind}, participant_{std::move(participant)},
          scene_id_{std::move(scene_id)}, object_id_{object_id},
          component_{std::move(component)},
          name_{"participant_guard." + participant_ + "." +
                std::to_string(object_id_.value)} {}

    EditorProjectionAdapterKind kind() const noexcept override {
        return kind_;
    }
    std::string_view name() const noexcept override { return name_; }
    EditorProjectionPublicationMode publicationMode() const noexcept override {
        return EditorProjectionPublicationMode::StagedNoexcept;
    }
    void prepare(const EditorProjectionPrepareContext &context) override {
        if (relevant(context.base_resolved) ||
            relevant(context.next_resolved)) {
            throw std::runtime_error(
                "editor_projection_participant_missing@1: " +
                participant_);
        }
    }
    void publish() noexcept override {}
    void rollback() noexcept override {}
    void finish() noexcept override {}
};

class SimpleModelMigrationParticipant final
    : public ECSArchetypeMigrationAdapter {
    ModelAssetContainer &models_;
    PolygonInstanceContainer &renderer_;
    ECSArchetypeMigrationKind original_kind_;
    std::optional<StagedModelInstance> staged_;
    std::optional<ModelInstanceId> next_id_;
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
            renderer_.setStagedAuthoringObject(*staged_, component->authoring_object_id);
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
            component->dirty = 0;
        }
    }

    void rollback(const ECSArchetypeMigrationPrepareContext &) noexcept override {
        staged_.reset();
        next_id_.reset();
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
        staged_.reset();
        next_id_.reset();
        published_ = false;
    }
};

class EditorEcsMigrationProjectionAdapter final
    : public EditorProjectionAdapter {
    ECSCoreTemplatePublic &ecs_;
    ComponentInfoManager &components_;
    ModelAssetContainer *models_ = nullptr;
    PolygonInstanceContainer *renderer_ = nullptr;
    ComponentCodec codec_;
    std::vector<EditorProjectionRuntimeObjectBinding> bindings_;
    AuthoringObjectId object_id_{};
    std::string component_name_;
    nlohmann::ordered_json operation_;
    ECSArchetypeMigrationKind kind_ = ECSArchetypeMigrationKind::add;
    std::string name_;
    std::optional<SimpleModelMigrationParticipant> simple_model_;
    std::optional<ECSArchetypeMigrationToken> token_;

  public:
    EditorEcsMigrationProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        ModelAssetContainer *models, PolygonInstanceContainer *renderer,
        ComponentCodec codec,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        nlohmann::ordered_json operation, std::size_t operation_index)
        : ecs_{ecs}, components_{components}, models_{models},
          renderer_{renderer}, codec_{codec}, bindings_{std::move(bindings)},
          object_id_{operation.at("object_id").get<std::uint64_t>()},
          component_name_{operation.at("component_slot").get<std::string>()},
          operation_(std::move(operation)),
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
        if (codec_.editor_policy == EditorProjectionPolicy::RendererModel) {
            if (models_ == nullptr || renderer_ == nullptr) {
                throw std::runtime_error(
                    "editor_projection_participant_missing@1: renderer");
            }
            simple_model_.emplace(*models_, *renderer_, kind_);
            participants.push_back(&*simple_model_);
        }
        if (kind_ == ECSArchetypeMigrationKind::add) {
            ResolutionResult<ComponentCodecValue> decoded_result = [&] {
                try {
                    return codec_.decodeAuthored(
                        operation_.at("component"));
                } catch (const LegacyRejectException &error) {
                    throw ResolutionFailureException{
                        StoredResolutionFailure{LegacyResolutionText{
                            .legacy_branch_id = error.legacyBranchId(),
                            .message = error.what(),
                        }}};
                }
            }();
            if (!decoded_result) {
                throw ResolutionFailureException{
                    StoredResolutionFailure{
                        std::move(decoded_result).error()}};
            }
            auto decoded = std::move(decoded_result).value();
            auto canonical = codec_.encodeCanonical(decoded);
            token_.emplace(ECSArchetypeMigration::prepareAdd(
                ecs_, *entity, component_id,
                [this, codec = &codec_,
                 decoded = std::move(decoded),
                 canonical = std::move(canonical)](void *target) {
                    if (codec->editor_policy ==
                        EditorProjectionPolicy::TransformClosure) {
                        TransformCodecTarget transform{
                            .world = static_cast<TransformComponent *>(target)};
                        codec->applyRuntime(decoded, &transform);
                    } else if (codec->runtime_kind ==
                               ComponentCodecRuntimeKind::Ecs) {
                        codec->applyRuntime(decoded, target);
                        if (codec->editor_policy ==
                            EditorProjectionPolicy::RendererModel) {
                            static_cast<SimpleModelViewComponent *>(target)
                                ->authoring_object_id = object_id_.value;
                        }
                    } else {
                        components_.loadByJson(target, canonical);
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

        for (const auto &scene : context.next_resolved.scenes()) {
            if (scenes_.contains(scene.scene_id)) {
                std::unordered_map<std::string, const ResolvedObject *>
                    by_name;
                for (const auto &object : scene.objects) {
                    if (object.name) by_name.emplace(*object.name, &object);
                }
                std::unordered_map<std::uint64_t, TransformComponent> worlds;
                std::unordered_set<std::uint64_t> visiting;
                const auto compute_world = [&](auto &&self,
                                               const ResolvedObject &object)
                    -> const TransformComponent & {
                    if (const auto found =
                            worlds.find(object.authoring_object_id.value);
                        found != worlds.end()) {
                        return found->second;
                    }
                    if (!visiting.insert(object.authoring_object_id.value)
                             .second) {
                        throw std::runtime_error(
                            "validated transform hierarchy became cyclic");
                    }
                    const auto component = std::find_if(
                        object.components.begin(), object.components.end(),
                        [](const auto &candidate) {
                            return candidate.name == "transform";
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
                    component->runtime_codec->applyRuntime(
                        component->requireRuntimeValue(), &target);
                    visiting.erase(object.authoring_object_id.value);
                    return worlds
                        .emplace(object.authoring_object_id.value, next)
                        .first->second;
                };

                for (const auto &binding : bindings_) {
                    auto *world = ecs_.tryComponent<TransformComponent>(
                        binding.entity);
                    const auto *local =
                        ecs_.tryComponent<LocalTransformComponent>(
                            binding.entity);
                    if (world != nullptr && local == nullptr) {
                        const auto object = std::find_if(
                            scene.objects.begin(), scene.objects.end(),
                            [&](const auto &candidate) {
                                return candidate.authoring_object_id ==
                                       binding.authoring_object_id;
                            });
                        if (object == scene.objects.end()) {
                            throw std::runtime_error(
                                "standalone transform runtime binding has no authoring object");
                        }
                        prepared_.push_back(ecs_.prepareComponentSwap(
                            binding.entity,
                            compute_world(compute_world, *object)));
                    }
                }
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

        const ResolvedObject *object = nullptr;
        for (const auto &scene : context.next_resolved.scenes()) {
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
                return candidate.name == "transform";
            });
        if (transform == object->components.end()) {
            throw std::runtime_error("reparent target has no transform");
        }
        const auto &data =
            std::any_cast<const TransformCodecData &>(
                transform->requireRuntimeValue());
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
    ECSCoreTemplatePublic &ecs, const SceneProjectionState &projection_state,
    std::span<const EditorProjectionRuntimeObjectBinding> runtime_bindings) {
    std::vector<TransformProjectionBinding> result;
    const auto scenes = projection_state.resolved().scenes();
    for (const auto &binding : runtime_bindings) {
        const ResolvedSceneView *bound_scene = nullptr;
        const ResolvedObject *bound_object = nullptr;
        for (const auto &scene : scenes) {
            const auto object = std::find_if(scene.objects.begin(), scene.objects.end(),
                                             [&](const auto &candidate) {
                                                 return candidate.authoring_object_id ==
                                                        binding.authoring_object_id;
                                             });
            if (object != scene.objects.end()) {
                bound_scene = &scene;
                bound_object = &*object;
                break;
            }
        }
        if (bound_scene == nullptr || bound_object == nullptr) {
            throw std::runtime_error(
                "runtime binding has no authoring object");
        }
        auto *world = ecs.tryComponent<TransformComponent>(binding.entity);
        auto *local = ecs.tryComponent<LocalTransformComponent>(binding.entity);
        if (world != nullptr && local != nullptr) {
            result.push_back({bound_scene->scene_id,
                              bound_object->authoring_object_id,
                              binding.entity, world, local});
        }
    }
    return result;
}

struct BehaviorProjectionJunctionState {
    BehaviorAttachmentArena &arena;
    std::vector<BehaviorAttachmentEdit> edits;
    BehaviorRegistryAvailability availability =
        BehaviorRegistryAvailability::active;
    std::unique_ptr<PreparedBehaviorAttachmentEdits> prepared;

    static void prepare(void *context,
                        const EditorProjectionPrepareContext &) {
        auto &state = *static_cast<BehaviorProjectionJunctionState *>(context);
        state.prepared = state.arena.prepareEditorEdits(
            state.edits, state.availability);
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
    SceneProjectionState state_;

  public:
    explicit EphemeralEditorProjectionTarget(
        const SceneProjectionState &source)
        : state_{source} {}

    const SceneProjectionState &projectionState() const override {
        return state_;
    }
    SceneRevision nextProjectionRevision() const override {
        return SceneRevision{state_.revision().value + 1U};
    }
    SceneResolverGeneration nextProjectionResolverGeneration() const override {
        return SceneResolverGeneration{
            state_.resolverGeneration().value + 1U};
    }
    void publishProjectionState(
        SceneProjectionState &candidate) noexcept override {
        state_.swap(candidate);
    }
};

EditorGateObservation editorGateObservation(
    const EditorRuntimeModules &modules,
    internal::EditorTransitionCounter &transitions) {
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
    std::optional<watch::ReloadTransitionFacts> facts;
    if (modules.reload_service != nullptr) {
        facts = modules.reload_service->transitionFacts();
    }
    const bool reload_reconciling =
        facts.has_value() && facts->state == "reconciling";
    const auto transition_epoch = transitions.observe(
        modules.scene_loader.currentScene(),
        facts.has_value() ? &*facts : nullptr);
    return internal::applyBehaviorEditConcurrencyGate(
        {.reasons = reasons, .transition_epoch = transition_epoch},
        internal::behaviorCallbackActive(), isGameLogicReloadInProgress(),
        reload_reconciling);
}

std::vector<std::string> editorGateReasonNamesForRenderConfig(
    std::uint32_t bits) {
    std::vector<std::string> result;
    const std::array reasons{
        std::pair{EditorGateReason::replay, "replay"},
        std::pair{EditorGateReason::golden, "golden"},
        std::pair{EditorGateReason::strict, "strict"},
        std::pair{EditorGateReason::reload_scene_transition,
                  "reload_scene_transition"},
        std::pair{EditorGateReason::preview_lease_conflict,
                  "preview_lease_conflict"},
    };
    for (const auto &[reason, name] : reasons) {
        if ((bits & editorGateReasonBit(reason)) != 0) {
            result.emplace_back(name);
        }
    }
    return result;
}

EditorRuntimeObjectState queryEditorRuntime(const EditorRuntimeModules &modules,
                                            std::span<const EditorProjectionRuntimeObjectBinding> bindings,
                                            const ResolvedSceneView &scene,
                                            const ResolvedObject &object) {
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
        const auto &name = object.components[index].name;
        if (name == "behavior") {
            const auto found = std::find_if(
                behavior_attachments.begin(), behavior_attachments.end(),
                [&](const BehaviorAttachmentInfo &attachment) {
                    return attachment.entity == *entity_id &&
                           attachment.component_index ==
                               object.components[index]
                                   .merged_component_index;
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
        const auto *codec = object.components[index].runtime_codec;
        if (codec == nullptr) continue;
        if (codec->runtime_kind != ComponentCodecRuntimeKind::Ecs) {
            // Camera, Light, and Collider expose the exact resolver-completed
            // value used to prepare their production adapters.  This remains
            // absent from parent wire fixtures whose runtime_query is not
            // connected, preserving those bytes.
            result.component_runtime_json[index] =
                object.components[index].effective_json;
            continue;
        }
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

std::vector<EditorAssetQueryResult> collectEditorAssets(const EditorRuntimeModules &modules) {
    return collectEditorAssets(modules.project_config.assetDataJson(), modules.path_resolver);
}

struct EditorRuntimeState {
    EditorRuntimeModules modules;
    EditorRuntimeBindingState binding_state;
    // Shared by every editor service's gate so all of them see one
    // transition epoch.
    internal::EditorTransitionCounter transitions;

    EditorRuntimeState()
        : modules{resolveEditorRuntimeModules()},
          binding_state{
              .bindings = collectEditorRuntimeBindings(modules),
              .epoch = modules.scene_loader.runtimeSceneEpoch(),
          } {}

    void refreshRuntimeBindings() {
        const auto scene_epoch = modules.scene_loader.runtimeSceneEpoch();
        if (scene_epoch == binding_state.epoch) return;
        auto next = collectEditorRuntimeBindings(modules);
        binding_state.bindings.swap(next);
        binding_state.epoch = scene_epoch;
    }

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

        const auto &document = modules.project_config.resolvedScene();
        auto runtime_objects = Json::array();
        for (const auto &scene : document.scenes()) {
            for (const auto &object : scene.objects) {
                const auto state = queryEditorRuntime(modules, binding_state.bindings,
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
            {"authored_semantic_bytes",
             modules.project_config.sceneDataJson()},
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
                             {"frame_index", modules.engine_time.evaluationIndex()},
                             {"set_revision", modules.engine_time.timeSetRevision()}}},
            {"reload_generation", {{"enabled", reload.enabled},
                                   {"epoch", reload.epoch}, {"reason", reload.reason},
                                   {"service", modules.reload_service
                                       ? Json(modules.reload_service->statusJson()) : Json(nullptr)}}},
        };
    }
};

} // namespace

EditorProjectionContext makeEditorProjectionContext(
    std::string active_scene_id, EditorProjectionMode mode) {
    EditorLayoutSemanticMode layout_mode =
        EditorLayoutSemanticMode::Unconfigured;
    if (const auto *layout =
            FastModuleContainer::tryGet<RayLayoutRuntimeState>()) {
        if (!layout->configured()) {
            layout_mode = EditorLayoutSemanticMode::Unconfigured;
        } else {
            switch (layout->mode()) {
            case RayLayoutSemanticMode::LegacyNoLayout:
                layout_mode = EditorLayoutSemanticMode::Legacy;
                break;
            case RayLayoutSemanticMode::LayoutV3:
                layout_mode = EditorLayoutSemanticMode::Layout;
                break;
            }
        }
    }

    const auto available_codecs = componentCodecs();
    std::vector<ComponentCodec> codecs{available_codecs.begin(),
                                       available_codecs.end()};
    const auto defaults = componentCodecCanonicalDefaults();
    const auto layout_generation = editorProjectionCodecGeneration(
        codecs, layout_mode, defaults);

    std::vector<EditorBehaviorPolicy> registry;
    const auto &registrations =
        internal::getBehaviorRegisterer().registeredBehaviors();
    registry.reserve(registrations.size());
    for (const auto &registration : registrations) {
        registry.push_back(EditorBehaviorPolicy{
            .stable_name = registration.stable_name,
            .schema_version = registration.schema_version,
            .canonicalize = registration.canonicalize_params,
            .owner_generation =
                internal::registrationOwnerGeneration(registration.owner),
        });
    }
    const auto availability = configuredGameLogicStatus().loaded
                                  ? EditorBehaviorAvailability::Active
                                  : EditorBehaviorAvailability::DllUnavailable;
    const auto behavior_generation = editorProjectionBehaviorGeneration(
        registry, availability);
    return EditorProjectionContext{
        std::move(active_scene_id), mode, std::move(codecs), layout_mode,
        layout_generation, availability, std::move(registry),
        behavior_generation, defaults};
}

namespace {

EditorProjectionResult rejectProjectionPlanFailure(
    ProjectionPlanFailure failure, SceneRevision base_revision,
    SceneRevision committed_revision) {
    EditorProjectionError error;
    error.code = EditorProjectionErrorCode::CommandInvalid;
    error.object_path = "/";
    if (auto *recoverable =
            std::get_if<EditorProjectionRecoverable>(&failure)) {
        error.message = recoverable->name + ": " + recoverable->message;
    } else {
        auto violation =
            std::get<UserDataViolation>(std::move(failure));
        error.message = violation.leaf_message;
        error.data = nlohmann::ordered_json{
            {"resolution_error",
             resolutionFailureJson(
                 StoredResolutionFailure{std::move(violation)})}};
    }
    return EditorProjectionResult{
        .status = EditorProjectionStatus::Rejected,
        .base_revision = base_revision,
        .committed_revision = committed_revision,
        .error = std::move(error),
    };
}

} // namespace

EditorProjectionResult materializeEditorProjection(
    const EditorProjectionPlan &plan, const EditorProjectionModules &modules,
    EditorProjectionDocumentTarget &target,
    EditorRuntimeBindingState &binding_state,
    const EditorProjectionExpectation &expected,
    EditorProjectionFaultInjector *fault_injector) {
    const auto reject = [&](EditorProjectionStatus status,
                            EditorProjectionErrorCode code,
                            std::string message,
                            std::string object_path = "/",
                            nlohmann::ordered_json data =
                                nlohmann::ordered_json::object()) {
        return EditorProjectionResult{
            .status = status,
            .base_revision = plan.base_revision,
            .committed_revision = target.projectionState().revision(),
            .error = EditorProjectionError{
                .code = code,
                .object_path = std::move(object_path),
                .message = std::move(message),
                .data = std::move(data),
            },
        };
    };
    try {
        if (binding_state.epoch != expected.binding_epoch ||
            binding_state.codec_generation != expected.codec_generation ||
            binding_state.behavior_generation != expected.behavior_generation ||
            plan.context.layoutGeneration() != expected.codec_generation ||
            plan.context.behaviorGeneration() != expected.behavior_generation) {
            return reject(EditorProjectionStatus::Rejected,
                          EditorProjectionErrorCode::CommandInvalid,
                          "editor_projection_stale_bindings@1: runtime binding or policy generation changed");
        }
        // Registry validation is deliberately independent of runtime entity
        // binding. In particular, a document-only inactive-scene replay and
        // an active light-only object both observe the same active registry
        // before either path can decide that no ECS binding exists.
        if (plan.context.behaviorAvailability() ==
            EditorBehaviorAvailability::Active) {
            for (const auto &operation : plan.operations) {
                const auto unknown = std::visit(
                    [&](const auto &typed)
                        -> std::optional<std::pair<std::string,
                                                   AuthoringObjectId>> {
                        using Operation =
                            std::remove_cvref_t<decltype(typed)>;
                        if constexpr (
                            std::same_as<Operation,
                                         EditorSetComponentValueOperation> ||
                            std::same_as<Operation,
                                         EditorAddComponentOperation>) {
                            if (typed.component != "behavior") {
                                return std::nullopt;
                            }
                            const auto &authored = typed.wire.at(
                                std::same_as<
                                    Operation,
                                    EditorSetComponentValueOperation>
                                    ? "authored_component"
                                    : "component");
                            const auto stable_name =
                                authored.at("type").get<std::string>();
                            if (plan.context.findBehavior(stable_name) ==
                                nullptr) {
                                return std::pair{stable_name,
                                                 typed.object_id};
                            }
                        } else if constexpr (
                            std::same_as<Operation,
                                         EditorRemoveComponentOperation> ||
                            std::same_as<Operation, EditorReparentOperation> ||
                            std::same_as<Operation, EditorSpawnOperation> ||
                            std::same_as<Operation, EditorDestroyOperation> ||
                            std::same_as<Operation,
                                         EditorRemoveObjectsOperation> ||
                            std::same_as<Operation,
                                         EditorRestoreObjectsOperation> ||
                            std::same_as<Operation,
                                         EditorDuplicateOperation> ||
                            std::same_as<Operation,
                                         EditorSetPrefabParameterOperation> ||
                            std::same_as<Operation,
                                         EditorUnsetPrefabParameterOperation> ||
                            std::same_as<Operation,
                                         EditorUnpackPrefabOperation> ||
                            std::same_as<Operation,
                                         EditorRestorePrefabOperation>) {
                            // No authored behaviour body to name a stable type
                            // with: removal carries only the occurrence, and
                            // the object/prefab lifecycle operations reach the
                            // registry through their own lifecycle routes.
                        } else {
                            static_assert(
                                editorCanonicalOperationUnhandled<Operation>,
                                "unhandled EditorCanonicalOperation alternative");
                        }
                        return std::nullopt;
                    },
                    operation);
                if (unknown) {
                    return reject(
                        EditorProjectionStatus::Rejected,
                        EditorProjectionErrorCode::BehaviorTypeUnknown,
                        "Unknown behavior type '" + unknown->first + "'",
                        "/authoring_objects/" +
                            std::to_string(unknown->second.value) +
                            "/components/behavior",
                        nlohmann::ordered_json{
                            {"type", unknown->first},
                            {"object_id", unknown->second.value}});
                }
            }
        }
        const auto has_runtime_route = std::any_of(
            plan.routes.begin(), plan.routes.end(),
            [](const EditorProjectionRoute &route) {
                return route.kind != EditorProjectionRouteKind::DocumentOnly;
            });
        if (has_runtime_route &&
            (modules.ecs == nullptr || modules.component_info == nullptr)) {
            return reject(EditorProjectionStatus::Failed,
                          EditorProjectionErrorCode::AdapterPrepareFailed,
                          "editor_projection_module_missing@1: ECS projection modules are unavailable");
        }

        for (const auto &route : plan.routes) {
            if (route.kind != EditorProjectionRouteKind::BehaviorBatch) {
                continue;
            }
            if (!boundEditorEntity(binding_state.bindings,
                                   route.object_id)) {
                return reject(
                    EditorProjectionStatus::Rejected,
                    EditorProjectionErrorCode::RuntimeBindingMissing,
                    "runtime_binding_missing@1: active scene behavior target has no runtime entity binding",
                    "/authoring_objects/" +
                        std::to_string(route.object_id.value) +
                        "/components/behavior",
                    nlohmann::ordered_json{
                        {"object_id", route.object_id.value},
                        {"scene_id", route.scene_id}});
            }
        }

        auto runtime_bindings = binding_state.bindings;
        std::vector<std::unique_ptr<EditorProjectionAdapter>> adapters;
        std::vector<BehaviorAttachmentEdit> behavior_edits;
        struct LifecycleUpdate {
            AuthoringObjectId object_id{};
            std::string scene_id;
            SceneObjectProjectionAdapter *adapter = nullptr;
        };
        std::vector<LifecycleUpdate> lifecycle_updates;
        std::set<std::string> transform_scenes;
        std::set<std::string> ecs_codec_scenes;
        std::set<std::string> renderer_scenes;
        std::set<std::string> camera_scenes;
        std::set<std::string> light_scenes;
        std::set<std::string> collider_scenes;
        std::vector<EditorProjectionRoute> renderer_routes;
        std::vector<EditorProjectionRoute> camera_routes;
        std::vector<EditorProjectionRoute> light_routes;
        std::vector<EditorProjectionRoute> collider_routes;

        const auto hasRoute = [&](EditorProjectionRouteKind kind,
                                  std::size_t operation_index,
                                  AuthoringObjectId object_id) {
            return std::any_of(
                plan.routes.begin(), plan.routes.end(),
                [&](const EditorProjectionRoute &route) {
                    return route.kind == kind &&
                           route.operation_index == operation_index &&
                           route.object_id == object_id;
                });
        };
        const auto requireModule = [&](const void *module,
                                       std::string_view name) {
            if (module == nullptr) {
                throw std::runtime_error(
                    "editor_projection_module_missing@1: " +
                    std::string{name});
            }
        };
        const auto wireAt = [&](std::size_t operation_index)
            -> const nlohmann::ordered_json & {
            return std::visit(
                [](const auto &operation)
                    -> const nlohmann::ordered_json & {
                    return operation.wire;
                },
                plan.operations.at(operation_index));
        };
        const auto behavior_availability =
            plan.context.behaviorAvailability() ==
                    EditorBehaviorAvailability::Active
                ? BehaviorRegistryAvailability::active
                : BehaviorRegistryAvailability::dll_unavailable;
        const auto canonicalBehaviorParams =
            [&](const nlohmann::ordered_json &authored,
                AuthoringObjectId) {
                const auto stable_name =
                    authored.at("type").get<std::string>();
                const auto *policy =
                    plan.context.findBehavior(stable_name);
                if (policy == nullptr) {
                    return std::string{};
                }
                if (policy->canonicalize == nullptr) {
                    throw std::logic_error(
                        "behavior policy has no canonicalization hook: " +
                        stable_name);
                }
                const nlohmann::json params =
                    authored.contains("params")
                        ? nlohmann::json(authored.at("params"))
                        : nlohmann::json::object();
                return policy->canonicalize(params);
            };

        for (const auto &route : plan.routes) {
            // The ray plan also depends on declarations, transforms and
            // object lifecycle, not only on model component replacement.
            if (route.kind != EditorProjectionRouteKind::DocumentOnly)
                renderer_scenes.insert(route.scene_id);
            switch (route.kind) {
            case EditorProjectionRouteKind::DocumentOnly:
                break;
            case EditorProjectionRouteKind::TransformClosure:
                transform_scenes.insert(route.scene_id);
                break;
            case EditorProjectionRouteKind::RendererModel:
                renderer_scenes.insert(route.scene_id);
                renderer_routes.push_back(route);
                break;
            case EditorProjectionRouteKind::EcsCodec:
                ecs_codec_scenes.insert(route.scene_id);
                break;
            case EditorProjectionRouteKind::Camera:
                camera_scenes.insert(route.scene_id);
                camera_routes.push_back(route);
                break;
            case EditorProjectionRouteKind::Light:
                light_scenes.insert(route.scene_id);
                light_routes.push_back(route);
                break;
            case EditorProjectionRouteKind::Collider:
                collider_scenes.insert(route.scene_id);
                collider_routes.push_back(route);
                break;
            case EditorProjectionRouteKind::Reparent:
                adapters.push_back(
                    std::make_unique<ReparentLocalTransformProjectionAdapter>(
                        *modules.ecs, *modules.component_info,
                        runtime_bindings, wireAt(route.operation_index),
                        route.operation_index));
                break;
            case EditorProjectionRouteKind::ComponentLifecycle: {
                if (!route.codec_index ||
                    *route.codec_index >= plan.context.codecCatalog().size()) {
                    throw std::logic_error(
                        "component lifecycle route lost its codec descriptor");
                }
                const auto &codec =
                    plan.context.codecCatalog()[*route.codec_index];
                if (codec.runtime_kind == ComponentCodecRuntimeKind::Ecs ||
                    codec.runtime_kind == ComponentCodecRuntimeKind::Camera) {
                    adapters.push_back(std::make_unique<
                        EditorEcsMigrationProjectionAdapter>(
                        *modules.ecs, *modules.component_info,
                        modules.models, modules.polygon_instances, codec,
                        runtime_bindings, wireAt(route.operation_index),
                        route.operation_index));
                }
                break;
            }
            case EditorProjectionRouteKind::ObjectLifecycle: {
                const SceneObjectProjectionParticipants participants{
                    .renderer = hasRoute(
                        EditorProjectionRouteKind::LifecycleRenderer,
                        route.operation_index, route.object_id),
                    .camera = hasRoute(
                        EditorProjectionRouteKind::LifecycleCamera,
                        route.operation_index, route.object_id),
                    .light = hasRoute(
                        EditorProjectionRouteKind::LifecycleLight,
                        route.operation_index, route.object_id),
                    .collider = hasRoute(
                        EditorProjectionRouteKind::LifecycleCollider,
                        route.operation_index, route.object_id),
                };
                auto adapter =
                    std::make_unique<SceneObjectProjectionAdapter>(
                        *modules.ecs, *modules.component_info,
                        modules.models, modules.polygon_instances,
                        modules.camera, modules.lights, modules.physics,
                        route.scene_id, route.object_id, runtime_bindings,
                        participants);
                if (modules.scene_loader) adapter->connectRuntimeBindings(*modules.scene_loader);
                auto *raw = adapter.get();
                adapters.push_back(std::move(adapter));
                lifecycle_updates.push_back(
                    {route.object_id, route.scene_id, raw});
                break;
            }
            case EditorProjectionRouteKind::BehaviorBatch: {
                requireModule(modules.behavior_arena,
                              "behavior attachment arena");
                std::visit(
                    [&](const auto &operation) {
                        using Operation =
                            std::remove_cvref_t<decltype(operation)>;
                        if constexpr (std::same_as<
                                          Operation,
                                          EditorSetComponentValueOperation>) {
                            const auto entity = boundEditorEntity(
                                runtime_bindings, operation.object_id);
                            if (!entity) {
                                throw std::logic_error(
                                    "validated behavior binding disappeared");
                            }
                            const auto &authored =
                                operation.wire.at("authored_component");
                            behavior_edits.push_back(BehaviorAttachmentEdit{
                                .kind = BehaviorAttachmentEditKind::set_params,
                                .entity = *entity,
                                .authoring_object_id = operation.object_id,
                                .component_index =
                                    operation.attachment_index.value_or(0),
                                .identity = behaviorIdentity(operation.wire),
                                .stable_name =
                                    authored.at("type").get<std::string>(),
                                .canonical_params =
                                    canonicalBehaviorParams(
                                        authored, operation.object_id),
                                .raw_component = authored,
                            });
                        } else if constexpr (std::same_as<
                                                 Operation,
                                                 EditorAddComponentOperation>) {
                            const auto entity = boundEditorEntity(
                                runtime_bindings, operation.object_id);
                            if (!entity) {
                                throw std::logic_error(
                                    "validated behavior binding disappeared");
                            }
                            const ResolvedObject *object = nullptr;
                            for (const auto &scene :
                                 target.projectionState().resolved().scenes()) {
                                const auto found = std::find_if(
                                    scene.objects.begin(), scene.objects.end(),
                                    [&](const ResolvedObject &candidate) {
                                        return candidate.authoring_object_id ==
                                               operation.object_id;
                                    });
                                if (found != scene.objects.end()) {
                                    object = &*found;
                                    break;
                                }
                            }
                            if (object == nullptr) {
                                throw std::runtime_error(
                                    "behavior add target is absent from resolved scene");
                            }
                            const auto &authored =
                                operation.wire.at("component");
                            behavior_edits.push_back(BehaviorAttachmentEdit{
                                .kind = BehaviorAttachmentEditKind::attach,
                                .entity = *entity,
                                .authoring_object_id = operation.object_id,
                                .component_index = operation.component_index,
                                .component_origin = ResolvedComponentOrigin{
                                    AuthoredComponentOrigin{
                                        operation.component_index}},
                                .scene_object_index =
                                    object->authoring_object_index,
                                .scene_ordered = true,
                                .identity = behaviorIdentity(operation.wire),
                                .stable_name =
                                    authored.at("type").get<std::string>(),
                                .canonical_params =
                                    canonicalBehaviorParams(
                                        authored, operation.object_id),
                                .raw_component = authored,
                            });
                        } else if constexpr (std::same_as<
                                                 Operation,
                                                 EditorRemoveComponentOperation>) {
                            const auto entity = boundEditorEntity(
                                runtime_bindings, operation.object_id);
                            if (!entity) {
                                throw std::logic_error(
                                    "validated behavior binding disappeared");
                            }
                            behavior_edits.push_back(BehaviorAttachmentEdit{
                                .kind = BehaviorAttachmentEditKind::remove,
                                .entity = *entity,
                                .authoring_object_id = operation.object_id,
                                .component_index =
                                    operation.component_index,
                                .identity = behaviorIdentity(operation.wire),
                            });
                        } else if constexpr (
                            std::same_as<Operation, EditorReparentOperation> ||
                            std::same_as<Operation, EditorSpawnOperation> ||
                            std::same_as<Operation, EditorDestroyOperation> ||
                            std::same_as<Operation,
                                         EditorRemoveObjectsOperation> ||
                            std::same_as<Operation,
                                         EditorRestoreObjectsOperation> ||
                            std::same_as<Operation,
                                         EditorDuplicateOperation> ||
                            std::same_as<Operation,
                                         EditorSetPrefabParameterOperation> ||
                            std::same_as<Operation,
                                         EditorUnsetPrefabParameterOperation> ||
                            std::same_as<Operation,
                                         EditorUnpackPrefabOperation> ||
                            std::same_as<Operation,
                                         EditorRestorePrefabOperation>) {
                            // planEditorProjection only emits BehaviorBatch for
                            // the three component operations above; the object
                            // and prefab lifecycles use their own route kinds.
                        } else {
                            static_assert(
                                editorCanonicalOperationUnhandled<Operation>,
                                "unhandled EditorCanonicalOperation alternative");
                        }
                    },
                    plan.operations.at(route.operation_index));
                break;
            }
            case EditorProjectionRouteKind::LifecycleRenderer:
            case EditorProjectionRouteKind::LifecycleCamera:
            case EditorProjectionRouteKind::LifecycleLight:
            case EditorProjectionRouteKind::LifecycleCollider:
            case EditorProjectionRouteKind::LifecycleBehavior:
            case EditorProjectionRouteKind::Prefab:
            case EditorProjectionRouteKind::PrefabRenderer:
            case EditorProjectionRouteKind::PrefabCamera:
            case EditorProjectionRouteKind::PrefabLight:
            case EditorProjectionRouteKind::PrefabCollider:
            case EditorProjectionRouteKind::PrefabBehavior:
            case EditorProjectionRouteKind::OriginTransition:
                break;
            }
        }

        if (!transform_scenes.empty()) {
            // PM-2: an object this transaction removes is absent from the
            // next document and its entity goes with the lifecycle route, so
            // no transform is projected onto it. (A transaction that writes
            // an object's transform and then removes it -- the undo of a kept
            // "made it, moved it" -- would otherwise find a bound entity with
            // no authored transform.)
            std::unordered_set<std::uint64_t> removed;
            for (const auto &operation : plan.operations) {
                std::visit(
                    [&](const auto &candidate) {
                        using Operation = std::remove_cvref_t<decltype(candidate)>;
                        if constexpr (std::same_as<Operation, EditorDestroyOperation> ||
                                      std::same_as<Operation,
                                                   EditorRemoveObjectsOperation>) {
                            for (const auto id : candidate.object_ids)
                                removed.insert(id.value);
                        }
                    },
                    operation);
            }
            auto transform_bindings = runtime_bindings;
            std::erase_if(transform_bindings, [&](const auto &binding) {
                return removed.contains(binding.authoring_object_id.value);
            });
            adapters.push_back(std::make_unique<TransformProjectionAdapter>(
                makeTransformBindings(*modules.ecs,
                                      target.projectionState(),
                                      transform_bindings)));
            adapters.push_back(
                std::make_unique<StandaloneTransformProjectionAdapter>(
                    *modules.ecs, std::move(transform_bindings),
                    transform_scenes));
        }
        std::vector<ComponentCodec> catalog{
            plan.context.codecCatalog().begin(),
            plan.context.codecCatalog().end()};
        for (const auto &scene : ecs_codec_scenes) {
            adapters.push_back(std::make_unique<EcsCodecProjectionAdapter>(
                *modules.ecs, scene, runtime_bindings, catalog));
        }
        for (const auto &scene : renderer_scenes) {
            if (modules.models != nullptr &&
                modules.polygon_instances != nullptr) {
                adapters.push_back(
                    std::make_unique<RendererModelProjectionAdapter>(
                        *modules.ecs, *modules.models,
                        *modules.polygon_instances, scene,
                        runtime_bindings));
            }
        }
        if (modules.models == nullptr ||
            modules.polygon_instances == nullptr) {
            for (const auto &route : renderer_routes) {
                adapters.push_back(std::make_unique<
                    MissingProjectionParticipantGuard>(
                    EditorProjectionAdapterKind::RendererModel,
                    "renderer", route.scene_id, route.object_id,
                    route.component));
            }
        }
        for (const auto &scene : camera_scenes) {
            if (modules.camera != nullptr) {
                adapters.push_back(std::make_unique<CameraProjectionAdapter>(
                    *modules.camera, scene));
            }
        }
        if (modules.camera == nullptr) {
            for (const auto &route : camera_routes) {
                adapters.push_back(std::make_unique<
                    MissingProjectionParticipantGuard>(
                    EditorProjectionAdapterKind::Camera, "camera",
                    route.scene_id, route.object_id, route.component));
            }
        }
        for (const auto &scene : light_scenes) {
            if (modules.lights != nullptr) {
                adapters.push_back(std::make_unique<LightProjectionAdapter>(
                    *modules.lights, scene));
            }
        }
        if (modules.lights == nullptr) {
            for (const auto &route : light_routes) {
                adapters.push_back(std::make_unique<
                    MissingProjectionParticipantGuard>(
                    EditorProjectionAdapterKind::Light, "light",
                    route.scene_id, route.object_id, route.component));
            }
        }
        for (const auto &scene : collider_scenes) {
            if (modules.physics != nullptr) {
                adapters.push_back(std::make_unique<ColliderProjectionAdapter>(
                    *modules.ecs, *modules.physics, scene,
                    runtime_bindings));
            }
        }
        if (modules.physics == nullptr) {
            for (const auto &route : collider_routes) {
                adapters.push_back(std::make_unique<
                    MissingProjectionParticipantGuard>(
                    EditorProjectionAdapterKind::PhysWorld, "collider",
                    route.scene_id, route.object_id, route.component));
            }
        }

        std::unique_ptr<BehaviorProjectionJunctionState> behavior_state;
        if (!behavior_edits.empty() && lifecycle_updates.empty()) {
            requireModule(modules.behavior_arena,
                          "behavior attachment arena");
            behavior_state =
                std::make_unique<BehaviorProjectionJunctionState>(
                    BehaviorProjectionJunctionState{
                        .arena = *modules.behavior_arena,
                        .edits = std::move(behavior_edits),
                        .availability = behavior_availability,
                    });
            auto junction = makeBehaviorAttachmentProjectionJunction(
                "behavior_attachment.batch", behavior_state.get(),
                &BehaviorProjectionJunctionState::prepare,
                &BehaviorProjectionJunctionState::publish,
                &BehaviorProjectionJunctionState::rollback,
                &BehaviorProjectionJunctionState::finish);
            adapters.push_back(
                std::make_unique<EditorProjectionCallbackAdapter>(
                    std::move(junction)));
        }

        std::vector<internal::EditorPrefabProjectionRoute> prefab_routes;
        for (const auto &route : plan.routes) {
            if (route.kind != EditorProjectionRouteKind::Prefab) continue;
            OriginTransitionMap transition;
            const auto origin = std::find_if(
                plan.routes.begin(), plan.routes.end(),
                [&](const EditorProjectionRoute &candidate) {
                    return candidate.kind ==
                               EditorProjectionRouteKind::OriginTransition &&
                           candidate.operation_index ==
                               route.operation_index;
                });
            if (origin != plan.routes.end()) {
                transition = origin->origin_transition;
            }
            prefab_routes.push_back(internal::EditorPrefabProjectionRoute{
                .scene_id = route.scene_id,
                .full_component_projection =
                    route.full_component_projection,
                .origin_transition = std::move(transition),
                .behavior_availability = behavior_availability,
            });
        }
        if (lifecycle_updates.empty() && !prefab_routes.empty()) {
            requireModule(modules.behavior_arena,
                          "behavior attachment arena");
            auto prefab_adapters =
                internal::makeEditorPrefabBehaviorProjectionAdapters(
                    *modules.behavior_arena, runtime_bindings,
                    prefab_routes, behavior_availability);
            for (auto &adapter : prefab_adapters) {
                adapters.push_back(std::move(adapter));
            }
        }
        if (!lifecycle_updates.empty()) {
            requireModule(modules.behavior_arena,
                          "behavior attachment arena");
            const auto &scene_id = lifecycle_updates.front().scene_id;
            if (std::any_of(
                    lifecycle_updates.begin(), lifecycle_updates.end(),
                    [&](const LifecycleUpdate &update) {
                        return update.scene_id != scene_id;
                    })) {
                throw std::runtime_error(
                    "one editor lifecycle transaction cannot span runtime scenes");
            }
            std::vector<SceneObjectProjectionAdapter *>
                lifecycle_adapters;
            lifecycle_adapters.reserve(lifecycle_updates.size());
            for (const auto &update : lifecycle_updates) {
                lifecycle_adapters.push_back(update.adapter);
            }
            adapters.push_back(
                std::make_unique<BehaviorSceneProjectionAdapter>(
                    *modules.behavior_arena, scene_id,
                    runtime_bindings, std::move(lifecycle_adapters),
                    OriginTransitionMap{}, behavior_availability));
        }

        std::vector<EditorProjectionAdapter *> adapter_ptrs;
        adapter_ptrs.reserve(adapters.size());
        for (auto &adapter : adapters) adapter_ptrs.push_back(adapter.get());
        testing::ScopedComponentCodecCanonicalDefaults defaults{
            plan.context.canonicalDefaults()};
        EditorProjectionTransaction transaction{
            target, plan.base_revision, fault_injector};
        auto result = transaction.commit(plan.commands, adapter_ptrs);
        if (result.committed()) {
            for (const auto &update : lifecycle_updates) {
                const auto next = update.adapter->runtimeBindings();
                const auto replacement = std::find_if(
                    next.begin(), next.end(), [&](const auto &binding) {
                        return binding.authoring_object_id ==
                               update.object_id;
                    });
                const auto current_binding = std::find_if(
                    runtime_bindings.begin(), runtime_bindings.end(),
                    [&](const auto &binding) {
                        return binding.authoring_object_id ==
                               update.object_id;
                    });
                if (replacement == next.end()) {
                    if (current_binding != runtime_bindings.end()) {
                        runtime_bindings.erase(current_binding);
                    }
                } else if (current_binding == runtime_bindings.end()) {
                    runtime_bindings.push_back(*replacement);
                } else {
                    *current_binding = *replacement;
                }
            }
            binding_state.bindings = std::move(runtime_bindings);
        }
        return result;
    } catch (const std::exception &error) {
        return reject(EditorProjectionStatus::Failed,
                      EditorProjectionErrorCode::AdapterPrepareFailed,
                      error.what());
    }
}

// FG-2: how verify_fold starts its children: this executable, at this
// launch's fixed-step rate and the project's window size, with the launch
// options that change what a project resolves to (absolute paths, the game
// logic DLL).
static FoldVerifyProcessOptions foldVerifyProcessOptions(
    const EngineLaunchConfig &launch, const ProjectBasicConfig &project) {
    FoldVerifyProcessOptions options;
    options.executable = foldVerifyExecutablePath().value_or(std::filesystem::path{});
    options.fps = launch.fps;
    // The runs draw at the size the project opens its window at.
    const auto window = project.initialWindowSize();
    if (window.width > 0 && window.height > 0) {
        options.width = static_cast<std::uint32_t>(window.width);
        options.height = static_cast<std::uint32_t>(window.height);
    }
    if (launch.allow_absolute_paths)
        options.extra_arguments.emplace_back("--allow-absolute-paths");
    if (launch.game_logic_dll) {
        options.extra_arguments.emplace_back("--game-logic");
        options.extra_arguments.push_back(*launch.game_logic_dll);
    }
    return options;
}

std::unique_ptr<EditorCommandService> makeEditorRuntimeService(
    EditorRuntimeServiceOptions options) {
    auto runtime = std::make_shared<EditorRuntimeState>();
    std::shared_ptr<RenderConfigEditorService>
        render_config_editor;
    if (options.render_config_editing) {
        const auto source_reference =
            runtime->modules.project_config
                .renderingConfigReference();
        const auto source_bytes =
            runtime->modules.path_resolver
                .loadText(source_reference);
        // The editor service reads the pelican.graph root and refuses any
        // other root by name (render_config.legacy_root@1). A root without a
        // features array is inspected as is; the array is initialized only by
        // a requested edit and committed through the normal candidate
        // transaction.
        {
            const auto resolved =
                runtime->modules.path_resolver
                    .resolveExistingFileReference(
                        source_reference);
            const auto *source_path =
                std::get_if<std::filesystem::path>(
                    &resolved);
            if (source_path == nullptr) {
                throw std::runtime_error(
                    "WP331 requires the authored render config root to resolve to a file");
            }
            render_config_editor =
                std::make_shared<RenderConfigEditorService>(
                RenderConfigEditorDependencies{
                    .source_reference = source_reference,
                    .source_path = *source_path,
                    .source_bytes = source_bytes,
                    .project_root =
                        runtime->modules.path_resolver.projectRoot(),
                    .normalize_reference = [runtime](
                                               std::string_view reference) {
                        return runtime->modules.path_resolver
                            .normalizedReference(reference);
                    },
                    .resolve_document_path = [runtime](
                                                 std::string_view reference) {
                        const auto resolved =
                            runtime->modules.path_resolver
                                .resolveProjectRef(reference);
                        const auto *path =
                            std::get_if<std::filesystem::path>(&resolved);
                        if (path == nullptr) {
                            throw std::runtime_error(
                                "render authoring documents must resolve to project files");
                        }
                        return *path;
                    },
                    // EL-5: get_shader_interface resolves a stem's stage
                    // files as the shader library does; only paths cross
                    // to its worker, which reads the disk.
                    .resolve_shader_reference = [runtime](
                                                    std::string_view reference) {
                        return runtime->modules.path_resolver
                            .resolveProjectRef(reference);
                    },
                    // This is the independent editor query policy.  In
                    // particular, it never calls ReloadGate::enabled(),
                    // which is intentionally false for --rpc drivers
                    // (unless project.json declares hot_reload).
                    .gate = [runtime] {
                        const auto observation =
                            editorGateObservation(
                                runtime->modules, runtime->transitions);
                        return RenderConfigEditorGateObservation{
                            .can_edit =
                                observation.reasons == 0,
                            .transition_epoch =
                                observation.transition_epoch,
                            .reasons =
                                editorGateReasonNamesForRenderConfig(
                                    observation.reasons),
                        };
                    },
                    .runtime_snapshot = [runtime] {
                        (void)runtime;
                        const auto generation =
                            GET_MODULE(FrameGraphRuntimeContainer)
                                .snapshot();
                        if (generation == nullptr) {
                            throw std::runtime_error(
                                "render pipeline generation is unavailable");
                        }
                        return RenderConfigRuntimeSnapshot{
                            .published_generation =
                                generation->generation,
                            .enabled_feature_names =
                                generation
                                    ->enabled_feature_names,
                        };
                    },
                    .apply_candidate =
                        [runtime](
                            RenderConfigCandidateDocumentSet candidate,
                            RenderConfigSourceCommit source_commit,
                            RenderConfigRuntimeApplyCompletion
                                completion) {
                            if (runtime->modules.reload_service ==
                                nullptr) {
                                return RenderConfigRuntimeApplyResult{
                                    .error =
                                        "pelican.render_pipeline reload participant is unavailable",
                                };
                            }
                            const auto applied =
                                runtime->modules.reload_service
                                    ->applyAuthoredCandidate(
                                        watch::renderPipelineReloadParticipantName,
                                        std::move(candidate),
                                        std::move(source_commit),
                                        [completion =
                                             std::move(completion)](
                                            watch::AuthoredRuntimeReloadResult
                                                completed) mutable {
                                            if (!completion) return;
                                            completion(
                                                RenderConfigRuntimeApplyResult{
                                                    .committed = completed.committed,
                                                    .staged = completed.staged,
                                                    .staged_generation = completed.staged_generation,
                                                    .published_generation = completed.published_generation,
                                                    .error = std::move(completed.error),
                                                    .post_commit_error = std::move(completed.post_commit_error),
                                                    .exception = std::move(completed.exception),
                                                });
                                        });
                            return RenderConfigRuntimeApplyResult{
                                .committed = applied.committed,
                                .staged = applied.staged,
                                .staged_generation =
                                    applied.staged_generation,
                                .published_generation =
                                    applied.published_generation,
                                .error = applied.error,
                                .post_commit_error =
                                    applied.post_commit_error,
                                .exception = applied.exception,
                            };
                        },
                    .resolve_authoring_context =
                        [runtime](
                            const RenderConfigCandidateDocumentSet
                                &documents) {
                            const auto loader =
                                [&documents, runtime](
                                    std::string_view reference) {
                                    return loadRenderConfigCandidateText(
                                        documents,
                                        runtime->modules.path_resolver,
                                        reference);
                                };
                            std::optional<std::string> preset_bytes;
                            auto composed = composeRenderFeatureConfig(
                                nlohmann::json::parse(
                                    documents.rootDocument().bytes),
                                RenderFeatureComposeDependencies{
                                    .load_feature_json = loader,
#if PELICAN_RUNTIME_SHADER_COMPILER
                                    .runtime_shader_compiler_enabled = true,
#else
                                    .runtime_shader_compiler_enabled = false,
#endif
                                    .load_pipeline_json = [&](std::string_view reference) {
                                        preset_bytes = loader(reference);
                                        return *preset_bytes;
                                    },
                                    .validate_feature =
                                        [](std::string_view name,
                                           const nlohmann::json &feature) {
                                            requireCurrentRenderFeatureRuntimeAvailability(
                                                name, feature);
                                        },
                                    // LP-Vb: sizes a graph.loop over view_axis.
                                    .view_axis_length =
                                        renderGraphViewAxisLengthResolver(),
                                });
                            return ResolvedRenderConfigAuthoringContext{
                                .config = std::move(composed.config),
                                .pipeline_preset =
                                    std::move(composed.pipeline_preset),
                                .pass_provenance =
                                    std::move(composed.pass_provenance),
                                .pipeline_preset_bytes = std::move(preset_bytes),
                                .resource_provenance =
                                    std::move(composed.resource_provenance),
                                // FG-1: edit_target_planning reads them.
                                .fold_planning =
                                    std::move(composed.fold_planning),
                            };
                        },
                    // G-6: the candidate as the engine's start-up
                    // registration composes it -- the launch's feature
                    // overlays applied, preview always, xr only when this
                    // launch activated it.
                    .preflight_graph_variants =
                        [runtime](
                            const RenderConfigCandidateDocumentSet
                                &documents) {
                            const auto loader =
                                [&documents, runtime](
                                    std::string_view reference) {
                                    return loadRenderConfigCandidateText(
                                        documents,
                                        runtime->modules.path_resolver,
                                        reference);
                                };
                            const auto &launch =
                                runtime->modules.launch_config;
                            std::string bytes =
                                documents.rootDocument().bytes;
                            if (!launch.render_feature_overlays.empty()) {
                                bytes = applyRenderFeatureOverlays(
                                            nlohmann::json::parse(bytes),
                                            launch.render_feature_overlays,
                                            loader)
                                            .dump();
                            }
                            preflightRenderGraphVariants(
                                bytes, loader,
#if PELICAN_RUNTIME_SHADER_COMPILER
                                true,
#else
                                false,
#endif
                                launch.xr_active);
                        },
                    // X-1: the catalog is the engine's own set plus the
                    // documents installed packages provide.  The package half
                    // is evaluated with the same availability rule and never
                    // throws, so bytes a user brought in cannot stop this
                    // service from being constructed.
                    .feature_catalog = [runtime] {
                        const std::function<
                            RenderFeatureRuntimeAvailability(
                                std::string_view, const nlohmann::json &)>
                            availability =
                                [](std::string_view feature_name,
                                   const nlohmann::json &feature_document) {
                                    return currentRenderFeatureRuntimeAvailability(
                                        feature_name, feature_document);
                                };
                        return composeRenderFeatureCatalog(
                            enumerateEngineRenderFeatureDocuments(
                                availability),
                            enumeratePackageRenderFeatureDocuments(
                                runtime->modules.path_resolver
                                    .projectRoot(),
                                availability));
                    },
                    // Read what the draw path published; never evaluate again.
                    .graph_values = [runtime] {
                        return runtime->modules.scene_loader.graphValues();
                    },
                    // EL-1: the materials the draw-loop tables judge, read
                    // the way get_frame_plan's draw_loop elements read them.
                    .draw_loop_materials = [] {
                        std::vector<MaterialDrawSubject> subjects;
                        if (!FastModuleContainer::isInitialized<
                                MaterialContainer>()) {
                            return subjects;
                        }
                        const auto &materials =
                            GET_MODULE(MaterialContainer);
                        for (const auto material :
                             materials.drawLoopMaterials()) {
                            subjects.push_back(
                                materials.drawLoopSubject(material));
                        }
                        return subjects;
                    },
                    .region_providers = [] {
                        std::vector<RenderAuthoringRegionProvider> result;
                        for (auto &provider :
                             subgraphReplacementRegistry()
                                 .providerListing()) {
                            result.push_back({
                                .name = std::move(provider.name),
                                .engine_owned = provider.engine_owned,
                                .provider_version =
                                    provider.provider_version,
                            });
                        }
                        return result;
                    },
                    // RL-1: the preset and features the live generation was
                    // composed from, as its programs recorded them.
                    .published_documents = [runtime] {
                        std::map<std::string, std::string> result;
                        const auto generation =
                            GET_MODULE(FrameGraphRuntimeContainer).snapshot();
                        if (generation == nullptr) return result;
                        for (const auto &[id, program] : generation->programs) {
                            (void)id;
                            if (!program.frame_graph.source_documents) continue;
                            for (const auto &[reference, text] :
                                 *program.frame_graph.source_documents) {
                                if (reference.starts_with("engine://")) continue;
                                try {
                                    result.insert_or_assign(
                                        runtime->modules.path_resolver
                                            .normalizedReference(reference),
                                        renderConfigSourceDigest(text));
                                } catch (const std::exception &) {
                                }
                            }
                        }
                        return result;
                    },
                    // IA-2: the initial assets the live generation read, as
                    // its programs recorded them.
                    .published_initial_assets = [] {
                        std::vector<RenderConfigPublishedInitialAsset> result;
                        const auto generation =
                            GET_MODULE(FrameGraphRuntimeContainer).snapshot();
                        if (generation == nullptr) return result;
                        for (const auto &[id, program] : generation->programs) {
                            (void)id;
                            for (const auto &[name, asset] :
                                 program.frame_graph.image_initial_assets) {
                                (void)name;
                                result.push_back({.asset = asset->asset,
                                                  .file = asset->file,
                                                  .sha256 = asset->content_sha256});
                            }
                        }
                        return result;
                    },
                    // FG-2: verify_fold's runs are this executable, headless,
                    // in a copy of the project (graphfoldverify.hpp). What the
                    // children need of this launch is read once, here: the
                    // worker never reads the engine's modules.
                    .verify_fold =
                        [options = foldVerifyProcessOptions(
                             runtime->modules.launch_config,
                             runtime->modules.project_config)](
                            const FoldVerifyJob &job,
                            const std::atomic<bool> &cancelled) {
                            return runFoldVerifyProcesses(job, options,
                                                          cancelled);
                        },
                    // A deterministic driver does not start processes of its
                    // own; neither does a launch that cannot name its
                    // executable.
                    .verify_fold_unavailable =
                        [runtime]() -> std::optional<std::string> {
                        const auto &launch = runtime->modules.launch_config;
                        if (launch.golden_mode) return "golden";
                        if (launch.input_replay ||
                            runtime->modules.input_sequence.isReplaying())
                            return "replay";
                        if (launch.headless) return "headless";
                        if (!foldVerifyExecutablePath())
                            return "executable_unknown";
                        return std::nullopt;
                    },
                    // AB-1: list_derived_assets walks the asset stores.
                    .asset_stores = [runtime] {
                        return runtime->modules.path_resolver.stores();
                    },
                });
        }
    }
    std::shared_ptr<ProceduralAssetEditorService> procedural_asset_editor;
    if (options.render_config_editing) {
        const auto reference = runtime->modules.project_config.assetDataReference();
        const auto resolved = runtime->modules.path_resolver.resolveExistingFileReference(reference);
        if (const auto *path = std::get_if<std::filesystem::path>(&resolved)) {
            const auto raw_assets = runtime->modules.path_resolver.loadText(reference);
            const bool source_matches_runtime = runtime->modules.project_config.prepareAssetDataJson(
                raw_assets, runtime->modules.path_resolver) == runtime->modules.project_config.assetDataJson();
            procedural_asset_editor = std::make_shared<ProceduralAssetEditorService>(ProceduralAssetEditorDependencies{
                .reference = reference,
                .normalized_reference = runtime->modules.path_resolver.normalizedReference(reference),
                .path = *path,
                .project_root = runtime->modules.path_resolver.projectRoot(),
                .source_bytes = raw_assets,
                .gate = [runtime, source_matches_runtime] {
                    const auto gate = editorGateObservation(
                        runtime->modules, runtime->transitions);
                    return RenderConfigEditorGateObservation{
                        .can_edit = source_matches_runtime && gate.reasons == 0 && !runtime->modules.renderer.hasPendingRenderPipelinePublication(),
                        .transition_epoch = gate.transition_epoch};
                },
                .apply = [runtime](std::string_view bytes, const std::function<void()> &source_commit) {
                    auto prepared = PreparedProceduralAssetPublication::prepare(
                        runtime->modules.project_config, runtime->modules.path_resolver,
                        GET_MODULE(ProceduralGeometryContainer), runtime->modules.polygon_instances,
                        FastModuleContainer::tryGet<VulkanManageCore>(), runtime->modules.scene_loader.currentScene(), bytes);
                    prepared->commit(source_commit);
                }});
        }
    }
    return std::make_unique<EditorCommandService>(EditorCommandServiceDependencies{
        .document = [runtime]() -> const AuthoringSceneDocument & {
            return runtime->modules.project_config.sceneDocument();
        },
        .resolved_scene = [runtime]() -> const ResolvedScene & {
            return runtime->modules.project_config.resolvedScene();
        },
        .current_scene_id = [runtime] {
            return runtime->modules.scene_loader.currentScene();
        },
        .runtime_query =
            [runtime](const ResolvedSceneView &scene,
                      const ResolvedObject &object) {
                runtime->refreshRuntimeBindings();
                return queryEditorRuntime(runtime->modules,
                                           runtime->binding_state.bindings,
                                           scene, object);
            },
        .assets = [runtime] { return collectEditorAssets(runtime->modules); },
        .snapshot_state = [] { return EditorSnapshotState{}; },
        .edit = EditorEditRuntimeDependencies{
            .document = [runtime]() -> const AuthoringSceneDocument & {
                return runtime->modules.project_config.sceneDocument();
            },
            .projection_state = [runtime]() -> const SceneProjectionState & {
                return runtime->modules.project_config.sceneProjectionState();
            },
            .current_scene_id = [runtime] {
                return runtime->modules.scene_loader.currentScene();
            },
            .execute = [runtime, fault_injector =
                                     options.projection_fault_injector](
                           const EditorEditExecutionRequest &request) {
                runtime->refreshRuntimeBindings();
                auto context = makeEditorProjectionContext(
                    runtime->modules.scene_loader.currentScene(),
                    EditorProjectionMode::Committed);
                runtime->binding_state.codec_generation =
                    context.layoutGeneration();
                runtime->binding_state.behavior_generation =
                    context.behaviorGeneration();
                ProjectBasicConfigProjectionTarget target{
                    runtime->modules.project_config};
                try {
                    auto plan_result = planEditorProjection(
                        EditorProjectionPlanningRequest{
                            .base_revision = request.base_revision,
                            .commands = request.commands,
                            .operations = request.operations,
                        },
                        target.projectionState(), context);
                    if (!plan_result) {
                        return rejectProjectionPlanFailure(
                            std::move(plan_result).error(),
                            request.base_revision,
                            target.projectionState().revision());
                    }
                    auto plan = std::move(plan_result).value();
                    return materializeEditorProjection(
                        plan, editorProjectionModules(runtime->modules),
                        target, runtime->binding_state,
                        EditorProjectionExpectation{
                            .binding_epoch = runtime->modules.scene_loader
                                                 .runtimeSceneEpoch(),
                            .codec_generation = context.layoutGeneration(),
                            .behavior_generation =
                                context.behaviorGeneration(),
                        },
                        fault_injector);
                } catch (const EditorProjectionPlanError &error) {
                    return EditorProjectionResult{
                        .status = EditorProjectionStatus::Rejected,
                        .base_revision = request.base_revision,
                        .committed_revision =
                            target.projectionState().revision(),
                        .error = EditorProjectionError{
                            EditorProjectionErrorCode::CommandInvalid, "/",
                            error.what()},
                    };
                }
            },
            .execute_preview =
                [runtime](const EditorPreviewExecutionRequest &request) {
                    runtime->refreshRuntimeBindings();
                    auto context = makeEditorProjectionContext(
                        runtime->modules.scene_loader.currentScene(),
                        EditorProjectionMode::Preview);
                    runtime->binding_state.codec_generation =
                        context.layoutGeneration();
                    runtime->binding_state.behavior_generation =
                        context.behaviorGeneration();
                    EphemeralEditorProjectionTarget target{
                        runtime->modules.project_config
                            .sceneProjectionState()};
                    try {
                        auto plan_result = planEditorProjection(
                            EditorProjectionPlanningRequest{
                                .base_revision = request.base_revision,
                                .commands = request.commands,
                                .operations = request.operations,
                                .restore_committed =
                                    request.restore_committed,
                            },
                            target.projectionState(), context);
                        if (!plan_result) {
                            return rejectProjectionPlanFailure(
                                std::move(plan_result).error(),
                                request.base_revision,
                                target.projectionState().revision());
                        }
                        auto plan = std::move(plan_result).value();
                        return materializeEditorProjection(
                            plan,
                            editorProjectionModules(runtime->modules),
                            target, runtime->binding_state,
                            EditorProjectionExpectation{
                                .binding_epoch =
                                    runtime->modules.scene_loader
                                        .runtimeSceneEpoch(),
                                .codec_generation =
                                    context.layoutGeneration(),
                                .behavior_generation =
                                    context.behaviorGeneration(),
                            });
                    } catch (const EditorProjectionPlanError &error) {
                        return EditorProjectionResult{
                            .status = EditorProjectionStatus::Rejected,
                            .base_revision = request.base_revision,
                            .committed_revision =
                                target.projectionState().revision(),
                            .error = EditorProjectionError{
                                EditorProjectionErrorCode::CommandInvalid,
                                "/", error.what()},
                        };
                    }
                },
            .preview_boundary = [runtime] {
                runtime->modules.renderer.resetTemporalHistory();
            },
            .gate = [runtime] {
                return editorGateObservation(runtime->modules,
                                             runtime->transitions);
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
                    runtime->refreshRuntimeBindings();
                    const auto entity = boundEditorEntity(
                        runtime->binding_state.bindings, object_id);
                    if (!entity) return std::nullopt;
                    const auto attachment = runtime->modules.behavior_arena
                                                .findEditorAttachment(
                                                    *entity, attachment_index);
                    if (!attachment) return std::nullopt;
                    EditorBehaviorAttachmentIdentity::Origin origin;
                    if (const auto *authored =
                            attachment->component_origin.authored()) {
                        origin.authoring_component_index =
                            authored->authoring_component_index;
                    } else if (const auto *generated =
                                   attachment->component_origin.generated()) {
                        origin.generated = true;
                        origin.stable_generated_id =
                            generated->stable_generated_id;
                        origin.prefab = generated->prefab;
                        origin.instance_id = generated->instance_id;
                        origin.component_key = generated->component_key;
                        origin.digest_sha256 = generated->digest_sha256;
                    }
                    return EditorBehaviorAttachmentIdentity{
                        .handle = attachment->handle.value,
                        .attachment_seq = attachment->attachment_seq,
                        .authoring_object_id = object_id,
                        .authored_component_occurrence = attachment_index,
                        .stable_type = attachment->stable_name,
                        .origin = std::move(origin),
                    };
                },
            .install_commit_hook = true,
            .prefab_registry = [runtime] {
                return runtime->modules.project_config.resolvedScene()
                    .prefabRegistry();
            },
            .bindable_provider = [runtime] {
                return runtime->modules.project_config.resolvedScene()
                    .bindableProvider();
            },
            .closure_generation = [runtime] {
                return runtime->modules.project_config.resolvedScene()
                    .prefabClosure().generation;
            },
            .component_exists = [runtime](std::string_view name) {
                return runtime->modules.component_info.hasComponent(name);
            },
        },
        .preview = EditorPreviewServiceDependencies{
            .document = [runtime]() -> const AuthoringSceneDocument & {
                return runtime->modules.project_config.sceneDocument();
            },
            .projection_state = [runtime]() -> const SceneProjectionState & {
                return runtime->modules.project_config.sceneProjectionState();
            },
            .preview_graph = [runtime]() -> const PreviewGraphProgram & {
                return runtime->modules.renderer.previewGraphProgram();
            },
            .xr_active = [runtime] { return runtime->modules.launch_config.xr_active; },
            .engine_time = [runtime] {
                return PreviewEngineTimeSnapshot{
                    .time = runtime->modules.engine_time.now(),
                    .delta = runtime->modules.engine_time.dt(),
                    .frame_index = runtime->modules.engine_time.evaluationIndex(),
                };
            },
            .shared_state_snapshot = [runtime] {
                runtime->refreshRuntimeBindings();
                return runtime->previewSharedState();
            },
        },
        .render_config_editor =
            std::move(render_config_editor),
        .procedural_asset_editor = std::move(procedural_asset_editor),
        .import_scene_snapshot_versioned =
            [runtime](std::string_view bytes,
                      std::string_view current_scene_id,
                      std::uint64_t schema_version) {
                // The service has already refused every snapshot version
                // but 2 by name (MG-2b); a scene import has one form.
                (void)schema_version;
                std::vector<EditorProjectionRuntimeObjectBinding>
                    next_runtime_bindings;
                const auto revision =
                    runtime->modules.project_config.importSceneDocument(
                        bytes,
                        [&](SceneLoadRequest &&request) {
                            request.scene_id = std::string{current_scene_id};
                            auto result = runtime->modules.scene_loader.load(
                                std::move(request));
                            if (!std::holds_alternative<PreCommitFailure>(
                                    result)) {
                                next_runtime_bindings =
                                    collectEditorRuntimeBindings(
                                        runtime->modules);
                            }
                            return result;
                        });
                runtime->binding_state.bindings.swap(next_runtime_bindings);
                runtime->binding_state.epoch =
                    runtime->modules.scene_loader.runtimeSceneEpoch();
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
        .ray_occurrences = [runtime](std::uint64_t object) {
            const auto &instances = runtime->modules.polygon_instances;
            return EditorRayOccurrencesResult{
                .entries = instances.rayOccurrences(object),
                .plan = instances.rayAsPlan(),
                .authoring_object_id = object,
            };
        },
        .resolve_ray_occurrence = [runtime](std::string_view occurrence_id) {
            return runtime->modules.polygon_instances
                .resolveRayOccurrence(occurrence_id);
        },
    });
}

} // namespace Pelican
