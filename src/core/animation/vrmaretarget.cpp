#include "vrmaretarget.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <picosha2.h>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Pelican {
namespace {

constexpr float epsilon = 1.0e-6f;
constexpr std::array<std::string_view, 15> requiredHumanBones{
    "hips",          "spine",         "head",          "leftUpperLeg",
    "leftLowerLeg",  "leftFoot",      "rightUpperLeg", "rightLowerLeg",
    "rightFoot",     "leftUpperArm",  "leftLowerArm",  "leftHand",
    "rightUpperArm", "rightLowerArm", "rightHand",
};

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("VRMA retarget: " + message);
}

bool finite(glm::vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(glm::quat value) noexcept {
    return std::isfinite(value.w) && std::isfinite(value.x) &&
           std::isfinite(value.y) && std::isfinite(value.z);
}

glm::quat normalized(glm::quat value, std::string_view context) {
    if (!finite(value) || glm::dot(value, value) <= epsilon * epsilon)
        fail(std::string{context} + " has an invalid zero or non-finite quaternion");
    return glm::normalize(value);
}

bool requiredBone(std::string_view name) noexcept {
    return std::find(requiredHumanBones.begin(), requiredHumanBones.end(), name) !=
           requiredHumanBones.end();
}

void appendU32(std::string &output, std::uint32_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
        output.push_back(digits[(value >> shift) & 0x0fu]);
}

void appendI32(std::string &output, int value) {
    appendU32(output, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
}

void appendFloat(std::string &output, float value) {
    appendU32(output, std::bit_cast<std::uint32_t>(value));
}

void appendString(std::string &output, std::string_view value) {
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

std::string hashSourceRig(const VrmaSourceRig &rig) {
    std::string canonical{"pelican.vrma.source-rig.v1"};
    appendU32(canonical, static_cast<std::uint32_t>(rig.nodes.size()));
    for (const auto &node : rig.nodes) {
        appendString(canonical, node.name);
        appendI32(canonical, node.parent);
        for (float value : node.translation) appendFloat(canonical, value);
        for (float value : node.rotation) appendFloat(canonical, value);
        for (float value : node.scale) appendFloat(canonical, value);
        appendU32(canonical, node.matrix ? 1u : 0u);
        if (node.matrix)
            for (float value : *node.matrix) appendFloat(canonical, value);
    }
    appendU32(canonical, static_cast<std::uint32_t>(rig.human_bones.size()));
    for (const auto &bone : rig.human_bones) {
        appendString(canonical, bone.name);
        appendI32(canonical, bone.source_node);
    }
    return picosha2::hash256_hex_string(canonical);
}

std::string hashTargetRig(const SkeletalModelData &rig,
                          const VrmSemanticData &semantic) {
    std::string canonical{"pelican.vrma.target-rig.v1"};
    appendString(canonical, rig.source_path);
    appendU32(canonical, static_cast<std::uint32_t>(rig.nodes.size()));
    for (const auto &node : rig.nodes) {
        appendString(canonical, node.name);
        appendI32(canonical, node.parent);
        appendFloat(canonical, node.translation.x);
        appendFloat(canonical, node.translation.y);
        appendFloat(canonical, node.translation.z);
        appendFloat(canonical, node.rotation.x);
        appendFloat(canonical, node.rotation.y);
        appendFloat(canonical, node.rotation.z);
        appendFloat(canonical, node.rotation.w);
        appendFloat(canonical, node.scale.x);
        appendFloat(canonical, node.scale.y);
        appendFloat(canonical, node.scale.z);
    }
    appendString(canonical, semantic.spec_version);
    appendU32(canonical, static_cast<std::uint32_t>(semantic.human_bones.size()));
    for (const auto &bone : semantic.human_bones) {
        appendString(canonical, bone.name);
        appendI32(canonical, bone.node);
    }
    appendU32(canonical,
              static_cast<std::uint32_t>(semantic.preset_expressions.size()));
    for (const auto &[name, unused] : semantic.preset_expressions) {
        (void)unused;
        appendString(canonical, name);
    }
    appendU32(canonical,
              static_cast<std::uint32_t>(semantic.custom_expressions.size()));
    for (const auto &[name, unused] : semantic.custom_expressions) {
        (void)unused;
        appendString(canonical, name);
    }
    return picosha2::hash256_hex_string(canonical);
}

VrmaLocalTransform sourceLocalTransform(const VrmaSourceNode &node,
                                        std::size_t index) {
    const auto context = "source node[" + std::to_string(index) + "]";
    if (!node.matrix) {
        VrmaLocalTransform result{
            .translation = {node.translation[0], node.translation[1],
                            node.translation[2]},
            .rotation = normalized({node.rotation[3], node.rotation[0],
                                    node.rotation[1], node.rotation[2]},
                                   context),
            .scale = {node.scale[0], node.scale[1], node.scale[2]},
        };
        if (!finite(result.translation) || !finite(result.scale) ||
            std::abs(result.scale.x) <= epsilon ||
            std::abs(result.scale.y) <= epsilon ||
            std::abs(result.scale.z) <= epsilon)
            fail(context + " has invalid TRS");
        return result;
    }

    const auto &values = *node.matrix;
    glm::mat4 matrix{1.0f};
    for (glm::length_t column = 0; column < 4; ++column)
        for (glm::length_t row = 0; row < 4; ++row)
            matrix[column][row] =
                values[static_cast<std::size_t>(column * 4 + row)];
    if (!std::isfinite(matrix[0][0]) || !std::isfinite(matrix[0][1]) ||
        !std::isfinite(matrix[0][2]) || !std::isfinite(matrix[1][0]) ||
        !std::isfinite(matrix[1][1]) || !std::isfinite(matrix[1][2]) ||
        !std::isfinite(matrix[2][0]) || !std::isfinite(matrix[2][1]) ||
        !std::isfinite(matrix[2][2]) || !std::isfinite(matrix[3][0]) ||
        !std::isfinite(matrix[3][1]) || !std::isfinite(matrix[3][2]) ||
        std::abs(matrix[0][3]) > epsilon || std::abs(matrix[1][3]) > epsilon ||
        std::abs(matrix[2][3]) > epsilon ||
        std::abs(matrix[3][3] - 1.0f) > epsilon)
        fail(context + " matrix is not a finite affine transform");

    const glm::vec3 columns[]{glm::vec3{matrix[0]}, glm::vec3{matrix[1]},
                              glm::vec3{matrix[2]}};
    const glm::vec3 scale{glm::length(columns[0]), glm::length(columns[1]),
                          glm::length(columns[2])};
    if (!finite(scale) || scale.x <= epsilon || scale.y <= epsilon ||
        scale.z <= epsilon)
        fail(context + " matrix has a zero scale axis");
    const glm::vec3 x = columns[0] / scale.x;
    const glm::vec3 y = columns[1] / scale.y;
    const glm::vec3 z = columns[2] / scale.z;
    if (std::abs(glm::dot(x, y)) > 1.0e-4f ||
        std::abs(glm::dot(x, z)) > 1.0e-4f ||
        std::abs(glm::dot(y, z)) > 1.0e-4f ||
        glm::determinant(glm::mat3{x, y, z}) <= epsilon)
        fail(context + " matrix has shear or reflection unsupported by R0");
    return {
        .translation = glm::vec3{matrix[3]},
        .rotation = normalized(glm::quat_cast(glm::mat3{x, y, z}), context),
        .scale = scale,
    };
}

VrmaLocalTransform targetLocalTransform(const SkeletonNodeRestPose &node,
                                        std::size_t index) {
    const auto context = "target node[" + std::to_string(index) + "]";
    if (!finite(node.translation) || !finite(node.scale) ||
        std::abs(node.scale.x) <= epsilon || std::abs(node.scale.y) <= epsilon ||
        std::abs(node.scale.z) <= epsilon)
        fail(context + " has invalid TRS");
    return {
        .translation = node.translation,
        .rotation = normalized(node.rotation, context),
        .scale = node.scale,
    };
}

glm::mat4 localMatrix(const VrmaLocalTransform &transform) {
    return glm::translate(glm::mat4{1.0f}, transform.translation) *
           glm::mat4_cast(transform.rotation) *
           glm::scale(glm::mat4{1.0f}, transform.scale);
}

glm::mat4 worldMatrix(std::size_t node, std::span<const int> parents,
                      std::span<const VrmaLocalTransform> local,
                      std::vector<glm::mat4> &world,
                      std::vector<std::uint8_t> &state, std::string_view label) {
    if (state[node] == 2) return world[node];
    if (state[node] == 1) fail(std::string{label} + " hierarchy contains a cycle");
    state[node] = 1;
    const auto parent = parents[node];
    if (parent < -1 || parent >= static_cast<int>(local.size()))
        fail(std::string{label} + " node[" + std::to_string(node) +
             "] references invalid parent " + std::to_string(parent));
    world[node] = localMatrix(local[node]);
    if (parent >= 0)
        world[node] = worldMatrix(static_cast<std::size_t>(parent), parents,
                                  local, world, state, label) *
                      world[node];
    state[node] = 2;
    return world[node];
}

std::vector<glm::mat4> worldTransforms(std::span<const int> parents,
                                      std::span<const VrmaLocalTransform> local,
                                      std::string_view label) {
    if (parents.size() != local.size())
        fail(std::string{label} + " hierarchy/rest size mismatch");
    std::vector<glm::mat4> world(local.size(), glm::mat4{1.0f});
    std::vector<std::uint8_t> state(local.size());
    for (std::size_t i = 0; i < local.size(); ++i)
        worldMatrix(i, parents, local, world, state, label);
    return world;
}

void validateVectorTrack(const VrmaVectorTrack &track, std::uint8_t components,
                         std::string_view context) {
    if (track.component_count != components || track.times.empty() ||
        track.times.size() != track.values.size())
        fail(std::string{context} + " has invalid keyframe shape");
    if (track.interpolation == VrmaInterpolation::cubic_spline &&
        (track.in_tangents.size() != track.times.size() ||
         track.out_tangents.size() != track.times.size()))
        fail(std::string{context} + " has invalid cubic-spline tangents");
    if (track.interpolation != VrmaInterpolation::cubic_spline &&
        (!track.in_tangents.empty() || !track.out_tangents.empty()))
        fail(std::string{context} + " has unexpected non-cubic tangents");
    for (std::size_t i = 0; i < track.times.size(); ++i) {
        if (!std::isfinite(track.times[i]) ||
            (i > 0 && !(track.times[i] > track.times[i - 1])))
            fail(std::string{context} + " has invalid keyframe times");
        for (std::size_t component = 0; component < components; ++component) {
            if (!std::isfinite(track.values[i][component]))
                fail(std::string{context} + " has a non-finite keyframe value");
            if (track.interpolation == VrmaInterpolation::cubic_spline &&
                (!std::isfinite(track.in_tangents[i][component]) ||
                 !std::isfinite(track.out_tangents[i][component])))
                fail(std::string{context} + " has a non-finite tangent");
        }
        if (components == 4) {
            const auto &value = track.values[i];
            normalized({value[3], value[0], value[1], value[2]}, context);
        }
    }
}

void validateScalarTrack(const VrmaScalarTrack &track, std::string_view context) {
    if (track.times.empty() || track.times.size() != track.values.size())
        fail(std::string{context} + " has invalid keyframe shape");
    if (track.interpolation == VrmaInterpolation::cubic_spline &&
        (track.in_tangents.size() != track.times.size() ||
         track.out_tangents.size() != track.times.size()))
        fail(std::string{context} + " has invalid cubic-spline tangents");
    if (track.interpolation != VrmaInterpolation::cubic_spline &&
        (!track.in_tangents.empty() || !track.out_tangents.empty()))
        fail(std::string{context} + " has unexpected non-cubic tangents");
    for (std::size_t i = 0; i < track.times.size(); ++i) {
        if (!std::isfinite(track.times[i]) || !std::isfinite(track.values[i]) ||
            (i > 0 && !(track.times[i] > track.times[i - 1])))
            fail(std::string{context} + " has an invalid keyframe");
        if (track.interpolation == VrmaInterpolation::cubic_spline &&
            (!std::isfinite(track.in_tangents[i]) ||
             !std::isfinite(track.out_tangents[i])))
            fail(std::string{context} + " has a non-finite tangent");
    }
}

struct SampleInterval {
    std::size_t previous = 0;
    std::size_t next = 0;
    float alpha = 0.0f;
    float span = 0.0f;
};

SampleInterval interval(std::span<const float> times, float time,
                        VrmaInterpolation interpolation) {
    if (time <= times.front()) return {};
    if (time >= times.back())
        return {.previous = times.size() - 1, .next = times.size() - 1};
    const auto upper = std::upper_bound(times.begin(), times.end(), time);
    const auto next = static_cast<std::size_t>(upper - times.begin());
    const auto previous = next - 1;
    const float span = times[next] - times[previous];
    return {
        .previous = previous,
        .next = interpolation == VrmaInterpolation::step ? previous : next,
        .alpha = interpolation == VrmaInterpolation::step
                     ? 0.0f
                     : (time - times[previous]) / span,
        .span = span,
    };
}

float hermite(float p0, float m0, float p1, float m1, float alpha,
              float span) {
    const float a2 = alpha * alpha;
    const float a3 = a2 * alpha;
    return (2.0f * a3 - 3.0f * a2 + 1.0f) * p0 +
           (a3 - 2.0f * a2 + alpha) * span * m0 +
           (-2.0f * a3 + 3.0f * a2) * p1 +
           (a3 - a2) * span * m1;
}

std::array<float, 4> sampleVector(const VrmaVectorTrack &track, float time) {
    const auto key = interval(track.times, time, track.interpolation);
    if (key.previous == key.next) return track.values[key.previous];
    std::array<float, 4> result{};
    for (std::size_t component = 0; component < track.component_count; ++component) {
        if (track.interpolation == VrmaInterpolation::cubic_spline) {
            result[component] = hermite(
                track.values[key.previous][component],
                track.out_tangents[key.previous][component],
                track.values[key.next][component],
                track.in_tangents[key.next][component], key.alpha, key.span);
        } else {
            result[component] =
                std::lerp(track.values[key.previous][component],
                          track.values[key.next][component], key.alpha);
        }
    }
    if (track.component_count == 4 &&
        track.interpolation == VrmaInterpolation::linear) {
        const auto &a = track.values[key.previous];
        const auto &b_value = track.values[key.next];
        const auto qa = normalized({a[3], a[0], a[1], a[2]}, "rotation track");
        auto qb = normalized({b_value[3], b_value[0], b_value[1], b_value[2]},
                             "rotation track");
        if (glm::dot(qa, qb) < 0.0f) qb = -qb;
        const auto q = normalized(glm::slerp(qa, qb, key.alpha), "rotation sample");
        result = {q.x, q.y, q.z, q.w};
    }
    return result;
}

float sampleScalar(const VrmaScalarTrack &track, float time) {
    const auto key = interval(track.times, time, track.interpolation);
    if (key.previous == key.next) return track.values[key.previous];
    if (track.interpolation == VrmaInterpolation::cubic_spline)
        return hermite(track.values[key.previous], track.out_tangents[key.previous],
                       track.values[key.next], track.in_tangents[key.next],
                       key.alpha, key.span);
    return std::lerp(track.values[key.previous], track.values[key.next], key.alpha);
}

std::unordered_map<std::string, int>
sourceBoneMap(const VrmaSourceRig &rig) {
    std::unordered_map<std::string, int> result;
    std::unordered_set<int> nodes;
    for (const auto &bone : rig.human_bones) {
        if (!isKnownVrmHumanBone(bone.name))
            fail("source humanoid contains unknown bone '" + bone.name + "'");
        if (bone.source_node < 0 ||
            bone.source_node >= static_cast<int>(rig.nodes.size()))
            fail("source humanoid bone '" + bone.name + "' has invalid node");
        if (!result.emplace(bone.name, bone.source_node).second)
            fail("source humanoid duplicates bone '" + bone.name + "'");
        if (!nodes.insert(bone.source_node).second)
            fail("source humanoid maps multiple bones to node " +
                 std::to_string(bone.source_node));
    }
    return result;
}

std::unordered_map<std::string, int>
targetBoneMap(const VrmSemanticData &semantic, std::size_t node_count) {
    if (semantic.spec_version != "1.0")
        fail("target semantic has unsupported VRMC_vrm version '" +
             semantic.spec_version + "'");
    std::unordered_map<std::string, int> result;
    std::unordered_set<int> nodes;
    for (const auto &bone : semantic.human_bones) {
        if (!bone.recognized && !isKnownVrmHumanBone(bone.name)) continue;
        if (bone.node < 0 || bone.node >= static_cast<int>(node_count))
            fail("target humanoid bone '" + bone.name + "' has invalid node");
        if (!result.emplace(bone.name, bone.node).second)
            fail("target humanoid duplicates bone '" + bone.name + "'");
        if (!nodes.insert(bone.node).second)
            fail("target humanoid maps multiple bones to node " +
                 std::to_string(bone.node));
    }
    for (const auto name : requiredHumanBones)
        if (!result.contains(std::string{name}))
            fail("target humanoid is missing required bone '" +
                 std::string{name} + "'");
    return result;
}

} // namespace

VrmaRetargetSample VrmaRetargetedClip::sample(float requested_time) const {
    if (!std::isfinite(requested_time)) fail("sample time must be finite");
    VrmaRetargetSample result;
    result.time = std::clamp(requested_time, start, end);
    result.local_transforms = target_rest_pose;
    for (const auto &channel : body_channels) {
        if (channel.target_node < 0 ||
            channel.target_node >= static_cast<int>(result.local_transforms.size()))
            fail("retargeted body channel has an invalid target node");
        auto &target = result.local_transforms[static_cast<std::size_t>(channel.target_node)];
        const auto value = sampleVector(channel.track, result.time);
        if (channel.path == VrmaBodyPath::rotation) {
            const auto source_rotation = normalized(
                {value[3], value[0], value[1], value[2]}, "body rotation sample");
            target.rotation = normalized(
                channel.target_rest.rotation *
                    glm::inverse(channel.source_rest.rotation) * source_rotation,
                "retargeted body rotation");
        } else {
            const glm::vec3 source_translation{value[0], value[1], value[2]};
            target.translation = channel.target_rest.translation +
                                 channel.translation_scale *
                                     (source_translation -
                                      channel.source_rest.translation);
        }
    }
    result.expressions.reserve(expression_channels.size());
    for (const auto &channel : expression_channels)
        result.expressions.push_back({
            .expression = channel.expression,
            .preset = channel.preset,
            .weight = sampleScalar(channel.weight, result.time),
        });
    if (gaze_channel) {
        const auto value = sampleVector(gaze_channel->rotation, result.time);
        result.gaze = VrmaGazeSample{
            .offset_from_head_bone = gaze_channel->offset_from_head_bone,
            .rotation = normalized({value[3], value[0], value[1], value[2]},
                                   "gaze rotation sample"),
        };
    }
    return result;
}

std::shared_ptr<const VrmaRetargetedClip>
retargetVrmaClip(const VrmaClip &source, const SkeletalModelData &target_rig,
                 const VrmSemanticData &target_semantic,
                 VrmaRetargetProfileOptions options) {
    if (options.version != vrmaRetargetProfileVersion)
        fail("unsupported profile version " + std::to_string(options.version));
    if (options.root_motion_policy !=
        VrmaRootMotionPolicy::preserve_hips_translation)
        fail("profile version 1 does not support root-motion extraction");
    if (!source.source_rig) fail("source clip has no source rig");
    if (!std::isfinite(source.start) || !std::isfinite(source.end) ||
        source.end < source.start)
        fail("source clip has an invalid time range");
    if (source.metadata.source_uri.empty() || source.metadata.source_fragment.empty() ||
        source.metadata.content_sha256.empty() ||
        source.metadata.import_profile.empty() || source.metadata.tool_version.empty())
        fail("source clip provenance is incomplete");

    auto result = std::make_shared<VrmaRetargetedClip>();
    result->profile.version = options.version;
    result->profile.root_motion_policy = options.root_motion_policy;
    result->start = source.start;
    result->end = source.end;
    result->profile.provenance = {
        .source_clip = source.metadata,
        .profile_version = options.version,
        .source_rig_sha256 = hashSourceRig(*source.source_rig),
        .target_rig_sha256 = hashTargetRig(target_rig, target_semantic),
    };

    std::vector<int> source_parents;
    std::vector<VrmaLocalTransform> source_rest;
    source_parents.reserve(source.source_rig->nodes.size());
    source_rest.reserve(source.source_rig->nodes.size());
    for (std::size_t i = 0; i < source.source_rig->nodes.size(); ++i) {
        source_parents.push_back(source.source_rig->nodes[i].parent);
        source_rest.push_back(sourceLocalTransform(source.source_rig->nodes[i], i));
    }
    std::vector<int> target_parents;
    target_parents.reserve(target_rig.nodes.size());
    result->target_rest_pose.reserve(target_rig.nodes.size());
    for (std::size_t i = 0; i < target_rig.nodes.size(); ++i) {
        target_parents.push_back(target_rig.nodes[i].parent);
        result->target_rest_pose.push_back(targetLocalTransform(target_rig.nodes[i], i));
    }
    const auto source_world = worldTransforms(source_parents, source_rest, "source rig");
    const auto target_world =
        worldTransforms(target_parents, result->target_rest_pose, "target rig");
    const auto source_bones = sourceBoneMap(*source.source_rig);
    const auto target_bones = targetBoneMap(target_semantic, target_rig.nodes.size());

    result->profile.bone_mappings.reserve(source.source_rig->human_bones.size());
    for (const auto &source_bone : source.source_rig->human_bones) {
        const auto target_bone = target_bones.find(source_bone.name);
        VrmaRetargetBoneMapping mapping{
            .human_bone = source_bone.name,
            .source_node = source_bone.source_node,
            .target_node = target_bone == target_bones.end() ? -1 : target_bone->second,
            .source_rest = source_rest[static_cast<std::size_t>(source_bone.source_node)],
        };
        if (mapping.target_node >= 0)
            mapping.target_rest = result->target_rest_pose[static_cast<std::size_t>(
                mapping.target_node)];
        result->profile.bone_mappings.push_back(std::move(mapping));
    }

    std::unordered_set<std::string> body_paths;
    for (const auto &channel : source.body_channels) {
        const auto path_name = channel.path == VrmaBodyPath::rotation ? "rotation"
                                                                     : "translation";
        const auto context = "body channel '" + channel.human_bone + "' " + path_name;
        validateVectorTrack(channel.track,
                            channel.path == VrmaBodyPath::rotation ? 4 : 3,
                            context);
        const auto source_bone = source_bones.find(channel.human_bone);
        if (source_bone == source_bones.end() ||
            source_bone->second != channel.source_node)
            fail(context + " does not match the source humanoid map");
        if (!body_paths.insert(channel.human_bone + ":" + path_name).second)
            fail(context + " is duplicated");
        const auto target_bone = target_bones.find(channel.human_bone);
        if (target_bone == target_bones.end()) {
            if (requiredBone(channel.human_bone))
                fail("target humanoid is missing required bone '" +
                     channel.human_bone + "'");
            result->warnings.push_back({
                .kind = VrmaRetargetWarningKind::missing_optional_target_bone,
                .subject = channel.human_bone,
                .message = "skipped " + context +
                           ": optional bone is absent from the target rig",
            });
            continue;
        }
        const int source_node = source_bone->second;
        const int target_node = target_bone->second;
        float translation_scale = 1.0f;
        if (channel.path == VrmaBodyPath::translation) {
            if (channel.human_bone != "hips")
                fail(context + ": only hips translation is supported");
            const float source_height =
                std::abs(source_world[static_cast<std::size_t>(source_node)][3].y);
            const float target_height =
                std::abs(target_world[static_cast<std::size_t>(target_node)][3].y);
            if (!std::isfinite(source_height) || source_height <= epsilon)
                fail("source hips rest height must be non-zero for translation retargeting");
            if (!std::isfinite(target_height) || target_height <= epsilon)
                fail("target hips rest height must be non-zero for translation retargeting");
            translation_scale = target_height / source_height;
            result->profile.hips_translation_scale = translation_scale;
        }
        result->body_channels.push_back({
            .human_bone = channel.human_bone,
            .source_node = source_node,
            .target_node = target_node,
            .path = channel.path,
            .track = channel.track,
            .source_rest = source_rest[static_cast<std::size_t>(source_node)],
            .target_rest =
                result->target_rest_pose[static_cast<std::size_t>(target_node)],
            .translation_scale = translation_scale,
        });
    }

    std::unordered_set<std::string> expressions;
    for (const auto &channel : source.expression_channels) {
        const auto context = "expression channel '" + channel.expression + "'";
        validateScalarTrack(channel.weight, context);
        if (channel.source_node < 0 ||
            channel.source_node >= static_cast<int>(source.source_rig->nodes.size()))
            fail(context + " has an invalid source node");
        const auto &target_set = channel.preset ? target_semantic.preset_expressions
                                                : target_semantic.custom_expressions;
        if (!target_set.contains(channel.expression)) {
            result->warnings.push_back({
                .kind = VrmaRetargetWarningKind::missing_target_expression,
                .subject = channel.expression,
                .message = "skipped " + context +
                           ": matching target expression is absent",
            });
            continue;
        }
        const auto identity = std::string{channel.preset ? "preset:" : "custom:"} +
                              channel.expression;
        if (!expressions.insert(identity).second) fail(context + " is duplicated");
        result->expression_channels.push_back(channel);
    }

    if (source.gaze_channel) {
        validateVectorTrack(source.gaze_channel->rotation, 4, "gaze channel");
        if (source.gaze_channel->source_node < 0 ||
            source.gaze_channel->source_node >=
                static_cast<int>(source.source_rig->nodes.size()))
            fail("gaze channel has an invalid source node");
        result->gaze_channel = source.gaze_channel;
    }
    return result;
}

} // namespace Pelican
