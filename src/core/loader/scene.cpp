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
#include "sceneloadparticipant.hpp"
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
#if !PELICAN_WITH_ANIMATION
    // Without the animation module nothing evaluates an animation component,
    // so the object would be drawn in its rest pose forever. Say so at prepare
    // -- the codec still decodes and encodes the component, so an OFF build's
    // tools can still read and save the scene -- and let the load's own
    // rollback keep whatever scene was already live.
    for (const auto &object : scene.objects) {
        for (const auto &component : object.components) {
            if (component.name != "animation") continue;
            throw std::runtime_error(
                "animation.module_unavailable@1: this build has PELICAN_WITH_ANIMATION=OFF, "
                "so the animation component on object '" +
                object.name.value_or(std::to_string(object.authoring_object_id.value)) +
                "' cannot be evaluated");
        }
    }
#endif
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

// The per-object load plan the scene-load participants share.  It has
// external linkage on purpose: SceneLoadContext holds a vector of these, and
// a transaction state visible from the header must not be built out of
// anonymous-namespace types.
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

namespace {

void runSceneLoadVerb(SceneLoadParticipant &participant,
                      SceneLoadVerb verb) {
    switch (verb) {
    case SceneLoadVerb::prepare: participant.prepare(); return;
    case SceneLoadVerb::early_publish: participant.earlyPublish(); return;
    case SceneLoadVerb::publish: participant.publish(); return;
    case SceneLoadVerb::activate: participant.activate(); return;
    case SceneLoadVerb::commit: participant.commit(); return;
    case SceneLoadVerb::retire: participant.retire(); return;
    case SceneLoadVerb::flush: participant.flush(); return;
    case SceneLoadVerb::finish: participant.finish(); return;
    case SceneLoadVerb::rollback: participant.rollback(); return;
    }
}

} // namespace

bool SceneLoadMutationGate::applyBefore(SceneLoadParticipant &participant,
                                        SceneLoadVerb verb) const noexcept {
    bool skip = false;
    for (const auto &point : participant.mutationPoints()) {
        if (point.verb != verb || point.after) continue;
        if (internal::sceneLoadMutationEnabled(injector_, point.fault_phase,
                                               point.participant,
                                               point.mutation_id)) {
            skip = participant.mutateBefore(point.mutation_id) || skip;
        }
    }
    return skip;
}

void SceneLoadMutationGate::run(SceneLoadParticipant &participant,
                                SceneLoadVerb verb) const {
    if (!applyBefore(participant, verb)) {
        runSceneLoadVerb(participant, verb);
    }
    for (const auto &point : participant.mutationPoints()) {
        if (point.verb != verb || !point.after) continue;
        if (internal::sceneLoadMutationEnabled(injector_, point.fault_phase,
                                               point.participant,
                                               point.mutation_id)) {
            participant.mutateAfter(point.mutation_id);
        }
    }
}

MetadataCommitToken SceneLoader::prepareLoadMetadata(
    ProjectBasicConfig &config, SceneLoadRequest &request,
    SceneAuthority authority, SceneLoadPhaseTrace &phase_trace) {
    MetadataCommitToken metadata;
    if (auto *owned =
            std::get_if<SceneLoadRequest::Owned>(&request.source)) {
        metadata = std::move(owned->token);
    } else {
        metadata = config.prepareBorrowedSceneMetadata(
            std::get<SceneLoadRequest::Borrowed>(request.source)
                .committed.get());
    }
    metadata.prepareRuntimeIdentity(request.scene_id, authority, phase_trace);
    return metadata;
}

void SceneLoader::publishLoadMetadataDocumentOnly(
    MetadataCommitToken &metadata) noexcept {
    metadata.publishDocumentOnlyForFaultInjection();
}

void SceneLoader::commitLoadMetadata(
    MetadataCommitToken &metadata,
    SceneLoadPhaseTrace &phase_trace) noexcept {
    static_assert(noexcept(std::declval<MetadataCommitToken &>().commit(
        std::declval<SceneLoadPhaseTrace &>())));
    metadata.commit(phase_trace);
}

// What used to be load()'s ~30 candidate locals.  Participants own the
// candidates they prepare and publish; this is where those candidates live
// for the length of one transaction, together with the handful of helpers
// that reach into SceneLoader's own fields -- the only friendship the whole
// participant set needs.
struct SceneLoadContext {
    SceneLoader &loader;
    SceneLoadRequest &request;
    ProjectBasicConfig &config;
    SceneLoadFaultInjector *injector = nullptr;
    SceneLoadPhaseTrace &phase_trace;
    SceneAuthority authority{};

    const ResolvedSceneView *scene = nullptr;
    std::vector<LightLoadEntry> light_entries;

    // Declaration order below is the order the old locals were declared in,
    // so candidates are still destroyed in the same order at the end of a
    // load: the metadata token last, after every candidate it outlived.
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
    std::unordered_map<std::string, SceneLoader::ObjectBinding>
        candidate_object_bindings;
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
    double candidate_scene_start_time = 0;

    // -- metadata ---------------------------------------------------------
    void prepareMetadata() {
        metadata = SceneLoader::prepareLoadMetadata(config, request, authority,
                                                    phase_trace);
    }
    void publishMetadataDocumentOnly() noexcept {
        SceneLoader::publishLoadMetadataDocumentOnly(metadata);
    }
    void commitMetadata() noexcept {
        SceneLoader::commitLoadMetadata(metadata, phase_trace);
    }

    // -- SceneLoader-owned state ------------------------------------------
    void reserveCandidateBindings(std::size_t object_count) {
        candidate_runtime_bindings.reserve(object_count);
        candidate_object_bindings.reserve(object_count);
        loader.retirement_state->reserveOne();
    }

    void bindCandidateObject(const std::string &name,
                             GameObjectId object_id) {
        candidate_object_bindings.emplace(
            name, SceneLoader::ObjectBinding{object_id});
    }

    void publishLoaderState() noexcept {
        static_assert(noexcept(loader.object_bindings.swap(
            candidate_object_bindings)));
        static_assert(noexcept(loader.runtime_object_bindings.swap(
            candidate_runtime_bindings)));
        static_assert(noexcept(std::swap(loader.runtime_only_changes,
                                         candidate_runtime_only_changes)));
        static_assert(noexcept(loader.transient_models.swap(
            candidate_transient_models)));
        loader.object_bindings.swap(candidate_object_bindings);
        loader.runtime_object_bindings.swap(candidate_runtime_bindings);
        std::swap(loader.runtime_only_changes,
                  candidate_runtime_only_changes);
        loader.transient_models.swap(candidate_transient_models);
    }

    void rollbackLoaderState() noexcept {
        loader.transient_models.swap(candidate_transient_models);
        loader.runtime_only_changes = candidate_runtime_only_changes;
        loader.runtime_object_bindings.swap(candidate_runtime_bindings);
        loader.object_bindings.swap(candidate_object_bindings);
    }

    void commitSceneIdentity() noexcept {
        loader.scene_start_time = candidate_scene_start_time;
        loader.graph_runtime.clear();
        // The unmatched-owner memos belong to the scene that was live, not to
        // the one being committed: a document that could not bind an object
        // there says nothing about what this scene has.
        loader.graph_owner_resolved_scopes.clear();
        loader.graph_owner_reported_scopes.clear();
    }

    void enqueueRetirementBatch(
        std::vector<ECSEntityMutationToken> destroyed,
        std::vector<ModelTemplate> transient) noexcept {
        static_assert(noexcept(
            std::declval<SceneLoader::RetirementState &>().enqueue(
                std::declval<SceneLoader::RetirementState::Batch>(),
                std::declval<SceneLoadPhaseTrace &>())));
        loader.retirement_state->enqueue(
            SceneLoader::RetirementState::Batch{
                .destroyed_entities = std::move(destroyed),
                .transient_models = std::move(transient),
            },
            phase_trace);
    }

    const std::optional<SceneId> &livePendingScene() const noexcept {
        return loader.pending_scene_id;
    }

    void publishPendingScene() noexcept {
        static_assert(noexcept(std::declval<std::optional<SceneId> &>().swap(
            std::declval<std::optional<SceneId> &>())));
        loader.pending_scene_id.swap(pending_candidate);
    }

    void rollbackPendingScene() noexcept {
        internal::rollbackScenePendingCandidate(loader.pending_scene_id,
                                                pending_candidate, injector);
    }
};

namespace {

// Every participant below differs only in which verbs it implements; the
// transaction reference is all they share.
struct TransactionParticipant : SceneLoadParticipant {
    SceneLoadContext &t;

    explicit TransactionParticipant(SceneLoadContext &transaction) noexcept
        : t{transaction} {}
};

// The one metadata candidate.  Its publication is the commit point, so it
// prepares first and publishes last: the document-only mutation has to land
// at the very end of publication, where the activation window can observe a
// torn commit.
struct MetadataParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    static constexpr std::array points{
        SceneLoadMutationPoint{SceneLoadVerb::prepare,
                               SceneLoadFaultPhase::prepare, "metadata",
                               scene_load_mutation::metadata_projection,
                               true},
        SceneLoadMutationPoint{SceneLoadVerb::publish,
                               SceneLoadFaultPhase::commit, "metadata",
                               scene_load_mutation::metadata_document, false},
    };

    std::string_view name() const noexcept override { return "metadata"; }
    std::span<const SceneLoadMutationPoint>
    mutationPoints() const noexcept override {
        return points;
    }
    bool mutateBefore(std::string_view) noexcept override {
        t.publishMetadataDocumentOnly();
        return false;
    }
    void mutateAfter(std::string_view) override {
        throw std::runtime_error("injected scene metadata prepare drop");
    }
    void prepare() override { t.prepareMetadata(); }
    void commit() override { t.commitMetadata(); }
};

// The resolved-projection gate.  Nothing is staged here; it only decides
// whether the rest of the transaction may run at all.
struct SceneDocumentParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    std::string_view name() const noexcept override {
        return "scene_document";
    }
    void prepare() override {
        const auto &projection = t.metadata.projection();
        const auto &scene_document = projection.resolved();
        for (const auto &warning : scene_document.warnings()) {
            if (logger != nullptr) LOG_WARNING(logger, "{}", warning);
        }
        t.scene = scene_document.findScene(t.request.scene_id);
        if (t.scene == nullptr) {
            throw std::runtime_error("scene not found: " + t.request.scene_id);
        }
        internal::validateResolvedSceneFeatureBindings(*t.scene);
    }
};

// SceneLoader's own published state: the two binding maps, the runtime-only
// flag, the transient model list and the scene clock.  Its retirement also
// carries the ECS destroy tokens, because one batch is what the next frame
// drains.
struct LoaderStateParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    static constexpr std::array points{
        SceneLoadMutationPoint{SceneLoadVerb::retire,
                               SceneLoadFaultPhase::retire, "scene_loader",
                               scene_load_mutation::transient_retire, false},
    };

    bool published_ = false;
    bool drop_transient_ = false;

    std::string_view name() const noexcept override { return "scene_loader"; }
    std::span<const SceneLoadMutationPoint>
    mutationPoints() const noexcept override {
        return points;
    }
    bool mutateBefore(std::string_view) noexcept override {
        drop_transient_ = true;
        return false;
    }
    void prepare() override {
        t.candidate_scene_start_time = GET_MODULE(EngineTime).now();
    }
    void publish() noexcept override {
        t.publishLoaderState();
        published_ = true;
    }
    void commit() override { t.commitSceneIdentity(); }
    void retire() override {
        t.enqueueRetirementBatch(
            std::move(t.destroyed_entities),
            drop_transient_ ? std::vector<ModelTemplate>{}
                            : std::move(t.candidate_transient_models));
    }
    void rollback() noexcept override {
        if (published_) t.rollbackLoaderState();
    }
};

struct ProceduralParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    static constexpr std::array points{
        SceneLoadMutationPoint{
            SceneLoadVerb::publish, SceneLoadFaultPhase::publish,
            "procedural", scene_load_mutation::procedural_aabb_buffer, false},
        SceneLoadMutationPoint{
            SceneLoadVerb::publish, SceneLoadFaultPhase::publish,
            "procedural", scene_load_mutation::procedural_allocation_generation,
            false},
        SceneLoadMutationPoint{
            SceneLoadVerb::publish, SceneLoadFaultPhase::publish,
            "procedural", scene_load_mutation::procedural_instance_snapshot,
            false},
        SceneLoadMutationPoint{
            SceneLoadVerb::retire, SceneLoadFaultPhase::retire, "procedural",
            scene_load_mutation::procedural_retire_queue, false},
    };

    bool published_ = false;

    std::string_view name() const noexcept override { return "procedural"; }
    std::span<const SceneLoadMutationPoint>
    mutationPoints() const noexcept override {
        return points;
    }
    bool mutateBefore(std::string_view mutation_id) noexcept override {
        if (mutation_id == scene_load_mutation::procedural_retire_queue) {
            return true;
        }
        auto &procedural = GET_MODULE(ProceduralGeometryContainer);
        if (mutation_id == scene_load_mutation::procedural_aabb_buffer) {
            procedural.dropPreparedAabbBuffersForMutation(
                t.procedural_candidate);
        } else if (mutation_id ==
                   scene_load_mutation::procedural_allocation_generation) {
            procedural.dropPreparedAllocationGenerationForMutation(
                t.procedural_candidate);
        } else {
            procedural.dropPreparedInstancesForMutation(
                t.procedural_candidate);
        }
        return false;
    }
    void prepare() override {
        const bool has_procedural = std::any_of(
            t.scene->objects.begin(), t.scene->objects.end(),
            [](const auto &object) {
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
                nlohmann::json::parse(t.config.assetDataJson()));
            procedural_vkcore = &GET_MODULE(VulkanManageCore);
        }
        t.procedural_candidate =
            GET_MODULE(ProceduralGeometryContainer).prepareScene(
                asset_data, *t.scene, procedural_vkcore,
                t.authority.scene_epoch, t.phase_trace);
    }
    void publish() noexcept override {
        static_assert(noexcept(
            std::declval<ProceduralGeometryContainer &>().publishPrepared(
                std::declval<ProceduralGeometryContainer::PreparedSceneState &>(),
                std::declval<SceneLoadPhaseTrace &>())));
        GET_MODULE(ProceduralGeometryContainer).publishPrepared(
            t.procedural_candidate, t.phase_trace);
        published_ = true;
    }
    void retire() override {
        GET_MODULE(ProceduralGeometryContainer).enqueueRetirement(
            std::move(t.procedural_candidate), t.phase_trace);
    }
    void rollback() noexcept override {
        if (published_) {
            GET_MODULE(ProceduralGeometryContainer)
                .rollbackPrepared(t.procedural_candidate);
        }
    }
};

// The per-object load plan every later participant reads: ECS component
// values, colliders, behaviors, and the light entries the light candidate is
// built from.
struct SceneBindingsParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    std::string_view name() const noexcept override {
        return "scene_bindings";
    }
    void prepare() override {
        auto &component_info_manager = GET_MODULE(ComponentInfoManager);
        const auto game_logic_status = configuredGameLogicStatus();
        const auto behavior_availability =
            game_logic_status.loaded
                ? BehaviorRegistryAvailability::active
                : BehaviorRegistryAvailability::dll_unavailable;
        auto behavior_entries = prepareResolvedSceneBehaviorAttachments(
            t.scene->objects, behavior_availability);
        t.ecs_objects = prepareSceneBindings(
            *t.scene, component_info_manager, t.light_entries,
            std::move(behavior_entries));
    }
};

// The flat renderer's scene replacement, the staged model instances and the
// ray scene plan.  It publishes before ECS entities exist because a created
// entity's simplemodelview already names a prepared instance slot.
struct RendererParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    static constexpr std::array points{
        SceneLoadMutationPoint{SceneLoadVerb::prepare,
                               SceneLoadFaultPhase::prepare, "ray",
                               scene_load_mutation::ray_occurrence_catalog,
                               true},
        SceneLoadMutationPoint{SceneLoadVerb::prepare,
                               SceneLoadFaultPhase::prepare, "ray",
                               scene_load_mutation::ray_as_plan, true},
    };

    bool published_ = false;

    std::string_view name() const noexcept override {
        return "renderer.flat";
    }
    std::span<const SceneLoadMutationPoint>
    mutationPoints() const noexcept override {
        return points;
    }
    void mutateAfter(std::string_view mutation_id) override {
        auto dropped = std::make_shared<RayScenePlanState>(
            *t.renderer_candidate.ray_scene_plan);
        if (mutation_id == scene_load_mutation::ray_occurrence_catalog) {
            dropped->inventory = std::make_shared<RaySceneInventorySnapshot>();
        } else {
            dropped->resolved_rows.reset();
        }
        t.renderer_candidate.ray_scene_plan = std::move(dropped);
    }
    void prepare() override {
        auto &component_info_manager = GET_MODULE(ComponentInfoManager);
        auto &renderer = GET_MODULE(PolygonInstanceContainer);
        t.renderer_candidate =
            renderer.prepareSceneReplacement(t.renderer_retire_indices);
        for (auto &object : t.ecs_objects) {
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
                    model, runtimeObjectIdentityName(t.request.scene_id,
                        object.authoring_object_id, object.name), PhysWorldTransform{});
                auto staged =
                    renderer.stagePreparedSceneModel(t.renderer_candidate,
                                                     model);
                object.prepared_model_instance = staged.id();
                renderer.publishPreparedSceneModel(t.renderer_candidate,
                                                    std::move(staged));
                t.renderer_candidate.slots.ray_authoring_object_ids.at(
                    object.prepared_model_instance->index) = object.authoring_object_id.value;
                break;
            }
        }
        const auto ray_project = loadRayTracingProjectConfig(
            loadRenderingPassConfigJsonFromString(t.config.renderingConfigJson(),
                                                 "scene ray plan prepare"),
            [](std::string_view reference) { return GET_MODULE(PathResolver).loadText(reference); },
            PELICAN_RUNTIME_SHADER_COMPILER != 0);
        renderer.prepareRayScenePlan(t.renderer_candidate, ray_project, t.scene,
            GET_MODULE(ProceduralGeometryContainer).preparedRayInstances(*t.scene, &t.procedural_candidate),
            &t.phase_trace);
    }
    void earlyPublish() override {
        static_assert(noexcept(std::declval<PolygonInstanceContainer &>()
                                   .publishPreparedScene(std::declval<
                                       PolygonInstanceContainer::PreparedSceneState &&>())));
        GET_MODULE(PolygonInstanceContainer).publishPreparedScene(
            std::move(t.renderer_candidate), t.phase_trace);
        published_ = true;
    }
    void retire() override {
        static_assert(noexcept(std::declval<PolygonInstanceContainer &>()
                                   .enqueueSceneRetirement(
                                       std::declval<std::vector<std::uint32_t>>())));
        GET_MODULE(PolygonInstanceContainer).enqueueSceneRetirement(
            std::move(t.renderer_retire_indices), t.phase_trace);
    }
    void rollback() noexcept override {
        if (published_) {
            GET_MODULE(PolygonInstanceContainer).publishPreparedScene(
                std::move(t.renderer_candidate));
        }
    }
};

struct CameraParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    bool published_ = false;

    std::string_view name() const noexcept override { return "camera"; }
    void prepare() override {
        t.camera_candidate = GET_MODULE(Camera).prepareSceneCameras(
            t.request.scene_id, t.metadata.projection().resolved());
    }
    void publish() noexcept override {
        static_assert(noexcept(std::declval<Camera &>().publishPrepared(
            std::declval<Camera::PreparedSceneState &&>())));
        GET_MODULE(Camera).publishPrepared(std::move(t.camera_candidate),
                                           t.phase_trace);
        published_ = true;
    }
    void rollback() noexcept override {
        if (published_) {
            GET_MODULE(Camera).publishPrepared(
                std::move(t.camera_candidate));
        }
    }
};

struct LightParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    static constexpr std::array points{
        SceneLoadMutationPoint{
            SceneLoadVerb::publish, SceneLoadFaultPhase::publish, "light",
            scene_load_mutation::light_directional_names, false},
    };

    bool published_ = false;

    std::string_view name() const noexcept override { return "light"; }
    std::span<const SceneLoadMutationPoint>
    mutationPoints() const noexcept override {
        return points;
    }
    bool mutateBefore(std::string_view) noexcept override { return true; }
    void prepare() override {
        t.light_candidate = LightContainer::prepareLoad(t.light_entries);
        for (const auto &warning : t.light_candidate.warnings) {
            if (logger != nullptr) LOG_WARNING(logger, "{}", warning);
        }
    }
    void publish() noexcept override {
        static_assert(noexcept(std::declval<LightContainer &>().publishPrepared(
            std::declval<LightContainer::PreparedLoad &&>())));
        GET_MODULE(LightContainer).publishPrepared(
            std::move(t.light_candidate), t.phase_trace);
        published_ = true;
    }
    void rollback() noexcept override {
        if (published_) {
            GET_MODULE(LightContainer).publishPrepared(
                std::move(t.light_candidate));
        }
    }
};

struct SeqPlayerParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    bool published_ = false;

    std::string_view name() const noexcept override { return "seqplayer"; }
    void prepare() override {
        t.seq_candidate = GET_MODULE(SeqPlayer).prepareSceneLoad();
    }
    void publish() noexcept override {
        static_assert(noexcept(std::declval<SeqPlayer &>().publishPreparedScene(
            std::declval<SeqPlayer::PreparedSceneState &&>())));
        GET_MODULE(SeqPlayer).publishPreparedScene(std::move(t.seq_candidate),
                                                   t.phase_trace);
        published_ = true;
    }
    void rollback() noexcept override {
        if (published_) {
            GET_MODULE(SeqPlayer).publishPreparedScene(
                std::move(t.seq_candidate));
        }
    }
};

struct VatPlayerParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    bool published_ = false;

    std::string_view name() const noexcept override { return "vatplayer"; }
    void prepare() override {
        t.vat_candidate = GET_MODULE(VatPlayer).prepareSceneLoad();
    }
    void publish() noexcept override {
        static_assert(noexcept(std::declval<VatPlayer &>().publishPreparedScene(
            std::declval<VatPlayer::PreparedSceneState &&>())));
        GET_MODULE(VatPlayer).publishPreparedScene(std::move(t.vat_candidate),
                                                   t.phase_trace);
        published_ = true;
    }
    void rollback() noexcept override {
        if (published_) {
            GET_MODULE(VatPlayer).publishPreparedScene(
                std::move(t.vat_candidate));
        }
    }
};

// Entity creation, the component values written into the new entities, and
// the parent/world transform closure over them.  Its inverse token is per
// entity, so rollback walks the created entities in reverse.
struct EcsParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    static constexpr std::array points{
        SceneLoadMutationPoint{SceneLoadVerb::rollback,
                               SceneLoadFaultPhase::publish, "ecs",
                               scene_load_mutation::ecs_liveness, false},
    };

    bool bypass_create_rollback_ = false;

    std::string_view name() const noexcept override { return "ecs"; }
    std::span<const SceneLoadMutationPoint>
    mutationPoints() const noexcept override {
        return points;
    }
    bool mutateBefore(std::string_view) noexcept override {
        bypass_create_rollback_ = true;
        return false;
    }
    void prepare() override {
        t.old_entities =
            GET_MODULE(ECSCore).getTemplatePublicModule().liveEntities();
        t.object_ids.assign(t.ecs_objects.size(), invalidGameObjectId);
        t.created_entities.reserve(t.ecs_objects.size());
        t.destroyed_entities.reserve(t.old_entities.size());
        // The candidate binding maps and the retirement slot are reserved
        // here because their size is the object count this participant is
        // about to create.
        t.reserveCandidateBindings(t.ecs_objects.size());
    }
    void earlyPublish() override {
        auto &component_info_manager = GET_MODULE(ComponentInfoManager);
        auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();

        const auto create_object = [&](std::size_t object_index) {
            auto &object = t.ecs_objects[object_index];
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
            t.object_ids[object_index] = token.entity();
            token.publish();
            t.created_entities.push_back(std::move(token));
            t.candidate_runtime_bindings.push_back(
                SceneRuntimeObjectBinding{
                    .authoring_object_id = object.authoring_object_id,
                    .object_id = t.object_ids[object_index],
                });
        };

        for (std::size_t index = 0; index < t.ecs_objects.size(); ++index) {
            create_object(index);
        }

        const auto transform_id =
            component_info_manager.getComponentIdByName("transform");
        std::unordered_map<std::string, GameObjectId> object_ids_by_name;
        object_ids_by_name.reserve(t.ecs_objects.size());
        for (std::size_t index = 0; index < t.ecs_objects.size(); ++index) {
            const auto &object = t.ecs_objects[index];
            const auto object_id = t.object_ids[index];
            if (!object.name.empty() && object_id != invalidGameObjectId) {
                object_ids_by_name.emplace(object.name, object_id);
            }
            const bool has_transform =
                object_id != invalidGameObjectId &&
                std::find(object.components_id.begin(),
                          object.components_id.end(), transform_id) !=
                    object.components_id.end();
            if (!object.name.empty() && has_transform) {
                t.bindCandidateObject(object.name, object_id);
            }
        }

        const bool scene_uses_parents = std::any_of(
            t.ecs_objects.begin(), t.ecs_objects.end(), [](const auto &object) {
                return object.hierarchy_participant;
            });
        if (scene_uses_parents) {
            for (std::size_t index = 0; index < t.ecs_objects.size(); ++index) {
                const auto &object = t.ecs_objects[index];
                if (!object.hierarchy_participant) continue;
                auto *local = ecs.tryComponent<LocalTransformComponent>(
                    t.object_ids[index]);
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
                        t.object_ids[index],
                        ComponentIdByType<LocalTransformComponent>::value);
                }
            }

            std::unordered_map<std::string, std::size_t>
                object_indices_by_name;
            object_indices_by_name.reserve(t.ecs_objects.size());
            for (std::size_t index = 0; index < t.ecs_objects.size(); ++index) {
                if (!t.ecs_objects[index].name.empty()) {
                    object_indices_by_name.emplace(t.ecs_objects[index].name,
                                                   index);
                }
            }
            // Only hierarchy participants are started; the parent link was
            // validated just above, so the lookup stays `.at()` and a broken
            // link keeps raising its std::out_of_range here.
            resolveParentOrder(
                t.ecs_objects.size(),
                [&](std::size_t index) {
                    return t.ecs_objects[index].hierarchy_participant;
                },
                [&](std::size_t index) {
                    const auto &object = t.ecs_objects[index];
                    if (object.parent.empty()) return ParentLookupResult::root();
                    return ParentLookupResult::at(
                        object_indices_by_name.at(object.parent));
                },
                [&](std::size_t index, const std::size_t *parent_index) {
                    const auto &object = t.ecs_objects[index];
                    auto *world = ecs.tryComponent<TransformComponent>(
                        t.object_ids[index]);
                    auto *local = ecs.tryComponent<LocalTransformComponent>(
                        t.object_ids[index]);
                    const TransformComponent *parent_world =
                        parent_index == nullptr
                            ? nullptr
                            : ecs.tryComponent<TransformComponent>(
                                  t.object_ids[*parent_index]);
                    TransformCodecTarget target{.world = world,
                                                .local = local,
                                                .parent_world = parent_world};
                    const auto &prepared = transformLoad(object);
                    prepared.codec->applyRuntime(prepared.decoded, &target);
                },
                [](std::size_t) {
                    throw std::logic_error(
                        "validated scene hierarchy became cyclic during projection");
                },
                [](std::size_t) {
                    // Unreachable: the lookup above is `.at()`, which has
                    // already thrown, so no parent is ever reported missing.
                });
        }
    }
    void finish() override {
        static_assert(noexcept(std::declval<ECSEntityMutationToken &>().finish(
            std::declval<SceneLoadPhaseTrace &>())));
        for (auto &token : t.created_entities) token.finish();
    }
    void rollback() noexcept override {
        for (auto it = t.created_entities.rbegin();
             it != t.created_entities.rend(); ++it) {
            if (bypass_create_rollback_) {
                it->finish();
            } else {
                it->rollback();
            }
        }
    }
};

// The other half of the ECS lifetime: the previous scene's entities are
// destroyed only after every new binding has been staged, and their inverse
// tokens are what the retirement batch carries.
struct EcsRetireParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    std::string_view name() const noexcept override { return "ecs.retire"; }
    void earlyPublish() override {
        auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
        for (const auto entity : t.old_entities) {
            auto token = ECSEntityMutation::prepareDestroy(ecs, entity);
            token.publish();
            t.destroyed_entities.push_back(std::move(token));
        }
    }
    void rollback() noexcept override {
        for (auto it = t.destroyed_entities.rbegin();
             it != t.destroyed_entities.rend(); ++it) {
            it->rollback();
        }
    }
};

struct PhysicsParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    bool published_ = false;

    std::string_view name() const noexcept override { return "physics"; }
    void earlyPublish() override {
        auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
        std::vector<PhysWorld::Binding> physics_bindings;
        for (std::size_t index = 0; index < t.ecs_objects.size(); ++index) {
            const auto &object = t.ecs_objects[index];
            const auto object_id = t.object_ids[index];
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
                    t.request.scene_id, object.authoring_object_id, object.name);
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
        t.physics_candidate = GET_MODULE(PhysWorld).prepareSceneBindings(
            std::move(physics_bindings), t.object_ids);
    }
    void publish() noexcept override {
        static_assert(noexcept(std::declval<PhysWorld &>().publishPrepared(
            std::declval<PhysWorld::PreparedState &&>())));
        GET_MODULE(PhysWorld).publishPrepared(std::move(t.physics_candidate),
                                              t.phase_trace);
        published_ = true;
    }
    void rollback() noexcept override {
        if (published_) {
            GET_MODULE(PhysWorld).publishPrepared(
                std::move(t.physics_candidate));
        }
    }
};

// The only participant with an activation window: attachment construction
// runs game-DLL code that may still fail the load.
struct BehaviorParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    static constexpr std::array points{
        SceneLoadMutationPoint{
            SceneLoadVerb::activate, SceneLoadFaultPhase::activate,
            "behavior_arena", scene_load_mutation::behavior_attachments,
            false},
    };

    std::string_view name() const noexcept override {
        return "behavior_arena";
    }
    std::span<const SceneLoadMutationPoint>
    mutationPoints() const noexcept override {
        return points;
    }
    bool mutateBefore(std::string_view) noexcept override { return true; }
    void earlyPublish() override {
        std::vector<BoundSceneBehaviorAttachment> bound_behaviors;
        for (std::size_t index = 0; index < t.ecs_objects.size(); ++index) {
            auto &object = t.ecs_objects[index];
            for (auto &behavior : object.behaviors) {
                bound_behaviors.push_back(BoundSceneBehaviorAttachment{
                    .prepared = std::move(behavior),
                    .entity = t.object_ids[index],
                });
            }
        }
        t.behavior_candidate =
            GET_MODULE(BehaviorAttachmentArena).prepareSceneReconciliation(
                std::move(bound_behaviors));
    }
    void publish() noexcept override {
        static_assert(noexcept(std::declval<PreparedBehaviorSceneReconciliation &>()
                                   .publish()));
        t.behavior_candidate->publish(t.phase_trace);
    }
    void activate() override { t.behavior_candidate->activate(t.phase_trace); }
    void commit() override {
        static_assert(noexcept(
            std::declval<PreparedBehaviorSceneReconciliation &>().finish(
                std::declval<SceneLoadPhaseTrace &>())));
        t.behavior_candidate->finish(t.phase_trace);
    }
    void rollback() noexcept override {
        if (t.behavior_candidate) t.behavior_candidate->rollback();
    }
};

struct RngParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    std::string_view name() const noexcept override { return "rng_sequence"; }
    void prepare() override {
        t.rng = &GET_MODULE(DeterministicRng);
        t.rng_candidate = t.rng->prepareSequence();
    }
    void flush() override {
        static_assert(noexcept(std::declval<DeterministicRng &>()
                                   .publishPreparedSequence(
                                       std::declval<DeterministicRng::PreparedSequence>())));
        GET_MODULE(DeterministicRng).publishPreparedSequence(t.rng_candidate,
                                                             t.phase_trace);
    }
    void rollback() noexcept override {
        if (t.rng != nullptr) {
            internal::rollbackSceneRngCandidate(*t.rng, t.rng_candidate,
                                                t.injector);
        }
    }
};

struct PendingSceneParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    std::string_view name() const noexcept override {
        return "pending_scene";
    }
    void prepare() override {
        t.pending_candidate = t.request.consumes_pending_request
                                  ? std::optional<SceneId>{}
                                  : t.livePendingScene();
    }
    void flush() override { t.publishPendingScene(); }
    void rollback() noexcept override { t.rollbackPendingScene(); }
};

struct EventOutboxParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    std::string_view name() const noexcept override { return "event_outbox"; }
    void prepare() override {
        t.event_outbox = std::make_unique<internal::PreparedEventOutbox>(
            internal::getEventRegisterer(), 1U);
        t.scene_loaded_event = t.event_outbox->prepare(
            SceneLoaded{t.request.scene_id});
    }
    void retire() override {
        static_assert(noexcept(std::declval<internal::PreparedEventOutbox &>()
                                   .stageReserved(
                                       std::declval<internal::QueuedEvent>())));
        t.event_outbox->stageReserved(std::move(t.scene_loaded_event));
    }
    void flush() override {
        static_assert(noexcept(
            std::declval<internal::PreparedEventOutbox &>().flush(
                std::declval<SceneLoadPhaseTrace &>())));
        t.event_outbox->flush(t.phase_trace);
    }
    void rollback() noexcept override {
        if (t.event_outbox) {
            internal::rollbackSceneEventOutbox(*t.event_outbox, t.injector);
        }
    }
};

struct EffectsParticipant final : TransactionParticipant {
    using TransactionParticipant::TransactionParticipant;

    std::string_view name() const noexcept override { return "effects"; }
    void prepare() override {
        t.effect_transaction =
            std::make_unique<internal::PreparedEffectTransaction>(
                GET_MODULE(Persistence), t.loader);
    }
    void commit() override {
        t.effect_transaction->setRetiredObjects(t.destroyed_entities);
    }
    void flush() override {
        static_assert(noexcept(
            std::declval<internal::PreparedEffectTransaction &>().flush(
                std::declval<SceneLoadFaultInjector *>(),
                std::declval<SceneLoadPhaseTrace &>())));
        t.effect_transaction->flush(t.injector, t.phase_trace);
    }
};

} // namespace

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
    SceneLoadContext transaction{
        .loader = *this,
        .request = request,
        .config = config,
        .injector = injector,
        .phase_trace = phase_trace,
        .authority = candidate_authority,
    };

    MetadataParticipant metadata{transaction};
    SceneDocumentParticipant scene_document{transaction};
    LoaderStateParticipant loader_state{transaction};
    ProceduralParticipant procedural{transaction};
    SceneBindingsParticipant scene_bindings{transaction};
    RendererParticipant renderer{transaction};
    CameraParticipant camera{transaction};
    LightParticipant light{transaction};
    SeqPlayerParticipant seq_player{transaction};
    VatPlayerParticipant vat_player{transaction};
    EcsParticipant ecs{transaction};
    PhysicsParticipant physics{transaction};
    BehaviorParticipant behavior{transaction};
    EcsRetireParticipant ecs_retire{transaction};
    RngParticipant rng{transaction};
    PendingSceneParticipant pending_scene{transaction};
    EventOutboxParticipant event_outbox{transaction};
    EffectsParticipant effects{transaction};

    const SceneLoadMutationGate gate{injector};

    // These orders are the transaction's specification, and they are not all
    // the same list: the renderer publishes before ECS entities exist, the
    // metadata document lands last so a torn commit is observable from the
    // activation window, and rollback is the reverse of publication only for
    // the candidate-swap participants -- the renderer's swap-back and the
    // procedural container's prepared-resource release trail the rest, as
    // they always have.
    SceneLoadParticipant *const prepare_order[] = {
        &metadata,    &scene_document, &loader_state, &procedural,
        &scene_bindings, &renderer,    &camera,       &light,
        &seq_player,  &vat_player,     &ecs,          &rng,
        &pending_scene, &event_outbox, &effects};
    SceneLoadParticipant *const early_publish_order[] = {
        &renderer, &ecs, &physics, &behavior, &ecs_retire};
    SceneLoadParticipant *const publish_order[] = {
        &loader_state, &seq_player, &vat_player, &camera,   &light,
        &physics,      &procedural, &behavior,   &metadata};
    SceneLoadParticipant *const activate_order[] = {&behavior};
    SceneLoadParticipant *const commit_order[] = {
        &metadata, &loader_state, &effects, &behavior};
    SceneLoadParticipant *const retire_order[] = {
        &loader_state, &renderer, &procedural, &event_outbox};
    SceneLoadParticipant *const flush_order[] = {
        &rng, &pending_scene, &event_outbox, &effects};
    SceneLoadParticipant *const finish_order[] = {&ecs};
    SceneLoadParticipant *const rollback_order[] = {
        &behavior,    &physics,    &light,       &camera, &vat_player,
        &seq_player,  &loader_state, &renderer,  &procedural,
        &ecs_retire,  &ecs,        &event_outbox, &rng,   &pending_scene};

    const auto fail = [&](SceneLoadPhase phase,
                          std::exception_ptr error) -> SceneLoadResult {
        return PreCommitFailure{phase, exceptionMessage(error), error,
                                releaseErrorStatus(error)};
    };
    const auto rollback = [&]() noexcept {
        for (auto *participant : rollback_order) {
            gate.run(*participant, SceneLoadVerb::rollback);
        }
    };

    try {
        for (auto *participant : prepare_order) {
            gate.run(*participant, SceneLoadVerb::prepare);
        }
    } catch (...) {
        return fail(SceneLoadPhase::prepare, std::current_exception());
    }

    // Every game-DLL callback, including a component JSON loader invoked by
    // early publication, must observe the same transaction-local effects.
    PendingSceneTransaction pending_transaction{
        .loader = this,
        .projection = &transaction.metadata.projection(),
        .candidate = &transaction.pending_candidate,
    };
    internal::EventOutboxScope event_scope{*transaction.event_outbox};
    internal::EffectTransactionScope effect_scope{
        *transaction.effect_transaction};
    RngSequenceScope rng_scope{transaction.rng_candidate};
    PendingSceneTransactionScope pending_scope{pending_transaction};

    try {
        for (auto *participant : early_publish_order) {
            gate.run(*participant, SceneLoadVerb::early_publish);
        }
    } catch (...) {
        rollback();
        return fail(SceneLoadPhase::early_publish,
                    std::current_exception());
    }

    // All remaining candidate publications are swaps over prepared storage.
    // Every one of them is statically asserted noexcept in the participant
    // that performs it, and SceneLoadParticipant::publish is noexcept, so
    // this loop has no failure path at all.
    for (auto *participant : publish_order) {
        gate.run(*participant, SceneLoadVerb::publish);
    }

    try {
        for (auto *participant : activate_order) {
            gate.run(*participant, SceneLoadVerb::activate);
        }
    } catch (...) {
        const auto error = std::current_exception();
        rollback();
        return fail(SceneLoadPhase::activate, error);
    }

    // The commit point is exactly one metadata swap. Nothing after it can
    // produce PreCommitFailure.
    for (auto *participant : commit_order) {
        gate.run(*participant, SceneLoadVerb::commit);
    }
    for (auto *participant : retire_order) {
        gate.run(*participant, SceneLoadVerb::retire);
    }
    phase_trace.record(SceneLoadPhase::flush);
    for (auto *participant : flush_order) {
        gate.run(*participant, SceneLoadVerb::flush);
    }

    // The batch is an executor participant in its own right.  Recording here
    // keeps an empty scene observable while individual token completion stays
    // an implementation detail of the batch.
    phase_trace.record(SceneLoadPhase::finish);
    for (auto *participant : finish_order) {
        gate.run(*participant, SceneLoadVerb::finish);
    }

    // Public strings and JSON are deliberately materialized only after the
    // noexcept commit tail has completed.
    auto effect_errors = transaction.effect_transaction->effectErrors();
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
    // The values a previous scene drew say nothing about this one, and nothing
    // republishes them until a frame is actually drawn.
    graph_values = {};
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
