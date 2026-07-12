#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Pelican {

inline constexpr std::uint32_t maxSkinJoints = 128;

enum class AnimationInterpolation : std::uint8_t { linear, step };
enum class AnimationPath : std::uint8_t { translation, rotation, scale };

struct SkeletonNodeRestPose {
    int parent = -1;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    std::string name;
};

struct SkeletalSkinBindingData {
    std::string name;
    std::uint32_t palette_offset = 0;
    std::uint32_t joint_count = 0;
};

struct SkeletalAnimationChannel {
    int node = -1;
    AnimationPath path = AnimationPath::translation;
    AnimationInterpolation interpolation = AnimationInterpolation::linear;
    std::vector<float> times;
    // Translation/scale use xyz. Rotation stores the glTF xyzw quaternion.
    std::vector<glm::vec4> values;
};

struct SkeletalAnimationClip {
    std::string name;
    float start = 0.0f;
    float end = 0.0f;
    std::vector<SkeletalAnimationChannel> channels;
};

struct SkeletalModelData {
    std::string source_path;
    std::vector<SkeletonNodeRestPose> nodes;
    std::vector<int> joint_nodes;
    std::vector<glm::mat4> inverse_bind_matrices;
    // Keeps the original glTF skin boundaries while the two arrays above retain
    // the WP38 combined-palette layout used by the renderer.
    std::vector<SkeletalSkinBindingData> skin_bindings;
    std::vector<SkeletalAnimationClip> clips;
};

// Evaluates glTF-local hierarchy data without creating ECS entities for joints.
// The returned matrices are indexed exactly like JOINTS_0.
std::vector<glm::mat4> evaluateSkinPalette(const SkeletalModelData &model,
                                           const SkeletalAnimationClip *clip,
                                           double engine_time, double speed,
                                           bool loop, double start_time);

const SkeletalAnimationClip &findAnimationClip(const SkeletalModelData &model,
                                               const std::string &name);

} // namespace Pelican
