#include "animationsystem.hpp"

#include "../../animation/animationjobs.hpp"
#include "../../animation/animationprobe.hpp"
#include "../../appflow/enginetime.hpp"
#include "../../asset/model.hpp"
#include "../../loader/pathresolver.hpp"
#include "../../model/skeletalanimation.hpp"
#include "../../renderer/polygoninstancecontainer.hpp"

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <variant>
#include <vector>

namespace Pelican {

struct AnimationSystemState {
    Animation::AnimationAssetRegistry assets;
    std::atomic_uint64_t next_frame_revision{1};
};

AnimationSystem::AnimationSystem() : state{std::make_unique<AnimationSystemState>()} {}
AnimationSystem::~AnimationSystem() = default;

void AnimationSystem::prepareEcsWorkerDependencies(bool has_matching_chunks) {
    if (!has_matching_chunks) return;
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
    const auto frame_revision = state->next_frame_revision.fetch_add(1, std::memory_order_relaxed);
    thread_local Animation::ProbeRuntime pose_runtime;
    const auto arena = pose_runtime.beginFrame(frame_revision);
    if (!Animation::isValid(arena)) throw std::runtime_error("animation pose arena is unavailable on this thread");
    for (size_t i = 0; i < count; ++i) {
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
        status = instances->publishAnimationFrame(*model_views[i].model_instance_id, publish);
        if (status != Animation::Status::ok) throw std::runtime_error("failed to publish animation frame");
    }
}

} // namespace Pelican
