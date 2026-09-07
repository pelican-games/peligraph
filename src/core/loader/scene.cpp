#include "scene.hpp"
#include "../appflow/enginetime.hpp"
#include <cmath>
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include "modelphysicsbinding.hpp"

#include "../ecs/predefined/modelview.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../ecs/core.hpp"
#include "../ecs/archetypemigration.hpp"
#include "../asset/model.hpp"
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
#include "../phys/physworld.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../renderer/proceduralgeometrycontainer.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../renderingpass/renderingpassconfigloader.hpp"
#include "../vkcore/core.hpp"
#include "../../project/assetdataformat.hpp"
#include "../userpublic/events.hpp"
#include "../userpublic/deterministicrng.hpp"
#include "../userpublic/details/event/registerer.hpp"
#include "../userpublic/gameobjects.hpp"
#include "pathresolver.hpp"
#include "sceneloadfault.hpp"
#include "effecttransaction.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <components/localtransform.hpp>
#include <components/predefined.hpp>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {

void internal::validateResolvedSceneFeatureBindings(
    const ResolvedSceneView &scene) {
    const auto *ray_layout =
        FastModuleContainer::tryGet<RayLayoutRuntimeState>();
    const auto ray_mode = ray_layout != nullptr && ray_layout->configured()
                              ? ray_layout->mode()
                              : RayLayoutSemanticMode::LegacyNoLayout;
    if (ray_mode != RayLayoutSemanticMode::LayoutV3) {
        for (const auto &object : scene.objects) {
            for (const auto &component : object.components) {
                if (component.name == "pelican_procedural_geometry") {
                    throw std::runtime_error(
                        "pelican.ray_group_type_requires_layout_v3@1: procedural geometry requires layout version 3");
                }
            }
        }
    }
    if (!requiresRayLayoutV2(ray_mode)) {
        bool external_owner = false;
        if (const auto *components =
                FastModuleContainer::tryGet<ComponentInfoManager>()) {
            try {
                (void)components->getComponentIdByName(
                    "pelican_ray_instance");
                external_owner = true;
            } catch (const ComponentUnknownError &) {
            }
        }
        if (!external_owner) {
            for (const auto &object : scene.objects) {
                if (std::any_of(object.components.begin(),
                                object.components.end(),
                                [](const auto &component) {
                                    return component.name ==
                                           "pelican_ray_instance";
                                })) {
                    throw std::runtime_error(
                        "pelican.ray_instance_requires_layout_v2@1: builtin pelican_ray_instance declarations require ray_group_layout version 2");
                }
            }
        }
    } else {
        for (const auto &object : scene.objects) {
            const bool has_ray_instance = std::any_of(
                object.components.begin(), object.components.end(),
                [](const auto &component) {
                    return component.name == "pelican_ray_instance";
                });
            const bool has_model = std::any_of(
                object.components.begin(), object.components.end(),
                [](const auto &component) {
                    return component.name == "simplemodelview";
                });
            const bool has_procedural = std::any_of(
                object.components.begin(), object.components.end(),
                [](const auto &component) {
                    return component.name ==
                           "pelican_procedural_geometry";
                });
            if (has_ray_instance && !has_model && !has_procedural) {
                throw std::runtime_error(
                    "pelican.ray_instance_not_ray_traced@1: pelican_ray_instance object has no ray-traceable model component");
            }
        }
    }
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

struct SceneLoader::RetirementState {
    struct Batch {
        std::vector<ECSEntityMutationToken> destroyed_entities;
        std::vector<ModelTemplate> transient_models;
    };
    std::vector<Batch> queue;

    void reserveOne() { queue.reserve(queue.size() + 1U); }

    void enqueue(Batch batch, SceneLoadPhaseTrace &phase_trace) noexcept {
        phase_trace.record(SceneLoadPhase::retire);
        static_assert(std::is_nothrow_move_constructible_v<Batch>);
        assert(queue.size() < queue.capacity());
        queue.push_back(std::move(batch));
    }
};

SceneLoadFaultInjector *SceneLoader::default_fault_injector_for_testing =
    nullptr;

SceneLoader::SceneLoader()
    : fault_injector{default_fault_injector_for_testing},
      retirement_state{std::make_unique<RetirementState>()} {
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    run_nonce = ticks ^ static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(this));
    if (run_nonce == 0) run_nonce = 1;
}
SceneLoader::~SceneLoader() {
    drainRetirementQueue();
    releaseTransientModels(false);
}

namespace {

struct PendingSceneTransaction {
    SceneLoader *loader = nullptr;
    const SceneProjectionState *projection = nullptr;
    std::optional<SceneId> *candidate = nullptr;
};

thread_local PendingSceneTransaction *active_pending_scene_transaction =
    nullptr;

class PendingSceneTransactionScope {
    PendingSceneTransaction *previous_ = nullptr;

  public:
    explicit PendingSceneTransactionScope(
        PendingSceneTransaction &transaction) noexcept
        : previous_{std::exchange(active_pending_scene_transaction,
                                  &transaction)} {}
    ~PendingSceneTransactionScope() {
        active_pending_scene_transaction = previous_;
    }
};

std::string exceptionMessage(const std::exception_ptr &error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const std::exception &exception) {
        return exception.what();
    } catch (...) {
        return "non-standard scene load exception";
    }
    return "unknown scene load failure";
}

std::vector<std::string>
releaseErrorStatus(const std::exception_ptr &error) {
    ReleaseReport report;
    bool has_report = false;
    try {
        if (error) std::rethrow_exception(error);
    } catch (const ModelResourceReleaseException &exception) {
        report = exception.releaseReport();
        has_report = true;
    } catch (...) {
    }
    if (!has_report) return {};

    std::vector<std::string> result;
    result.reserve(report.error_count +
                   (report.overflow_count == 0 ? 0U : 1U));
    for (const auto &release_error : report.view()) {
        result.emplace_back(releaseStageName(release_error.stage));
    }
    if (report.overflow_count != 0) {
        result.push_back("overflow:" +
                         std::to_string(report.overflow_count));
    }
    return result;
}

std::string displayObjectName(const std::string &object_name) {
    return object_name.empty() ? std::string{"<unnamed>"} : object_name;
}

ComponentId getComponentIdForObject(ComponentInfoManager &component_info_manager, const std::string &component_name,
                                    const std::string &object_name) {
    (void)object_name;
    return component_info_manager.getComponentIdByName(component_name);
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
    std::optional<ModelInstanceId> prepared_model_instance;
    std::optional<PhysWorld::Binding> prepared_model_physics;
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
    } catch (const ResolutionFailureException &error) {
        throw ResolutionFailureException{
            error.failure(),
            "Invalid collider on object '" +
                displayObjectName(object_name) + "': " + error.what()};
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
                    const auto &light =
                        std::any_cast<const LightCodecData &>(
                            component.requireRuntimeValue());
                    light_entries.push_back(
                        LightLoadEntry{object_name, light});
                } catch (const ResolutionFailureException &error) {
                    throw ResolutionFailureException{
                        error.failure(),
                        "Invalid light on object '" +
                            displayObjectName(object_name) + "': " +
                            error.what()};
                } catch (const std::exception &error) {
                    throw std::runtime_error(
                        "Invalid light on object '" +
                        displayObjectName(object_name) + "': " +
                        error.what());
                }
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
        if (owns_resources) ReleaseErrorSink::record(
            "unpublished transient model", releaseModelGpuResources(model, false));
    }
};

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

const SceneLoadResult &requireSceneLoadCommitted(const SceneLoadResult &result) {
    if (std::holds_alternative<SceneLoadCommitted>(result) ||
        std::holds_alternative<CommittedWithEffectFailure>(result)) {
        return result;
    }
    if (const auto *failure = std::get_if<PreCommitFailure>(&result)) {
        if (failure->exception) std::rethrow_exception(failure->exception);
        throw std::runtime_error(failure->error);
    }
    throw std::logic_error("unknown scene load result");
}

nlohmann::ordered_json effectErrorsJson(
    std::span<const CommittedWithEffectFailure::EffectError> errors) {
    auto result = nlohmann::ordered_json::array();
    for (const auto &error : errors) {
        result.push_back({{"api", error.api},
                          {"error_code", error.error_code},
                          {"error_fields", error.error_fields},
                          {"phase", error.phase}});
    }
    return result;
}

nlohmann::ordered_json sceneLoadDiagnosticsJson(
    const SceneLoadDiagnostics &diagnostics) {
    auto rejected_rows = nlohmann::ordered_json::array();
    for (const auto &row : diagnostics.rejected_rows) {
        rejected_rows.push_back({
            {"address",
             {{"object", row.address.object},
              {"component", row.address.component_witness},
              {"list_path", row.address.list_path},
              {"row_id", row.address.row_id}}},
            {"error", row.error},
        });
    }
    return {
        {"rejected_rows", std::move(rejected_rows)},
        {"effect_errors", effectErrorsJson(diagnostics.effect_errors)},
    };
}

SceneLoadDiagnostics rayScenePlanDiagnostics(
    const RaySceneRowSnapshot &plan, std::uint64_t authoring_object_id) {
    SceneLoadDiagnostics result;
    for (const auto &row : plan.occurrence_overrides) {
        if (row.error.empty() ||
            (authoring_object_id != 0 && row.authoring_object_id != authoring_object_id))
            continue;
        result.rejected_rows.push_back({
            .address = {.object = row.authoring_object_id,
                        .component_witness = row.ray_instance_component_witness,
                        .list_path = "/overrides", .row_id = row.row_id},
            .error = {{"code", row.error}, {"occurrence_id", row.occurrence_id},
                      {"hint", "Select an eligible occurrence from get_ray_occurrences or remove this override."}},
        });
    }
    return result;
}

SceneLoadResult SceneLoader::load(SceneLoadRequest request) {
    SceneLoadPhaseTrace phase_trace;
    auto *const injector = fault_injector;
    if (internal::sceneLoadRootDisconnected(request.root, injector)) {
        auto error = std::make_exception_ptr(
            std::runtime_error("injected scene-load root disconnect"));
        return PreCommitFailure{SceneLoadPhase::prepare,
                                exceptionMessage(error), error};
    }
    SceneAuthority candidate_authority{};
    if (next_authority_epoch == std::numeric_limits<std::uint64_t>::max()) {
        auto error = std::make_exception_ptr(
            std::overflow_error("scene authority epoch space exhausted"));
        return PreCommitFailure{SceneLoadPhase::prepare,
                                exceptionMessage(error), error};
    }
    candidate_authority = SceneAuthority{run_nonce, next_authority_epoch++};

    auto &config = GET_MODULE(ProjectBasicConfig);
    MetadataCommitToken metadata;
    std::unique_ptr<internal::PreparedEventOutbox> event_outbox;
    std::unique_ptr<internal::PreparedEffectTransaction> effect_transaction;
    internal::QueuedEvent scene_loaded_event;
    DeterministicRng::PreparedSequence rng_candidate;
    std::optional<SceneId> pending_candidate;
    Camera::PreparedSceneState camera_candidate;
    LightContainer::PreparedLoad light_candidate;
    PolygonInstanceContainer::PreparedSceneState renderer_candidate;
    ProceduralGeometryContainer::PreparedSceneState procedural_candidate;
    std::vector<std::uint32_t> renderer_retire_indices;
    SeqPlayer::PreparedSceneState seq_candidate;
    VatPlayer::PreparedSceneState vat_candidate;
    PhysWorld::PreparedState physics_candidate;
    std::unordered_map<std::string, ObjectBinding> candidate_object_bindings;
    std::vector<SceneRuntimeObjectBinding> candidate_runtime_bindings;
    std::vector<ModelTemplate> candidate_transient_models;
    bool candidate_runtime_only_changes = false;
    std::vector<EcsObjectLoad> ecs_objects;
    std::vector<GameObjectId> old_entities;
    std::vector<GameObjectId> object_ids;
    std::vector<ECSEntityMutationToken> created_entities;
    std::vector<ECSEntityMutationToken> destroyed_entities;
    std::unique_ptr<PreparedBehaviorSceneReconciliation> behavior_candidate;
    DeterministicRng *rng = nullptr;
    bool renderer_published = false;
    bool procedural_published = false;
    bool light_published = false;
    bool candidates_published = false;
    double candidate_scene_start_time = 0;

    const auto fail = [&](SceneLoadPhase phase,
                          std::exception_ptr error) -> SceneLoadResult {
        return PreCommitFailure{phase, exceptionMessage(error), error,
                                releaseErrorStatus(error)};
    };

    try {
        if (auto *owned = std::get_if<SceneLoadRequest::Owned>(
                &request.source)) {
            metadata = std::move(owned->token);
        } else {
            metadata = config.prepareBorrowedSceneMetadata(
                std::get<SceneLoadRequest::Borrowed>(request.source)
                    .committed.get());
        }
        metadata.prepareRuntimeIdentity(request.scene_id,
                                        candidate_authority,
                                        phase_trace);
        if (internal::sceneLoadMutationEnabled(
                injector, SceneLoadFaultPhase::prepare, "metadata",
                scene_load_mutation::metadata_projection)) {
            throw std::runtime_error(
                "injected scene metadata prepare drop");
        }
        const auto &projection = metadata.projection();
        const auto &scene_document = projection.resolved();
        for (const auto &warning : scene_document.warnings()) {
            if (logger != nullptr) LOG_WARNING(logger, "{}", warning);
        }
        const auto *scene = scene_document.findScene(request.scene_id);
        if (scene == nullptr) {
            throw std::runtime_error("scene not found: " + request.scene_id);
        }
        internal::validateResolvedSceneFeatureBindings(*scene);
        candidate_scene_start_time = GET_MODULE(EngineTime).now();
        const bool has_procedural = std::any_of(
            scene->objects.begin(), scene->objects.end(), [](const auto &object) {
                return std::any_of(
                    object.components.begin(), object.components.end(),
                    [](const auto &component) {
                        return component.name ==
                               "pelican_procedural_geometry";
                    });
            });
        AssetDataFormatDocument asset_data;
        VulkanManageCore *procedural_vkcore = nullptr;
        if (has_procedural) {
            asset_data = parseAssetDataFormatJson(
                nlohmann::json::parse(config.assetDataJson()));
            procedural_vkcore = &GET_MODULE(VulkanManageCore);
        }
        procedural_candidate =
            GET_MODULE(ProceduralGeometryContainer).prepareScene(
                asset_data, *scene, procedural_vkcore,
                candidate_authority.scene_epoch, phase_trace);

        std::vector<LightLoadEntry> light_entries;
        auto &component_info_manager = GET_MODULE(ComponentInfoManager);
        const auto game_logic_status = configuredGameLogicStatus();
        const auto behavior_availability =
            game_logic_status.loaded
                ? BehaviorRegistryAvailability::active
                : BehaviorRegistryAvailability::dll_unavailable;
        auto behavior_entries = prepareResolvedSceneBehaviorAttachments(
            scene->objects, behavior_availability);
        ecs_objects = prepareSceneBindings(
            *scene, component_info_manager, light_entries,
            std::move(behavior_entries));

        auto &renderer = GET_MODULE(PolygonInstanceContainer);
        renderer_candidate =
            renderer.prepareSceneReplacement(renderer_retire_indices);
        for (auto &object : ecs_objects) {
            for (const auto &component : object.components) {
                if (component.codec == nullptr ||
                    component.codec->name != "simplemodelview") {
                    continue;
                }
                SimpleModelViewComponent model_view;
                applyComponentLoad(component_info_manager, &model_view,
                                   component);
                if (model_view.model_name.empty()) continue;
                auto &model = GET_MODULE(ModelAssetContainer)
                                  .getModelTemplateByName(
                                      model_view.model_name);
                object.prepared_model_physics = makeModelPhysicsBinding(
                    model, runtimeObjectIdentityName(request.scene_id,
                        object.authoring_object_id, object.name), PhysWorldTransform{});
                auto staged =
                    renderer.stagePreparedSceneModel(renderer_candidate,
                                                     model);
                object.prepared_model_instance = staged.id();
                renderer.publishPreparedSceneModel(renderer_candidate,
                                                    std::move(staged));
                renderer_candidate.ray_authoring_object_ids.at(
                    object.prepared_model_instance->index) = object.authoring_object_id.value;
                break;
            }
        }
        const auto ray_project = loadRayTracingProjectConfig(
            loadRenderingPassConfigJsonFromString(config.renderingConfigJson(),
                                                 "scene ray plan prepare"),
            [](std::string_view reference) { return GET_MODULE(PathResolver).loadText(reference); },
            PELICAN_RUNTIME_SHADER_COMPILER != 0);
        renderer.prepareRayScenePlan(renderer_candidate, ray_project, scene,
            GET_MODULE(ProceduralGeometryContainer).preparedRayInstances(*scene, &procedural_candidate),
            &phase_trace);
        if (internal::sceneLoadMutationEnabled(injector, SceneLoadFaultPhase::prepare,
                "ray", scene_load_mutation::ray_occurrence_catalog)) {
            auto dropped = std::make_shared<RayScenePlanState>(*renderer_candidate.ray_scene_plan);
            dropped->inventory = std::make_shared<RaySceneInventorySnapshot>();
            renderer_candidate.ray_scene_plan = std::move(dropped);
        }
        if (internal::sceneLoadMutationEnabled(injector, SceneLoadFaultPhase::prepare,
                "ray", scene_load_mutation::ray_as_plan)) {
            auto dropped = std::make_shared<RayScenePlanState>(*renderer_candidate.ray_scene_plan);
            dropped->resolved_rows.reset();
            renderer_candidate.ray_scene_plan = std::move(dropped);
        }
        camera_candidate = GET_MODULE(Camera).prepareSceneCameras(
            request.scene_id, scene_document);
        light_candidate = LightContainer::prepareLoad(light_entries);
        for (const auto &warning : light_candidate.warnings) {
            if (logger != nullptr) LOG_WARNING(logger, "{}", warning);
        }
        seq_candidate = GET_MODULE(SeqPlayer).prepareSceneLoad();
        vat_candidate = GET_MODULE(VatPlayer).prepareSceneLoad();
        old_entities =
            GET_MODULE(ECSCore).getTemplatePublicModule().liveEntities();
        object_ids.assign(ecs_objects.size(), invalidGameObjectId);
        created_entities.reserve(ecs_objects.size());
        destroyed_entities.reserve(old_entities.size());
        candidate_runtime_bindings.reserve(ecs_objects.size());
        candidate_object_bindings.reserve(ecs_objects.size());
        retirement_state->reserveOne();

        rng = &GET_MODULE(DeterministicRng);
        rng_candidate = rng->prepareSequence();
        pending_candidate = request.consumes_pending_request
                                ? std::optional<SceneId>{}
                                : pending_scene_id;
        event_outbox = std::make_unique<internal::PreparedEventOutbox>(
            internal::getEventRegisterer(), 1U);
        scene_loaded_event = event_outbox->prepare(
            SceneLoaded{request.scene_id});
        effect_transaction =
            std::make_unique<internal::PreparedEffectTransaction>(
                GET_MODULE(Persistence), *this);
    } catch (...) {
        return fail(SceneLoadPhase::prepare, std::current_exception());
    }

    // Every game-DLL callback, including a component JSON loader invoked by
    // early publication, must observe the same transaction-local effects.
    PendingSceneTransaction pending_transaction{
        .loader = this,
        .projection = &metadata.projection(),
        .candidate = &pending_candidate,
    };
    internal::EventOutboxScope event_scope{*event_outbox};
    internal::EffectTransactionScope effect_scope{*effect_transaction};
    RngSequenceScope rng_scope{rng_candidate};
    PendingSceneTransactionScope pending_scope{pending_transaction};

    const auto rollback = [&]() noexcept {
        if (behavior_candidate) behavior_candidate->rollback();
        if (candidates_published) {
            GET_MODULE(PhysWorld).publishPrepared(
                std::move(physics_candidate));
            if (light_published) {
                GET_MODULE(LightContainer).publishPrepared(
                    std::move(light_candidate));
            }
            GET_MODULE(Camera).publishPrepared(
                std::move(camera_candidate));
            GET_MODULE(VatPlayer).publishPreparedScene(
                std::move(vat_candidate));
            GET_MODULE(SeqPlayer).publishPreparedScene(
                std::move(seq_candidate));
            transient_models.swap(candidate_transient_models);
            runtime_only_changes = candidate_runtime_only_changes;
            runtime_object_bindings.swap(candidate_runtime_bindings);
            object_bindings.swap(candidate_object_bindings);
        }
        if (renderer_published) {
            GET_MODULE(PolygonInstanceContainer).publishPreparedScene(
                std::move(renderer_candidate));
        }
        if (procedural_published) {
            GET_MODULE(ProceduralGeometryContainer)
                .rollbackPrepared(procedural_candidate);
        }
        for (auto it = destroyed_entities.rbegin();
             it != destroyed_entities.rend(); ++it) {
            it->rollback();
        }
        const bool bypass_ecs_create_rollback =
            internal::sceneLoadMutationEnabled(
                injector, SceneLoadFaultPhase::publish, "ecs",
                scene_load_mutation::ecs_liveness);
        for (auto it = created_entities.rbegin();
             it != created_entities.rend(); ++it) {
            if (bypass_ecs_create_rollback) {
                it->finish();
            } else {
                it->rollback();
            }
        }
        if (event_outbox) {
            internal::rollbackSceneEventOutbox(*event_outbox, injector);
        }
        if (rng != nullptr) {
            internal::rollbackSceneRngCandidate(
                *rng, rng_candidate, injector);
        }
        internal::rollbackScenePendingCandidate(
            pending_scene_id, pending_candidate, injector);
    };

    try {
        auto &component_info_manager = GET_MODULE(ComponentInfoManager);
        auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
        auto &renderer = GET_MODULE(PolygonInstanceContainer);
        renderer.publishPreparedScene(std::move(renderer_candidate),
                                      phase_trace);
        renderer_published = true;

        const auto create_object = [&](std::size_t object_index) {
            auto &object = ecs_objects[object_index];
            if (object.components_id.empty() && object.behaviors.empty()) {
                return;
            }
            auto token = ECSEntityMutation::prepareCreate(
                ecs, object.components_id,
                [&](std::span<void *> ptrs) {
                    for (std::size_t i = 0; i < object.components.size(); ++i) {
                        applyComponentLoad(component_info_manager, ptrs[i],
                                           object.components[i]);
                        if (object.prepared_model_instance &&
                            object.components[i].codec != nullptr &&
                            object.components[i].codec->name ==
                                "simplemodelview") {
                            auto &model_view =
                                *static_cast<SimpleModelViewComponent *>(
                                    ptrs[i]);
                            model_view.model_instance_id =
                                object.prepared_model_instance;
                            model_view.authoring_object_id =
                                object.authoring_object_id.value;
                            model_view.dirty = false;
                        }
                    }
                });
            object_ids[object_index] = token.entity();
            token.publish();
            created_entities.push_back(std::move(token));
            candidate_runtime_bindings.push_back(
                SceneRuntimeObjectBinding{
                    .authoring_object_id = object.authoring_object_id,
                    .object_id = object_ids[object_index],
                });
        };

        for (std::size_t index = 0; index < ecs_objects.size(); ++index) {
            create_object(index);
        }

        const auto transform_id =
            component_info_manager.getComponentIdByName("transform");
        std::unordered_map<std::string, GameObjectId> object_ids_by_name;
        object_ids_by_name.reserve(ecs_objects.size());
        for (std::size_t index = 0; index < ecs_objects.size(); ++index) {
            const auto &object = ecs_objects[index];
            const auto object_id = object_ids[index];
            if (!object.name.empty() && object_id != invalidGameObjectId) {
                object_ids_by_name.emplace(object.name, object_id);
            }
            const bool has_transform =
                object_id != invalidGameObjectId &&
                std::find(object.components_id.begin(),
                          object.components_id.end(), transform_id) !=
                    object.components_id.end();
            if (!object.name.empty() && has_transform) {
                candidate_object_bindings.emplace(
                    object.name, ObjectBinding{object_id});
            }
        }

        const bool scene_uses_parents = std::any_of(
            ecs_objects.begin(), ecs_objects.end(), [](const auto &object) {
                return object.hierarchy_participant;
            });
        if (scene_uses_parents) {
            for (std::size_t index = 0; index < ecs_objects.size(); ++index) {
                const auto &object = ecs_objects[index];
                if (!object.hierarchy_participant) continue;
                auto *local = ecs.tryComponent<LocalTransformComponent>(
                    object_ids[index]);
                if (local == nullptr) {
                    throw std::runtime_error(
                        "parent hierarchy object '" +
                        displayObjectName(object.name) +
                        "' has no localtransform ECS component");
                }
                if (!object.parent.empty()) {
                    const auto parent = object_ids_by_name.find(object.parent);
                    if (parent == object_ids_by_name.end() ||
                        ecs.tryComponent<TransformComponent>(parent->second) ==
                            nullptr) {
                        throw std::runtime_error(
                            "parent '" + object.parent + "' for object '" +
                            displayObjectName(object.name) +
                            "' has no ECS transform");
                    }
                    local->parent = parent->second;
                    (void)ecs.markComponentChanged(
                        object_ids[index],
                        ComponentIdByType<LocalTransformComponent>::value);
                }
            }

            std::unordered_map<std::string, std::size_t>
                object_indices_by_name;
            object_indices_by_name.reserve(ecs_objects.size());
            for (std::size_t index = 0; index < ecs_objects.size(); ++index) {
                if (!ecs_objects[index].name.empty()) {
                    object_indices_by_name.emplace(ecs_objects[index].name,
                                                   index);
                }
            }
            std::vector<std::uint8_t> transform_state(ecs_objects.size(), 0);
            const auto project_transform =
                [&](auto &&self, std::size_t index) -> void {
                if (transform_state[index] == 2) return;
                if (transform_state[index] == 1) {
                    throw std::logic_error(
                        "validated scene hierarchy became cyclic during projection");
                }
                transform_state[index] = 1;
                const auto &object = ecs_objects[index];
                const TransformComponent *parent_world = nullptr;
                if (!object.parent.empty()) {
                    const auto parent_index =
                        object_indices_by_name.at(object.parent);
                    self(self, parent_index);
                    parent_world = ecs.tryComponent<TransformComponent>(
                        object_ids[parent_index]);
                }
                auto *world = ecs.tryComponent<TransformComponent>(
                    object_ids[index]);
                auto *local = ecs.tryComponent<LocalTransformComponent>(
                    object_ids[index]);
                TransformCodecTarget target{.world = world,
                                            .local = local,
                                            .parent_world = parent_world};
                const auto &prepared = transformLoad(object);
                prepared.codec->applyRuntime(prepared.decoded, &target);
                transform_state[index] = 2;
            };
            for (std::size_t index = 0; index < ecs_objects.size(); ++index) {
                if (ecs_objects[index].hierarchy_participant) {
                    project_transform(project_transform, index);
                }
            }
        }

        std::vector<PhysWorld::Binding> physics_bindings;
        for (std::size_t index = 0; index < ecs_objects.size(); ++index) {
            const auto &object = ecs_objects[index];
            const auto object_id = object_ids[index];
            const bool has_transform =
                object_id != invalidGameObjectId &&
                ecs.tryComponent<TransformComponent>(object_id) != nullptr;
            if (object.prepared_model_physics) {
                auto binding = *object.prepared_model_physics;
                binding.transform_source = has_transform
                    ? std::variant<GameObjectId, PhysWorldTransform>{object_id}
                    : std::variant<GameObjectId, PhysWorldTransform>{identityPhysWorldTransform()};
                physics_bindings.push_back(std::move(binding));
            }
            for (const auto &collider : object.colliders) {
                const auto identity_name = runtimeObjectIdentityName(
                    request.scene_id, object.authoring_object_id, object.name);
                physics_bindings.push_back(PhysWorld::Binding{
                    .identity = phys::ColliderIdentity{
                        .name = identity_name,
                        .entity = has_transform
                                      ? std::optional<EntityId>{object_id}
                                      : std::nullopt,
                    },
                    .metadata = phys::ColliderQueryMetadata{
                        .layer = collider.layer,
                        .mask = collider.mask,
                        .trigger = collider.trigger,
                        .one_way = collider.one_way,
                    },
                    .collider = collider,
                    .transform_source = has_transform
                                            ? std::variant<GameObjectId,
                                                           PhysWorldTransform>{
                                                  object_id}
                                            : std::variant<GameObjectId,
                                                           PhysWorldTransform>{
                                                  identityPhysWorldTransform()},
                });
            }
        }
        physics_candidate = GET_MODULE(PhysWorld).prepareSceneBindings(
            std::move(physics_bindings), object_ids);

        std::vector<BoundSceneBehaviorAttachment> bound_behaviors;
        for (std::size_t index = 0; index < ecs_objects.size(); ++index) {
            auto &object = ecs_objects[index];
            for (auto &behavior : object.behaviors) {
                bound_behaviors.push_back(BoundSceneBehaviorAttachment{
                    .prepared = std::move(behavior),
                    .entity = object_ids[index],
                });
            }
        }
        behavior_candidate =
            GET_MODULE(BehaviorAttachmentArena).prepareSceneReconciliation(
                std::move(bound_behaviors));

        for (const auto entity : old_entities) {
            auto token = ECSEntityMutation::prepareDestroy(ecs, entity);
            token.publish();
            destroyed_entities.push_back(std::move(token));
        }
    } catch (...) {
        rollback();
        return fail(SceneLoadPhase::early_publish,
                    std::current_exception());
    }

    // All remaining candidate publications are swaps over prepared storage.
    static_assert(noexcept(std::declval<PolygonInstanceContainer &>()
                               .publishPreparedScene(std::declval<
                                   PolygonInstanceContainer::PreparedSceneState &&>())));
    static_assert(noexcept(std::declval<SeqPlayer &>().publishPreparedScene(
        std::declval<SeqPlayer::PreparedSceneState &&>())));
    static_assert(noexcept(std::declval<VatPlayer &>().publishPreparedScene(
        std::declval<VatPlayer::PreparedSceneState &&>())));
    static_assert(noexcept(std::declval<Camera &>().publishPrepared(
        std::declval<Camera::PreparedSceneState &&>())));
    static_assert(noexcept(std::declval<LightContainer &>().publishPrepared(
        std::declval<LightContainer::PreparedLoad &&>())));
    static_assert(noexcept(std::declval<PhysWorld &>().publishPrepared(
        std::declval<PhysWorld::PreparedState &&>())));
    static_assert(noexcept(std::declval<ProceduralGeometryContainer &>()
                               .publishPrepared(
                                   std::declval<ProceduralGeometryContainer::PreparedSceneState &>(),
                                   std::declval<SceneLoadPhaseTrace &>())));
    static_assert(noexcept(std::declval<PreparedBehaviorSceneReconciliation &>()
                               .publish()));
    static_assert(noexcept(object_bindings.swap(
        candidate_object_bindings)));
    static_assert(noexcept(runtime_object_bindings.swap(
        candidate_runtime_bindings)));
    static_assert(noexcept(std::swap(runtime_only_changes,
                                     candidate_runtime_only_changes)));
    static_assert(noexcept(transient_models.swap(
        candidate_transient_models)));
    object_bindings.swap(candidate_object_bindings);
    runtime_object_bindings.swap(candidate_runtime_bindings);
    std::swap(runtime_only_changes, candidate_runtime_only_changes);
    transient_models.swap(candidate_transient_models);
    GET_MODULE(SeqPlayer).publishPreparedScene(std::move(seq_candidate),
                                               phase_trace);
    GET_MODULE(VatPlayer).publishPreparedScene(std::move(vat_candidate),
                                               phase_trace);
    GET_MODULE(Camera).publishPrepared(std::move(camera_candidate), phase_trace);
    if (!internal::sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::publish, "light",
            scene_load_mutation::light_directional_names)) {
        GET_MODULE(LightContainer).publishPrepared(
            std::move(light_candidate), phase_trace);
        light_published = true;
    }
    GET_MODULE(PhysWorld).publishPrepared(std::move(physics_candidate),
                                          phase_trace);
    auto &procedural = GET_MODULE(ProceduralGeometryContainer);
    if (internal::sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::publish, "procedural",
            scene_load_mutation::procedural_aabb_buffer)) {
        procedural.dropPreparedAabbBuffersForMutation(
            procedural_candidate);
    }
    if (internal::sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::publish, "procedural",
            scene_load_mutation::procedural_allocation_generation)) {
        procedural.dropPreparedAllocationGenerationForMutation(
            procedural_candidate);
    }
    if (internal::sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::publish, "procedural",
            scene_load_mutation::procedural_instance_snapshot)) {
        procedural.dropPreparedInstancesForMutation(
            procedural_candidate);
    }
    procedural.publishPrepared(procedural_candidate, phase_trace);
    procedural_published = true;
    behavior_candidate->publish(phase_trace);
    candidates_published = true;

    if (internal::sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::commit, "metadata",
            scene_load_mutation::metadata_document)) {
        metadata.publishDocumentOnlyForFaultInjection();
    }

    try {
        if (!internal::sceneLoadMutationEnabled(
                injector, SceneLoadFaultPhase::activate,
                "behavior_arena",
                scene_load_mutation::behavior_attachments)) {
            behavior_candidate->activate(phase_trace);
        }
    } catch (...) {
        const auto error = std::current_exception();
        rollback();
        return fail(SceneLoadPhase::activate, error);
    }

    // The commit point is exactly one metadata swap. Nothing after it can
    // produce PreCommitFailure.
    static_assert(noexcept(std::declval<MetadataCommitToken &>().commit(
        std::declval<SceneLoadPhaseTrace &>())));
    static_assert(noexcept(
        std::declval<PreparedBehaviorSceneReconciliation &>().finish(
            std::declval<SceneLoadPhaseTrace &>())));
    static_assert(noexcept(std::declval<ECSEntityMutationToken &>().finish(
        std::declval<SceneLoadPhaseTrace &>())));
    static_assert(noexcept(std::declval<PolygonInstanceContainer &>()
                               .enqueueSceneRetirement(
                                   std::declval<std::vector<std::uint32_t>>())));
    static_assert(noexcept(std::declval<RetirementState &>().enqueue(
        std::declval<RetirementState::Batch>(),
        std::declval<SceneLoadPhaseTrace &>())));
    static_assert(noexcept(std::declval<internal::PreparedEventOutbox &>()
                               .stageReserved(
                                   std::declval<internal::QueuedEvent>())));
    static_assert(noexcept(
        std::declval<internal::PreparedEventOutbox &>().flush(
            std::declval<SceneLoadPhaseTrace &>())));
    static_assert(noexcept(std::declval<DeterministicRng &>()
                               .publishPreparedSequence(
                                   std::declval<DeterministicRng::PreparedSequence>())));
    static_assert(noexcept(std::declval<std::optional<SceneId> &>().swap(
        std::declval<std::optional<SceneId> &>())));
    metadata.commit(phase_trace);
    scene_start_time = candidate_scene_start_time;
    graph_runtime.clear();

    effect_transaction->setRetiredObjects(destroyed_entities);
    behavior_candidate->finish(phase_trace);
    const bool bypass_transient_retire =
        internal::sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::retire, "scene_loader",
            scene_load_mutation::transient_retire);
    retirement_state->enqueue(RetirementState::Batch{
        .destroyed_entities = std::move(destroyed_entities),
        .transient_models = bypass_transient_retire
                                ? std::vector<ModelTemplate>{}
                                : std::move(candidate_transient_models),
    }, phase_trace);
    GET_MODULE(PolygonInstanceContainer).enqueueSceneRetirement(
        std::move(renderer_retire_indices), phase_trace);
    if (!internal::sceneLoadMutationEnabled(
            injector, SceneLoadFaultPhase::retire, "procedural",
            scene_load_mutation::procedural_retire_queue)) {
        GET_MODULE(ProceduralGeometryContainer).enqueueRetirement(
            std::move(procedural_candidate), phase_trace);
    }

    event_outbox->stageReserved(std::move(scene_loaded_event));
    GET_MODULE(DeterministicRng).publishPreparedSequence(rng_candidate,
                                                          phase_trace);
    phase_trace.record(SceneLoadPhase::flush);
    pending_scene_id.swap(pending_candidate);
    event_outbox->flush(phase_trace);
    static_assert(noexcept(
        std::declval<internal::PreparedEffectTransaction &>().flush(
            std::declval<SceneLoadFaultInjector *>(),
            std::declval<SceneLoadPhaseTrace &>())));
    effect_transaction->flush(injector, phase_trace);

    // The batch is an executor participant in its own right.  Recording here
    // keeps an empty scene observable while individual token completion stays
    // an implementation detail of the batch.
    phase_trace.record(SceneLoadPhase::finish);
    for (auto &token : created_entities) token.finish();

    // Public strings and JSON are deliberately materialized only after the
    // noexcept commit tail has completed.
    auto effect_errors = effect_transaction->effectErrors();
    const auto ray_plan = GET_MODULE(PolygonInstanceContainer).rayAsPlan();
    auto diagnostics = ray_plan ? rayScenePlanDiagnostics(*ray_plan) : SceneLoadDiagnostics{};
    if (!diagnostics.rejected_rows.empty() &&
        (request.root == SceneLoadRoot::startup || request.root == SceneLoadRoot::pending)) {
        LOG_WARNING(logger, "{} scene committed with unresolved ray occurrence overrides: diagnostics={}",
                    request.root == SceneLoadRoot::startup ? "startup" : "pending",
                    sceneLoadDiagnosticsJson(diagnostics).dump());
    }
    if (!effect_errors.empty()) {
        diagnostics.effect_errors = effect_errors;
        diagnostics_by_root.at(static_cast<std::size_t>(request.root)) =
            diagnostics;
        return CommittedWithEffectFailure{
            candidate_authority, std::move(effect_errors), phase_trace,
            std::move(diagnostics)};
    }
    diagnostics_by_root.at(static_cast<std::size_t>(request.root)) =
        diagnostics;
    return SceneLoadCommitted{candidate_authority, phase_trace,
                              std::move(diagnostics)};
}

void SceneLoader::requestLoad(SceneId scene_id) {
    if (active_pending_scene_transaction != nullptr &&
        active_pending_scene_transaction->loader == this) {
        if (active_pending_scene_transaction->projection->resolved().findScene(
                scene_id) == nullptr) {
            throw std::runtime_error("scene not found: " + scene_id);
        }
        *active_pending_scene_transaction->candidate = std::move(scene_id);
        return;
    }
    auto &config = GET_MODULE(ProjectBasicConfig);
    if (config.resolvedScene().findScene(scene_id) == nullptr) {
        throw std::runtime_error("scene not found: " + scene_id);
    }
    pending_scene_id = std::move(scene_id);
}

void SceneLoader::requestLoadDuringRetire(SceneId scene_id) {
    auto &config = GET_MODULE(ProjectBasicConfig);
    if (config.resolvedScene().findScene(scene_id) == nullptr) {
        throw std::runtime_error("scene not found in committed projection: " +
                                 scene_id);
    }
    if (active_pending_scene_transaction != nullptr &&
        active_pending_scene_transaction->loader == this) {
        *active_pending_scene_transaction->candidate = std::move(scene_id);
        return;
    }
    pending_scene_id = std::move(scene_id);
}

bool SceneLoader::applyPendingLoad() {
    drainRetirementQueue();
    if (!pending_scene_id) {
        return false;
    }
    auto request = SceneLoadRequest::borrowed(
        *pending_scene_id,
        GET_MODULE(ProjectBasicConfig).sceneProjectionState(),
        SceneLoadRoot::pending);
    request.consumes_pending_request = true;
    const auto result = load(std::move(request));
    (void)requireSceneLoadCommitted(result);
    if (const auto *failure =
            std::get_if<CommittedWithEffectFailure>(&result)) {
        LOG_ERROR(logger,
                  "pending scene committed with effect failure: error_code='committed_with_effect_failure@1' diagnostics={}",
                  sceneLoadDiagnosticsJson(failure->diagnostics).dump());
    }
    return true;
}

nlohmann::ordered_json SceneLoader::diagnosticsStatusJson() const {
    static constexpr std::array<std::string_view, 6> names{
        "unspecified", "startup", "rpc", "pending",
        "import_unversioned", "import_versioned"};
    auto result = nlohmann::ordered_json::object();
    for (std::size_t index = 0; index < names.size(); ++index)
        result[std::string{names[index]}] =
            sceneLoadDiagnosticsJson(diagnostics_by_root[index]);
    return result;
}

const SceneId &SceneLoader::currentScene() const {
    return GET_MODULE(ProjectBasicConfig).currentSceneId();
}

std::uint64_t SceneLoader::runtimeSceneEpoch() const noexcept {
    return GET_MODULE(ProjectBasicConfig).runtimeSceneEpoch();
}

SceneAuthority SceneLoader::activeAuthority() const noexcept {
    return GET_MODULE(ProjectBasicConfig).activeSceneAuthority();
}

void SceneLoader::drainRetirementQueue() {
    std::vector<RetirementState::Batch> pending;
    pending.swap(retirement_state->queue);
    // Consume every inverse token before deferred GPU cleanup can fail.  A
    // committed scene must never be resurrected later by a token destructor.
    for (auto &batch : pending) {
        for (auto &token : batch.destroyed_entities) token.finish();
    }
    for (auto &batch : pending) {
        for (auto &model : batch.transient_models) {
            ReleaseErrorSink::record("retirement queue model release",
                                     releaseModelGpuResources(model, true));
        }
    }
    // A prepare failure may construct SceneLoader without constructing the
    // renderer.  During shutdown GET_MODULE would incorrectly try to create
    // that absent participant, so retirement uses a non-creating lookup.
    if (auto *renderer =
            FastModuleContainer::tryGet<PolygonInstanceContainer>()) {
        renderer->drainSceneRetirements();
    }
    if (auto *procedural =
            FastModuleContainer::tryGet<ProceduralGeometryContainer>()) {
        procedural->drainRetirements();
    }
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
    GET_MODULE(ProceduralGeometryContainer).clear();
    releaseTransientModels(true);
}

void SceneLoader::releaseTransientModels(bool deferred) noexcept {
    for (auto &model : transient_models) {
        ReleaseErrorSink::record("scene teardown transient release",
                                 releaseModelGpuResources(model, deferred));
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

bool SceneLoader::ownsGraphParameter(const ValueGraph::Graph &graph, GameObjectId object,
                                    std::string_view parameter) const {
    return ownsGraphOwner(graph.parameterOwner(parameter), object);
}

SceneLoader::GraphRuntime::~GraphRuntime() {
#if PELICAN_WITH_AUDIO
    if (auto *audio = FastModuleContainer::tryGet<Audio>())
        for (const auto &[node, sound] : sounds) audio->stopSound(sound);
#endif
}

std::optional<GameObjectId> SceneLoader::graphOwnerObject(const GraphInstanceOwner &owner) const {
    if (owner.scene != currentScene()) return std::nullopt;
    for (const auto &binding : runtime_object_bindings)
        if (binding.authoring_object_id.value == owner.object &&
            GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(binding.object_id)) return binding.object_id;
    return std::nullopt;
}

bool SceneLoader::ownsGraphOwner(const GraphInstanceOwner *owner, GameObjectId object) const {
    return owner && graphOwnerObject(*owner) == object;
}

SceneLoader::GraphRuntime &SceneLoader::synchronizeGraphRuntime(
    const std::shared_ptr<const ValueGraph::Graph> &graph) {
    std::erase_if(graph_runtime, [](const auto &entry) { return entry.first.expired(); });
    auto &runtime = graph_runtime.try_emplace(graph, *graph).first->second;
    std::map<std::string, GameObjectId> owners;
    for (const auto &[scope, owner] : graph->instanceOwners()) {
        if (const auto object = graphOwnerObject(owner)) owners.emplace(scope, *object);
    }
    ValueGraph::InactiveScopes changed;
    for (const auto &[scope, object] : runtime.owners) {
        const auto next = owners.find(scope);
        if (next == owners.end() || next->second != object) changed.insert(scope);
    }
    graph->resetState(runtime.evaluation, changed);
#if PELICAN_WITH_AUDIO
    if (auto *audio = FastModuleContainer::tryGet<Audio>())
        std::erase_if(runtime.sounds, [&](const auto &entry) {
            if (!graph->nodeActive(entry.first, changed)) {
                audio->stopSound(entry.second);
                return true;
            }
            return !audio->soundState(entry.second);
        });
#endif
    runtime.owners = std::move(owners);
    auto &parameters = runtime.parameters;
    std::erase_if(parameters, [&](const auto &entry) {
        return !ownsGraphParameter(*graph, entry.second.object, entry.first);
    });
    return runtime;
}

bool SceneLoader::setGraphParameter(std::shared_ptr<const ValueGraph::Graph> graph, GameObjectId object,
                                   std::string_view parameter, std::optional<double> value) {
    if (value && !std::isfinite(*value)) throw std::invalid_argument("graph parameter must be finite");
    if (!ownsGraphParameter(*graph, object, parameter)) return false;
    auto &parameters = synchronizeGraphRuntime(graph).parameters;
    if (value) parameters.insert_or_assign(std::string{parameter}, GraphParameterOverride{object, *value});
    else parameters.erase(std::string{parameter});
    return true;
}

ValueGraph::ParameterValues SceneLoader::graphParameterValues(std::shared_ptr<const ValueGraph::Graph> graph) {
    const auto &parameters = synchronizeGraphRuntime(graph).parameters;
    ValueGraph::ParameterValues values;
    for (const auto &[name, parameter] : parameters) values.emplace(name, parameter.value);
    return values;
}

ValueGraph::EvaluationState &SceneLoader::graphEvaluationState(const std::shared_ptr<const ValueGraph::Graph> &graph) {
    return synchronizeGraphRuntime(graph).evaluation;
}

bool SceneLoader::emitGraphEvent(std::shared_ptr<const ValueGraph::Graph> graph, GameObjectId object,
                                std::string_view input, Animation::CurvePlaybackCommand command) {
    if (!ownsGraphOwner(graph->eventOwner(input), object)) return false;
    return graph->emitEvent(synchronizeGraphRuntime(graph).evaluation, input, command,
                            GET_MODULE(EngineTime).now() - scene_start_time);
}

bool SceneLoader::applyGraphAudioEvent(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                     std::string_view node, Animation::CurvePlaybackCommand command) {
#if PELICAN_WITH_AUDIO
    const auto source = graph->audioSources().find(std::string{node});
    if (source == graph->audioSources().end() || !graph->nodeActive(node, inactiveGraphScopes(*graph))) return false;
    auto &sounds = synchronizeGraphRuntime(graph).sounds;
    auto &audio = GET_MODULE(Audio);
    const auto existing = sounds.find(node);
    if (command == Animation::CurvePlaybackCommand::start) {
        const auto sound = audio.playSound(source->second);
        if (existing != sounds.end()) {
            audio.stopSound(existing->second);
            existing->second = sound;
        } else sounds.emplace(std::string{node}, sound);
    } else if (existing != sounds.end()) {
        if (command == Animation::CurvePlaybackCommand::reset) {
            audio.stopSound(existing->second);
            sounds.erase(existing);
        } else audio.setSoundPaused(existing->second, command == Animation::CurvePlaybackCommand::pause);
    }
    return true;
#else
    return false;
#endif
}

std::optional<SoundHandle> SceneLoader::graphSound(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                                std::string_view node) const {
    const auto runtime = graph_runtime.find(graph);
    if (runtime == graph_runtime.end()) return std::nullopt;
    const auto sound = runtime->second.sounds.find(node);
    return sound == runtime->second.sounds.end() ? std::nullopt : std::optional{sound->second};
}

void SceneLoader::updateGraphRuntime() {
    std::erase_if(graph_runtime, [](const auto &entry) { return entry.first.expired(); });
    std::vector<std::shared_ptr<const ValueGraph::Graph>> graphs;
    for (const auto &[weak, runtime] : graph_runtime)
        if (auto graph = weak.lock()) graphs.push_back(std::move(graph));
    for (const auto &graph : graphs) (void)synchronizeGraphRuntime(graph);
}

ValueGraph::InactiveScopes SceneLoader::inactiveGraphScopes(const ValueGraph::Graph &graph) const {
    ValueGraph::InactiveScopes inactive;
    for (const auto &[scope, owner] : graph.instanceOwners()) {
        if (!graphOwnerObject(owner)) inactive.insert(scope);
    }
    return inactive;
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

std::optional<std::string> SceneLoader::objectName(GameObjectId object) const {
    for (const auto &[name, binding] : object_bindings)
        if (binding.object_id == object) return name;
    return std::nullopt;
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
        .model = gltf_loader.commit(std::move(prepared),
                                    GltfAuthoredAssetReference{
                                        std::string{path_ref}}),
    };
    auto staged_instance = instances.stageModelInstance(candidate.model);
    const auto model_instance_id = staged_instance.id();
    const auto initial_transform = identityObjectTransform();
    std::optional<PhysWorld::PreparedState> physics_candidate;
    if (auto mesh = makeModelPhysicsBinding(candidate.model,
            name && !name->empty() ? *name : "transient:" + std::to_string(model_instance_id.index),
            PhysWorldTransform{})) {
        mesh->runtime_only = true;
        auto &physics = GET_MODULE(PhysWorld);
        auto next = physics.snapshotPrepared();
        next.bindings.push_back(std::move(*mesh));
        physics_candidate = physics.prepareBindings(std::move(next.bindings));
    }

    transient_models.push_back(std::move(candidate.model));
    candidate.owns_resources = false;

    GameObjectId object_id;
    try {
        object_id = GameObjects::createWithComponents(component_ids, [&](std::span<void *> ptrs) {
            assignTransform(*static_cast<TransformComponent *>(ptrs[0]), initial_transform);
            static_cast<SimpleModelViewComponent *>(ptrs[1])->model_instance_id = model_instance_id;
        });
    } catch (...) {
        ReleaseErrorSink::record("transient entity failure release",
                                 releaseModelGpuResources(transient_models.back(), false));
        transient_models.pop_back();
        throw;
    }

    // No operation below allocates: this is the single publication point for
    // the entity's slot, draw commands, resources, and optional name.
    if (physics_candidate) {
        auto &mesh = physics_candidate->bindings.back();
        mesh.transform_source = object_id;
        mesh.identity.entity = object_id;
        GET_MODULE(PhysWorld).publishPrepared(std::move(*physics_candidate));
    }
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
