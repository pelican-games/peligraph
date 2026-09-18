#include "animationposearena.hpp"

#include "animationserviceabi.hpp"

#include <utility>

namespace Pelican::Animation::Internal {

PoseArena::PoseRecord *PoseArena::findPose(PoseHandle pose) {
    if (!isValid(pose)) return nullptr;
    const auto found = poses_.find(pose.identity);
    if (found == poses_.end() || !sameHandle(found->second.view.pose, pose))
        return nullptr;
    return &found->second;
}

Status PoseArena::resolvePose(PoseHandle pose, PoseRecord *&out) {
    out = findPose(pose);
    if (out) return Status::ok;
    if (const auto stale = stale_poses_.find(pose.identity);
        stale != stale_poses_.end() && stale->second == pose.generation)
        return Status::stale_generation;
    return ProbeRuntime::validatePoseHandle(pose);
}

Status PoseArena::beginFrame(AnimationOwnerHandle owner,
                             std::uint64_t frame_revision,
                             std::uint64_t &next_identity,
                             PoseArenaHandle &out_arena) {
    ArenaRecord *arena = nullptr;
    for (auto &[_, candidate] : arenas_) {
        if (sameHandle(candidate.owner, owner) &&
            candidate.thread == std::this_thread::get_id()) {
            arena = &candidate;
            break;
        }
    }
    if (!arena) {
        ArenaRecord created;
        created.handle = {next_identity++, 1, 0};
        created.owner = owner;
        created.thread = std::this_thread::get_id();
        created.runtime = std::make_unique<ProbeRuntime>();
        arena = &arenas_.emplace(created.handle.identity, std::move(created))
                     .first->second;
    } else {
        for (const auto pose_identity : arena->poses)
            poses_.erase(pose_identity);
        arena->poses.clear();
        if (++arena->handle.generation == 0) ++arena->handle.generation;
    }
    arena->internal_arena = arena->runtime->beginFrame(frame_revision);
    if (!isValid(arena->internal_arena)) return Status::wrong_thread;
    out_arena = arena->handle;
    return Status::ok;
}

Status PoseArena::acquirePose(PoseArenaHandle arena_handle,
                              PoseLayoutHandle layout,
                              std::uint32_t joint_count, PoseViewV1 &view) {
    const auto found = arenas_.find(arena_handle.identity);
    if (found == arenas_.end() || !isValid(arena_handle))
        return Status::invalid_handle;
    auto &arena = found->second;
    if (arena_handle.generation != arena.handle.generation)
        return Status::stale_generation;
    if (arena.thread != std::this_thread::get_id()) return Status::wrong_thread;
    auto produced = view;
    const auto status = arena.runtime->acquirePose(arena.internal_arena, layout,
                                                   joint_count, produced);
    if (status != Status::ok) return status;
    view = produced;
    arena.poses.push_back(produced.pose.identity);
    poses_.emplace(produced.pose.identity,
                   PoseRecord{produced, arena.handle.identity});
    return Status::ok;
}

Status PoseArena::blendNormalJob(const AnimationRig &rig,
                                 const BlendNormalDescV1 &desc) {
    PoseRecord *output{};
    if (const auto status = resolvePose(desc.output_pose, output);
        status != Status::ok)
        return status;
    std::vector<NormalBlendInput> inputs;
    inputs.reserve(desc.layer_count);
    for (std::uint32_t i = 0; i < desc.layer_count; ++i) {
        const auto &layer = desc.layers[i];
        if (const auto status = validateDescriptor(layer);
            status != Status::ok)
            return status;
        if (layer.mode != BlendMode::normal ||
            layer.additive_space != AdditiveSpace::local)
            return Status::invalid_argument;
        PoseRecord *pose{};
        if (const auto status = resolvePose(layer.pose, pose);
            status != Status::ok)
            return status;
        if (layer.joint_weight_count != 0 && layer.joint_weights == nullptr)
            return Status::invalid_argument;
        inputs.push_back({&pose->view, layer.weight,
                          {layer.joint_weights, layer.joint_weight_count}});
    }
    return blendNormal(rig, inputs, output->view);
}

Status PoseArena::localToModelJob(const AnimationRig &rig,
                                  LocalToModelDescV1 &desc) {
    PoseRecord *local{};
    if (const auto status = resolvePose(desc.local_pose, local);
        status != Status::ok)
        return status;
    const auto required = static_cast<std::uint32_t>(rig.rest_pose.size());
    desc.model_matrix_count = required;
    if (desc.model_matrix_capacity < required || !desc.model_matrices)
        return Status::buffer_too_small;
    PoseViewV1 *model_view = nullptr;
    PoseRecord *model{};
    if (isValid(desc.model_pose)) {
        if (const auto status = resolvePose(desc.model_pose, model);
            status != Status::ok)
            return status;
        model_view = &model->view;
    }
    const auto status = localToModel(rig, local->view,
        {desc.model_matrices, desc.model_matrix_capacity}, model_view);
    if (status == Status::ok && model)
        model->model_matrices.assign(desc.model_matrices,
                                     desc.model_matrices + required);
    return status;
}

void PoseArena::retireArenasForOwner(std::uint64_t owner_identity) {
    std::vector<std::uint64_t> dead_arenas;
    for (const auto &[arena_identity, arena] : arenas_)
        if (arena.owner.identity == owner_identity)
            dead_arenas.push_back(arena_identity);
    for (const auto arena_identity : dead_arenas) {
        for (const auto pose_identity : arenas_[arena_identity].poses)
            poses_.erase(pose_identity);
        arenas_.erase(arena_identity);
    }
}

void PoseArena::stalePosesForLayout(std::uint64_t layout_identity) {
    for (auto iterator = poses_.begin(); iterator != poses_.end();) {
        if (layout_identity == 0 ||
            iterator->second.view.layout.identity != layout_identity) {
            ++iterator;
            continue;
        }
        stale_poses_[iterator->first] = iterator->second.view.pose.generation;
        iterator = poses_.erase(iterator);
    }
}

void PoseArena::clearForReset() {
    for (const auto &[identity, pose] : poses_)
        stale_poses_[identity] = pose.view.pose.generation;
    poses_.clear();
    arenas_.clear();
}

} // namespace Pelican::Animation::Internal
