#pragma once

#include "../model/skeletalanimation.hpp"
#include "../model/vrmaanimation.hpp"
#include "../model/vrmsemantic.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

inline constexpr std::uint32_t vrmaRetargetProfileVersion = 1;

// R0 deliberately has no extraction mode. The enum is the versioned slot in
// which a later profile may add an explicit root-motion policy without
// reinterpreting v1 data.
enum class VrmaRootMotionPolicy : std::uint8_t { preserve_hips_translation };

struct VrmaRetargetProfileOptions {
    std::uint32_t version = vrmaRetargetProfileVersion;
    VrmaRootMotionPolicy root_motion_policy =
        VrmaRootMotionPolicy::preserve_hips_translation;
};

enum class VrmaRetargetWarningKind : std::uint8_t {
    missing_optional_target_bone,
    missing_target_expression,
};

struct VrmaRetargetWarning {
    VrmaRetargetWarningKind kind =
        VrmaRetargetWarningKind::missing_optional_target_bone;
    std::string subject;
    std::string message;
};

struct VrmaRetargetProvenance {
    VrmaClipMetadata source_clip;
    std::uint32_t profile_version = 0;
    std::string source_rig_sha256;
    std::string target_rig_sha256;
};

struct VrmaLocalTransform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct VrmaRetargetBoneMapping {
    std::string human_bone;
    int source_node = -1;
    // -1 means that an optional source bone is absent on the target. R0 does
    // not reconnect its descendants.
    int target_node = -1;
    VrmaLocalTransform source_rest;
    VrmaLocalTransform target_rest;
};

// This is the resolved, versioned application profile. Keeping provenance and
// every source humanoid mapping here makes the artifact independently
// auditable even when a bone has no channel in this particular clip.
struct VrmaRetargetProfile {
    std::uint32_t version = vrmaRetargetProfileVersion;
    VrmaRootMotionPolicy root_motion_policy =
        VrmaRootMotionPolicy::preserve_hips_translation;
    float hips_translation_scale = 1.0f;
    std::vector<VrmaRetargetBoneMapping> bone_mappings;
    VrmaRetargetProvenance provenance;
};

struct VrmaRetargetedBodyChannel {
    std::string human_bone;
    int source_node = -1;
    int target_node = -1;
    VrmaBodyPath path = VrmaBodyPath::rotation;
    VrmaVectorTrack track;
    VrmaLocalTransform source_rest;
    VrmaLocalTransform target_rest;
    float translation_scale = 1.0f;
};

struct VrmaExpressionSample {
    std::string expression;
    bool preset = false;
    float weight = 0.0f;
};

struct VrmaGazeSample {
    std::optional<std::array<float, 3>> offset_from_head_bone;
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Indexed by the target rig node layout. Nodes without a body channel retain
// their target rest transform. Expression and gaze stay separate from joint
// pose data so I0 can route them to typed sinks.
struct VrmaRetargetSample {
    float time = 0.0f;
    std::vector<VrmaLocalTransform> local_transforms;
    std::vector<VrmaExpressionSample> expressions;
    std::optional<VrmaGazeSample> gaze;
};

struct VrmaRetargetedClip {
    VrmaRetargetProfile profile;
    float start = 0.0f;
    float end = 0.0f;
    std::vector<VrmaLocalTransform> target_rest_pose;
    std::vector<VrmaRetargetedBodyChannel> body_channels;
    std::vector<VrmaExpressionChannel> expression_channels;
    std::optional<VrmaGazeChannel> gaze_channel;
    std::vector<VrmaRetargetWarning> warnings;

    VrmaRetargetSample sample(float time) const;
};

// Builds an immutable, target-rig-local intermediate clip. This is not an
// AnimationSource and does not publish to an instance, graph, renderer, or ABI.
std::shared_ptr<const VrmaRetargetedClip>
retargetVrmaClip(const VrmaClip &source, const SkeletalModelData &target_rig,
                 const VrmSemanticData &target_semantic,
                 VrmaRetargetProfileOptions options = {});

} // namespace Pelican
