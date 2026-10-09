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
#include "../../model/animationcomponentvalidation.hpp"
#include "../../watch/problemlist.hpp"
#include "../../renderer/polygoninstancecontainer.hpp"
#if PELICAN_WITH_ANIMATION
#include "../../animation/posegraphruntime.hpp"
#endif
#include "../../geomhelper/geomhelper.hpp"
#include "../../userpublic/components/localtransform.hpp"
#include "../../userpublic/gameobjects.hpp"
#include "../../renderingpass/framegraphruntime.hpp"
#include "../../renderingpass/graphinputs.hpp"
#include "../../renderingpass/graphmodelsource.hpp"
#include "../../renderingpass/renderingpasscontainer.hpp"
#include "../../watch/reloadgate.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <variant>
#include <utility>
#include <vector>
#include <map>
#include <set>
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
    std::unordered_map<EntityId, const SkeletalAnimationClip *> clips;
    Animation::AnimationAssetRegistry assets;
    std::vector<EntityId> candidates;
    // AG-3a. `<model name>\0<clip reference>` -> the clip's own [start, end].
    // A status read resolves a clip reference once; the bounds are a property
    // of the asset, so a second read of the same clip is arithmetic only.
    struct ClipBounds {
        std::weak_ptr<const SkeletalModelData> identity;
        double begin;
        double end;
    };
    std::map<std::string, ClipBounds> clip_bounds;
    std::unordered_map<EntityId, std::unique_ptr<GraphBinding>> graphs;

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

Animation::AnimationAssetRegistry &AnimationSystem::assetRegistry() { return state->assets; }

void AnimationSystem::prepareEcsWorkerDependencies(DependencyQuery chunks) {
    state->candidates.clear();
    for (const auto &chunk : chunks) {
        const auto entities = std::get<EntityId *>(chunk.components);
        state->candidates.insert(state->candidates.end(), entities, entities + chunk.count);
    }
    state->clips.clear();
    std::vector<nlohmann::json> problems;
    if (!chunks.empty()) {
        engine_time = &GET_MODULE(EngineTime);
        instances = &GET_MODULE(PolygonInstanceContainer);
        models = &GET_MODULE(ModelAssetContainer);
        path_resolver = &GET_MODULE(PathResolver);
        for (const auto &chunk : chunks) {
            const auto entities = std::get<EntityId *>(chunk.components);
            const auto animations = std::get<AnimationComponent *>(chunk.components);
            const auto views = std::get<SimpleModelViewComponent *>(chunk.components);
            for (size_t i = 0; i < chunk.count; ++i) {
                if (!animations[i].graph.empty() || !views[i].model_instance_id) continue;
                try {
                    const auto &model = models->getModelTemplateByName(views[i].model_name);
                    state->clips.emplace(entities[i], &requireAnimationComponentClip(
                        model.skeletal.get(), animations[i].clip, *path_resolver));
                } catch (const std::exception &error) {
                    auto &scene = GET_MODULE(SceneLoader);
                    const auto entity = toString(entities[i]);
                    nlohmann::json problem{
                        {"id", problemId({{"code", "animation.component_invalid@1"},
                                          {"scope", scene.runtimeSceneEpoch()}, {"node", entity}})},
                        {"severity", problemSeverityError}, {"source", "ecs"},
                        {"code", "animation.component_invalid@1"}, {"message", error.what()},
                        {"document_kind", problemKindScene},
                        {"fields", {{"entity", entity},
                                    {"object", views[i].authoring_object_id},
                                    {"name", scene.objectName(entities[i]).value_or(entity)},
                                    {"scene_id", scene.currentScene()},
                                    {"component", "animation"}, {"clip", animations[i].clip}}}};
                    problems.push_back(std::move(problem));
                }
            }
        }
    }
    GET_MODULE(ProblemList).replace("animation_components", std::move(problems));
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
    const auto frame_revision = engine_time->evaluationIndex();
    thread_local Animation::ProbeRuntime pose_runtime;
    const auto arena = pose_runtime.beginFrame(frame_revision);
    if (!Animation::isValid(arena)) throw std::runtime_error("animation pose arena is unavailable on this thread");
    for (size_t i = 0; i < count; ++i) {
        if (!animations[i].graph.empty()) continue;
        if (!model_views[i].model_instance_id) continue;
        const auto found = state->clips.find(std::get<EntityId *>(components)[i]);
        if (found == state->clips.end()) continue;
        auto &model = models->getModelTemplateByName(model_views[i].model_name);
        const auto &clip = *found->second;
        const auto &asset = state->assets.getOrCreate(model.skeletal);
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
        // The clip path writes the container directly, so it says who it is:
        // a pose graph reaching the same object this frame is then a named
        // conflict instead of a second pose landing on top of this one.
        status = instances->publishAnimationFrame(*model_views[i].model_instance_id,
                                                   {AnimationFrameWriterKind::animation_component_clip,
                                                    animationComponentClipWriter},
                                                   publish, model_matrices,
                                                   asset.rig.original_to_layout);
        if (status == Animation::Status::authority_conflict)
            throw std::runtime_error("another writer already published this object's pose this frame: " +
                                     animations[i].clip);
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
        auto found = state->graphs.find(entity);
        // EB-7: a hot reload build looks at the graph file every frame and
        // reads it again when it changed; a distribution build looks once,
        // when it first reads the file, and keeps that write time.
        const bool read_before =
            found != state->graphs.end() && found->second->reference == animation->graph;
        const auto modified =
            watch::looksAgainAtReadFile(PELICAN_WITH_HOT_RELOAD != 0, read_before)
                ? std::filesystem::last_write_time(resolver.resolveExistingFile(animation->graph))
                : found->second->modified;
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
        if (binding.evaluator->prepareTick(time.now(), time.dt(), time.evaluationIndex()) != Animation::Status::ok)
            throw std::runtime_error("animation graph tick preparation failed");
    }
    std::erase_if(state->graphs, [&](const auto &entry) { return !live.contains(entry.first); });
    updatePoseGraphs();
}

void AnimationSystem::updatePoseGraphs() {
#if PELICAN_WITH_ANIMATION
    auto &runtime = Animation::poseGraphRuntime();
    const auto *passes = FastModuleContainer::tryGet<RenderingPassContainer>();
    const auto generation = passes ? passes->snapshot() : nullptr;
    // The pose graphs of the programs that are published right now. Anything
    // else the runtime still holds belongs to a generation that is gone; it is
    // released before this pass attaches, so a retired state machine cannot
    // still be claiming the object its replacement is about to claim.
    std::set<const Animation::PoseGraph *> live;
    if (generation)
        for (const auto &[id, program] : generation->programs) {
            (void)id;
            if (program.frame_graph.pose_graph) live.insert(program.frame_graph.pose_graph.get());
        }
    runtime.retainOnly(live);
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
        frame.frame_revision = time.evaluationIndex();
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
        // AG-2. Where a root-motion displacement lands, and how. The object is
        // resolved by name once, at attach, and kept on the attachment as a
        // token: this lambda captures this function's locals and is gone long
        // before the animation phases run. The applier below captures nothing
        // for the same reason -- it is the one callback the pose graph keeps.
        frame.root_transform_object =
            [&](const GraphInstanceOwner &instance) -> std::optional<std::uint64_t> {
            const auto object = scene.graphOwnerObject(instance);
            if (!object || !ecs.isAlive(*object)) return std::nullopt;
            // Root motion writes a local transform. An object without one has
            // nowhere to put the movement; the graph names that rather than
            // extracting a displacement nobody applies. Every scene object with
            // a transform is given one (GP-1); an object created with only a
            // world transform, such as a transient glTF, still has none.
            if (!ecs.tryComponent<LocalTransformComponent>(*object)) return std::nullopt;
            return (static_cast<std::uint64_t>(object->index) << 32) | object->generation;
        };
        frame.apply_root_translation = [](std::uint64_t token, const double *delta) {
            const GameObjectId object{static_cast<std::uint32_t>(token >> 32),
                                      static_cast<std::uint32_t>(token & 0xffffffffull)};
            auto &objects = GET_MODULE(ECSCore).getTemplatePublicModule();
            if (!objects.isAlive(object)) return false;
            const auto *local = objects.tryComponent<LocalTransformComponent>(object);
            if (!local) return false;
            // The clip's root moves in the individual's own frame, so its
            // orientation turns the displacement before it is added. Scale is
            // deliberately not applied: a scaled individual would otherwise
            // cover a different distance for the same clip.
            const auto offset = to_glm(local->rotation) *
                glm::vec3{static_cast<float>(delta[0]), static_cast<float>(delta[1]),
                          static_cast<float>(delta[2])};
            auto moved = *local;
            moved.pos = vec3{local->pos.x + offset.x, local->pos.y + offset.y,
                             local->pos.z + offset.z};
            // Throws inside a scene effect transaction; that reaches the phase
            // as callback_failed and stops the frame rather than moving an
            // individual the transaction was about to replace.
            return GameObjects::setLocalTransform(object, moved);
        };
        frame.component_writer = [&](const GraphInstanceOwner &instance) {
            // Not "the object carries an animation component" but "the clip
            // path has already published this object's frame this revision".
            // The ECS update runs before the animation phases, so by the time a
            // pose graph is synchronized the writer token is already on the
            // instance; a graph sink on the same object is pose.sink_conflict@1
            // rather than a second pose landing on top of the first.
            const auto object = scene.graphOwnerObject(instance);
            if (!object || !ecs.isAlive(*object)) return false;
            const auto *view = ecs.tryComponent<SimpleModelViewComponent>(*object);
            if (!view || !view->model_instance_id) return false;
            const auto writer = GET_MODULE(PolygonInstanceContainer)
                                    .animationFrameWriter(*view->model_instance_id);
            return writer.kind == AnimationFrameWriterKind::animation_component_clip &&
                   writer.frame_revision == time.evaluationIndex();
        };
        // The scalars this frame, from the values partition of the same
        // document. A separate evaluation state -- owned by the scene beside
        // the rendering one, so both survive a graph generation change or
        // neither does -- advances once per frame with the same inputs, so pose
        // and rendering read the same numbers without either one advancing the
        // other's clocks.
        auto inactive = scene.inactiveGraphScopes(*value_graph);
        inactive.insert(program.frame_graph.inactive_graph_scopes.begin(),
                        program.frame_graph.inactive_graph_scopes.end());
        const auto &scalar_outputs = pose_graph->scalarOutputs();
        if (!scalar_outputs.empty()) {
            auto &evaluation = scene.poseGraphEvaluationState(value_graph);
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

std::vector<AnimationComponentTrace> AnimationSystem::componentTrace() {
    std::vector<AnimationComponentTrace> result;
    if (state->candidates.empty()) return result;
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &scene = GET_MODULE(SceneLoader);
    auto &time = GET_MODULE(EngineTime);
    // The clip path's own time map (sampleTime in animationjobs.cpp): shift by
    // start_time, scale by speed, then wrap or clamp inside the clip's own
    // [start, end]. Spelled here rather than recorded from the worker threads:
    // the numbers are the same ones the frame sampled with, and a status read
    // that never touches the ECS hot path cannot slow it down.
    const auto sampled = [](double begin, double end, double now, double speed, bool loop,
                            double start_time) {
        const auto duration = end - begin;
        auto local = (now - start_time) * speed + begin;
        if (loop && duration > 0.0) {
            local = std::fmod(local - begin, duration);
            if (local < 0.0) local += duration;
            local += begin;
        } else {
            local = std::clamp(local, begin, end);
        }
        return local;
    };
    // Resolving a clip reference walks the path resolver, so the answer is kept
    // per model and clip: a tool polling this every frame must not turn a
    // status read into filesystem work per frame.
    for (const auto entity : state->candidates) {
        const auto *animation = ecs.tryComponent<AnimationComponent>(entity);
        if (animation == nullptr) continue;
        AnimationComponentTrace trace;
        trace.object = entity;
        trace.object_name = scene.objectName(entity).value_or(std::string{});
        trace.frame_revision = time.evaluationIndex();
        if (!animation->graph.empty()) {
            const auto found = state->graphs.find(entity);
            if (found == state->graphs.end() || found->second->reference != animation->graph ||
                !found->second->evaluator)
                continue;
            trace.graph = true;
            trace.reference = animation->graph;
            const auto status = found->second->evaluator->getStatus();
            trace.state = status.current_state;
            trace.cursors = status.cursors;
            trace.transition_active = status.transition_active;
            trace.transition_target = status.transition_target;
            trace.transition_progress = status.transition_progress;
            trace.frame_revision = status.frame_revision;
        } else if (!animation->clip.empty()) {
            if (!state->clips.contains(entity)) continue;
            const auto *view = ecs.tryComponent<SimpleModelViewComponent>(entity);
            if (view == nullptr || !view->model_instance_id) continue;
            if (models == nullptr || path_resolver == nullptr) continue;
            const auto key = view->model_name + '\0' + animation->clip;
            auto bounds = state->clip_bounds.find(key);
            try {
                const auto &model = models->getModelTemplateByName(view->model_name);
                if (!model.skeletal) continue;
                if (bounds == state->clip_bounds.end() || bounds->second.identity.expired() ||
                    bounds->second.identity.lock() != model.skeletal ||
                    bounds->second.identity.owner_before(model.skeletal) ||
                    model.skeletal.owner_before(bounds->second.identity)) {
                    const auto resolved = path_resolver->resolveExistingFileReference(animation->clip);
                    const auto *reference = std::get_if<ResolvedPathFragment>(&resolved);
                    if (reference == nullptr || reference->fragment.kind != "animation") continue;
                    const auto &clip = findAnimationClip(*model.skeletal, reference->fragment.path);
                    bounds = state->clip_bounds.insert_or_assign(key,
                        AnimationSystemState::ClipBounds{model.skeletal, clip.start, clip.end}).first;
                }
            } catch (const std::exception &) {
                // An unresolvable clip is the frame's own error to raise;
                // observing the frame must not raise it a second time.
                continue;
            }
            const auto begin = bounds->second.begin;
            const auto end = bounds->second.end;
            const auto at = sampled(begin, end, time.now(), animation->speed,
                                    animation->loop != 0, animation->start_time);
            const auto span = end - begin;
            trace.reference = animation->clip;
            trace.cursors.push_back(AnimationGraph::CursorStatusV1{
                .clip = animation->clip, .time_seconds = at,
                .normalized_phase = span > 0.0 ? (at - begin) / span : 0.0});
        } else {
            continue;
        }
        result.push_back(std::move(trace));
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        if (left.object_name != right.object_name) return left.object_name < right.object_name;
        if (left.object.index != right.object.index) return left.object.index < right.object.index;
        return left.object.generation < right.object.generation;
    });
    return result;
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
