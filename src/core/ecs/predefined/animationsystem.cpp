#include "animationsystem.hpp"

#include "../../animation/animationjobs.hpp"
#include "../../animation/animationprobe.hpp"
#include "../../animation/animationservice.hpp"
#include "../../userpublic/animation/animgraph.hpp"
#include "../../registrationownerhost.hpp"
#include "../core.hpp"
#include "../../loader/scene.hpp"
#include "../../appflow/enginetime.hpp"
#include "../../asset/model.hpp"
#include "../../loader/pathresolver.hpp"
#include "../../model/skeletalanimation.hpp"
#include "../../renderer/polygoninstancecontainer.hpp"
#if PELICAN_WITH_ANIMATION
#include "../../animation/posegraphruntime.hpp"
#endif
#include "../../renderingpass/framegraphruntime.hpp"
#include "../../renderingpass/graphinputs.hpp"
#include "../../renderingpass/graphmodelsource.hpp"
#include "../../renderingpass/renderingpasscontainer.hpp"

#include <filesystem>
#include <stdexcept>
#include <variant>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {

struct AnimationSystemState {
    struct GraphBinding {
        std::string reference;
        std::optional<List<AnimationParameterOverride>> parameters;
        std::string object_name;
        bool owns_registration{};
        ModelInstanceId instance;
        const SkeletalModelData *model{};
        std::uint64_t epoch{};
        std::uint64_t time_set_revision{};
        std::filesystem::file_time_type modified;
        std::unique_ptr<AnimationGraph::EvaluatorV1> evaluator;
        ~GraphBinding() {
            evaluator.reset();
            if (owns_registration)
                Animation::animationServiceRuntime().unregisterObject(object_name);
        }
    };
    Animation::AnimationAssetRegistry assets;
    std::vector<EntityId> candidates;
    std::unordered_map<EntityId, std::unique_ptr<GraphBinding>> graphs;

    // The pose feed evaluates the same document with its own state, so its
    // clocks advance once per frame without touching the rendering evaluation.
    std::map<std::weak_ptr<const ValueGraph::Graph>, ValueGraph::EvaluationState,
             std::owner_less<std::weak_ptr<const ValueGraph::Graph>>> pose_scalars;

    ValueGraph::EvaluationState &poseScalarState(const std::shared_ptr<const ValueGraph::Graph> &graph) {
        std::erase_if(pose_scalars, [](const auto &entry) { return entry.first.expired(); });
        const auto found = pose_scalars.find(graph);
        if (found != pose_scalars.end()) return found->second;
        return pose_scalars.emplace(graph, graph->createState()).first->second;
    }

    AnimationGraph::EvaluatorV1 *currentEvaluator(EntityId entity) const {
        const auto found = graphs.find(entity);
        if (found == graphs.end()) return nullptr;
        const auto *animation = GET_MODULE(ECSCore).getTemplatePublicModule()
            .tryComponent<AnimationComponent>(entity);
        if (!animation || animation->graph != found->second->reference) return nullptr;
        return found->second->evaluator.get();
    }
};

AnimationSystem::AnimationSystem() : state{std::make_unique<AnimationSystemState>()} {}
AnimationSystem::~AnimationSystem() = default;

void AnimationSystem::prepareEcsWorkerDependencies(DependencyQuery chunks) {
    state->candidates.clear();
    for (const auto &chunk : chunks) {
        const auto entities = std::get<EntityId *>(chunk.components);
        state->candidates.insert(state->candidates.end(), entities, entities + chunk.count);
    }
    if (chunks.empty()) return;
    engine_time = &GET_MODULE(EngineTime);
    instances = &GET_MODULE(PolygonInstanceContainer);
    models = &GET_MODULE(ModelAssetContainer);
    path_resolver = &GET_MODULE(PathResolver);
}

void AnimationSystem::process(QueryComponents components, size_t count) {
    if (count == 0) return;
    if (engine_time == nullptr || instances == nullptr || models == nullptr ||
        path_resolver == nullptr) {
        throw std::logic_error("AnimationSystem dependencies were not prepared on the ECS owner thread");
    }
    auto animations = std::get<AnimationComponent *>(components);
    auto model_views = std::get<SimpleModelViewComponent *>(components);
    const auto time = engine_time->now();
    const auto frame_revision = engine_time->frameIndex();
    thread_local Animation::ProbeRuntime pose_runtime;
    const auto arena = pose_runtime.beginFrame(frame_revision);
    if (!Animation::isValid(arena)) throw std::runtime_error("animation pose arena is unavailable on this thread");
    for (size_t i = 0; i < count; ++i) {
        if (!animations[i].graph.empty()) continue;
        if (!model_views[i].model_instance_id) continue;
        auto &model = models->getModelTemplateByName(model_views[i].model_name);
        if (!model.skeletal) {
            throw std::runtime_error("animation component clip '" + animations[i].clip +
                                     "' is attached to a model without a glTF skin");
        }
        const auto resolved = path_resolver->resolveExistingFileReference(animations[i].clip);
        const auto *reference = std::get_if<ResolvedPathFragment>(&resolved);
        if (reference == nullptr || reference->fragment.kind != "animation") {
            throw std::runtime_error("animation component clip must be a #animation fragment: " +
                                     animations[i].clip);
        }
        std::error_code source_error, clip_error;
        const auto model_source = std::filesystem::weakly_canonical(model.skeletal->source_path, source_error);
        const auto clip_source = std::filesystem::weakly_canonical(reference->path, clip_error);
        if (source_error || clip_error || model_source != clip_source) {
            throw std::runtime_error("animation component clip source does not match the skinned model: " +
                                     animations[i].clip);
        }
        const auto &clip = findAnimationClip(*model.skeletal, reference->fragment.path);
        const auto &asset = state->assets.getOrCreate(*model.skeletal);
        const auto &clip_resource = Animation::findClip(asset, clip);

        auto local_pose = Animation::PoseViewV1{};
        local_pose.struct_size = sizeof(local_pose);
        local_pose.version = Animation::descriptorVersionV1;
        auto status = pose_runtime.acquirePose(arena, asset.rig.layout,
                                               static_cast<std::uint32_t>(asset.rig.rest_pose.size()), local_pose);
        if (status != Animation::Status::ok) throw std::runtime_error("failed to acquire local animation pose");
        status = Animation::samplePoseAt(asset, &clip_resource, time, animations[i].speed,
                                         animations[i].loop != 0, animations[i].start_time, local_pose);
        if (status != Animation::Status::ok) throw std::runtime_error("failed to sample animation pose");

        auto model_pose = Animation::PoseViewV1{};
        model_pose.struct_size = sizeof(model_pose);
        model_pose.version = Animation::descriptorVersionV1;
        status = pose_runtime.acquirePose(arena, asset.rig.layout,
                                          static_cast<std::uint32_t>(asset.rig.rest_pose.size()), model_pose);
        if (status != Animation::Status::ok) throw std::runtime_error("failed to acquire model animation pose");
        std::vector<Animation::Matrix4fV1> model_matrices(asset.rig.rest_pose.size());
        status = Animation::localToModel(asset.rig, local_pose, model_matrices, &model_pose);
        if (status != Animation::Status::ok) throw std::runtime_error("failed to convert animation pose to model space");

        std::vector<Animation::Matrix4fV1> palette(model.skeletal->joint_nodes.size());
        status = Animation::buildSkinPalette(asset, model_matrices, palette);
        if (status != Animation::Status::ok) throw std::runtime_error("failed to build animation skin palette");

        Animation::PublishAnimationFrameDescV1 publish{};
        publish.struct_size = sizeof(publish);
        publish.version = Animation::descriptorVersionV1;
        publish.instance = instances->animationInstance(*model_views[i].model_instance_id);
        publish.local_pose = local_pose.pose;
        publish.model_pose = model_pose.pose;
        publish.palette = palette.data();
        publish.palette_count = static_cast<std::uint32_t>(palette.size());
        publish.frame_revision = frame_revision;
        publish.root_delta.rotation.w = 1.0f;
        status = instances->publishAnimationFrame(*model_views[i].model_instance_id, publish,
                                                   model_matrices, asset.rig.original_to_layout);
        if (status != Animation::Status::ok) throw std::runtime_error("failed to publish animation frame");
    }
}

void AnimationSystem::updateGraphs() {
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &scene = GET_MODULE(SceneLoader);
    auto &time = GET_MODULE(EngineTime);
    auto &service = Animation::animationServiceRuntime();
    internal::ScopedRegistrationOwner owner{internal::engineRegistrationOwner};
    std::unordered_set<EntityId> live;
    for (const auto entity : state->candidates) {
        const auto *animation = ecs.tryComponent<AnimationComponent>(entity);
        const auto *view = ecs.tryComponent<SimpleModelViewComponent>(entity);
        if (!animation || animation->graph.empty() || !view || !view->model_instance_id) continue;
        live.insert(entity);
        auto &model = GET_MODULE(ModelAssetContainer).getModelTemplateByName(view->model_name);
        if (!model.skeletal) throw std::runtime_error("animation graph requires a skinned model");
        auto &resolver = GET_MODULE(PathResolver);
        const auto path = resolver.resolveExistingFile(animation->graph);
        const auto modified = std::filesystem::last_write_time(path);
        auto found = state->graphs.find(entity);
        const auto epoch = scene.runtimeSceneEpoch();
        const auto named = scene.objectName(entity);
        const auto object_name = named.value_or("__animation_component:" +
            std::to_string(epoch) + ":" + toString(entity));
        bool replace = found == state->graphs.end();
        if (!replace) {
            const auto &old = *found->second;
            replace = old.reference != animation->graph || old.modified != modified ||
                old.parameters != animation->parameters ||
                old.epoch != epoch || old.instance != *view->model_instance_id ||
                old.object_name != object_name;
            if (!replace && old.model != model.skeletal.get()) {
                if (old.evaluator->rebind() != Animation::Status::ok) replace = true;
                else found->second->model = model.skeletal.get();
            }
        }
        if (replace) {
            // Parse before releasing the previous writer. A malformed edited
            // asset must not silently discard the existing graph state.
            auto document = AnimationGraph::parseDocumentV1(resolver.loadText(animation->graph));
            state->graphs.erase(entity);
            auto binding = std::make_unique<AnimationSystemState::GraphBinding>();
            binding->reference = animation->graph;
            binding->parameters = animation->parameters;
            binding->object_name = object_name;
            binding->instance = *view->model_instance_id;
            binding->model = model.skeletal.get();
            binding->epoch = epoch;
            binding->modified = modified;
            binding->time_set_revision = time.timeSetRevision();
            if (!named) {
                service.registerObject(object_name, *model.skeletal, *view->model_instance_id);
                binding->owns_registration = true;
            }
            binding->evaluator = std::make_unique<AnimationGraph::EvaluatorV1>(std::move(document), object_name);
            if (binding->parameters)
                for (const auto &row : binding->parameters->rows)
                    if (binding->evaluator->setParameter(row.value.parameter, row.value.value) != Animation::Status::ok)
                        throw std::runtime_error("animation graph parameter override failed: " + row.value.parameter);
            if (binding->evaluator->bind() != Animation::Status::ok)
                throw std::runtime_error("animation graph bind failed: " + binding->evaluator->lastError());
            found = state->graphs.emplace(entity, std::move(binding)).first;
        }
        auto &binding = *found->second;
        if (binding.time_set_revision != time.timeSetRevision()) {
            const auto status = binding.evaluator->notify(Animation::AnimationNotificationKind::set_time, time.now());
            if (status != Animation::Status::ok) throw std::runtime_error("animation graph clock reset failed");
            binding.time_set_revision = time.timeSetRevision();
        }
        if (binding.evaluator->prepareTick(time.now(), time.dt(), time.frameIndex()) != Animation::Status::ok)
            throw std::runtime_error("animation graph tick preparation failed");
    }
    std::erase_if(state->graphs, [&](const auto &entry) { return !live.contains(entry.first); });
    updatePoseGraphs();
}

void AnimationSystem::updatePoseGraphs() {
#if PELICAN_WITH_ANIMATION
    auto &runtime = Animation::poseGraphRuntime();
    runtime.retireUnreferenced();
    const auto *passes = FastModuleContainer::tryGet<RenderingPassContainer>();
    const auto generation = passes ? passes->snapshot() : nullptr;
    if (!generation) return;
    auto &scene = GET_MODULE(SceneLoader);
    auto &time = GET_MODULE(EngineTime);
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    internal::ScopedRegistrationOwner owner{internal::engineRegistrationOwner};
    for (const auto &[id, program] : generation->programs) {
        (void)id;
        const auto &pose_graph = program.frame_graph.pose_graph;
        const auto &value_graph = program.frame_graph.value_graph;
        if (!pose_graph || pose_graph->empty() || !value_graph) continue;
        Animation::PoseGraphFrame frame;
        frame.time = time.now();
        frame.delta_seconds = time.dt();
        frame.frame_revision = time.frameIndex();
        frame.time_set_revision = time.timeSetRevision();
        frame.object_name = [&](const GraphInstanceOwner &instance) -> std::optional<std::string> {
            const auto object = scene.graphOwnerObject(instance);
            if (!object) return std::nullopt;
            const auto named = scene.objectName(*object);
            // A pose sink writes a named scene sink. An unnamed object has no
            // stable identity to hand a writer to; say so instead of inventing one.
            if (!named)
                throw std::runtime_error("pose sink requires a named scene object");
            return named;
        };
        frame.verify_rig = [&](const GraphInstanceOwner &instance, const std::string &rig,
                               const std::string &node) {
            // resolveGraphModelSource already names the mismatch between a
            // graph's declared asset and the owner's live model.
            (void)resolveGraphModelSource(instance, rig, node);
        };
        frame.component_writer = [&](const GraphInstanceOwner &instance) {
            // Either component source -- clip or graph -- already writes this
            // object's pose. AG-1b converts the component path to a graph
            // instance; until then the two cannot share one object.
            const auto object = scene.graphOwnerObject(instance);
            if (!object || !ecs.isAlive(*object)) return false;
            const auto *animation = ecs.tryComponent<AnimationComponent>(*object);
            return animation != nullptr && (!animation->clip.empty() || !animation->graph.empty());
        };
        // The scalars this frame, from the values partition of the same
        // document. A separate evaluation state advances once per frame with
        // the same inputs, so pose and rendering read the same numbers without
        // either one advancing the other's clocks.
        auto inactive = scene.inactiveGraphScopes(*value_graph);
        inactive.insert(program.frame_graph.inactive_graph_scopes.begin(),
                        program.frame_graph.inactive_graph_scopes.end());
        const auto &scalar_outputs = pose_graph->scalarOutputs();
        if (!scalar_outputs.empty()) {
            auto &evaluation = state->poseScalarState(value_graph);
            const auto inputs = collectGraphInputs(*value_graph, scene, inactive, time.now(), time.dt());
            const auto outputs = value_graph->evaluateLowered(inputs,
                scene.graphParameterValues(value_graph), inactive, &evaluation);
            for (const auto &name : scalar_outputs) {
                const auto found = outputs.find(name);
                if (found == outputs.end() || found->second.empty()) continue;
                frame.scalars.emplace(name, found->second.front());
            }
        }
        runtime.synchronize(pose_graph, frame);
    }
#endif
}

Animation::Status AnimationSystem::setGraphParameter(EntityId entity, std::string_view name, double value) {
    const auto evaluator = state->currentEvaluator(entity);
    return evaluator ? evaluator->setParameter(name, value) : Animation::Status::not_found;
}

Animation::Status AnimationSystem::forceGraphState(EntityId entity, std::string_view name) {
    const auto evaluator = state->currentEvaluator(entity);
    return evaluator ? evaluator->forceState(name) : Animation::Status::not_found;
}

} // namespace Pelican
