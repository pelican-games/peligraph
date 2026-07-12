#pragma once

#include "../model/skeletalanimation.hpp"
#include "../userpublic/animation/abi_v1.hpp"

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Pelican::Animation {

struct AnimationRig {
    RigHandle handle{};
    PoseLayoutHandle layout{};
    std::vector<std::int32_t> parents;
    std::vector<TransformV1> rest_pose;
    std::vector<std::string> node_names;
    std::vector<std::uint32_t> original_to_layout;
    std::vector<std::uint32_t> layout_to_original;
};

struct AnimationSkinBinding {
    SkinBindingHandle handle{};
    RigHandle source_rig{};
    PoseLayoutHandle layout{};
    std::uint32_t palette_offset{};
    std::vector<std::uint32_t> joint_layout_nodes;
    std::vector<glm::mat4> inverse_bind_matrices;
};

struct AnimationClipResource {
    ClipHandle handle{};
    RigHandle source_rig{};
    const SkeletalAnimationClip *source{};
};

struct AnimationAsset {
    const SkeletalModelData *source{};
    AnimationRig rig;
    std::vector<AnimationSkinBinding> skin_bindings;
    std::vector<AnimationClipResource> clips;
};

class AnimationAssetRegistry {
  public:
    const AnimationAsset &getOrCreate(const SkeletalModelData &model);
    void clear();

  private:
    std::uint64_t next_identity_{1};
    std::unordered_map<const SkeletalModelData *, AnimationAsset> assets_;
    std::mutex mutex_;
};

struct NormalBlendInput {
    const PoseViewV1 *pose{};
    float weight{};
    std::span<const float> joint_weights{};
};

const AnimationClipResource &findClip(const AnimationAsset &asset, const SkeletalAnimationClip &clip);

// Point operation. The supplied PoseView owns all output storage and is not retained.
Status samplePoseAt(const AnimationAsset &asset, const AnimationClipResource *clip,
                    double engine_time, double speed, bool loop, double start_time,
                    PoseViewV1 &out_pose);

// Frozen normal N-way blend. Translation/scale use normalized weighted sums and
// rotation uses the ABI v1 quaternion reference algorithm.
Status blendNormal(const AnimationRig &rig, std::span<const NormalBlendInput> inputs,
                   PoseViewV1 &out_pose);

Status localToModel(const AnimationRig &rig, const PoseViewV1 &local_pose,
                    std::span<Matrix4fV1> model_matrices, PoseViewV1 *model_pose = nullptr);

Status buildSkinPalette(const AnimationAsset &asset, std::span<const Matrix4fV1> model_matrices,
                        std::span<Matrix4fV1> palette);

} // namespace Pelican::Animation
