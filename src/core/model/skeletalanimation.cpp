#include "skeletalanimation.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace Pelican {
namespace {

glm::mat4 localMatrix(const SkeletonNodeRestPose &pose) {
    return glm::translate(glm::mat4{1.0f}, pose.translation) * glm::mat4_cast(pose.rotation) *
           glm::scale(glm::mat4{1.0f}, pose.scale);
}

float sampleTime(const SkeletalAnimationClip &clip, double engine_time, double speed,
                 bool loop, double start_time) {
    const double begin = clip.start;
    const double duration = static_cast<double>(clip.end) - begin;
    double local = (engine_time - start_time) * speed + begin;
    if (loop && duration > 0.0) {
        local = std::fmod(local - begin, duration);
        if (local < 0.0) local += duration;
        local += begin;
    } else {
        local = std::clamp(local, begin, static_cast<double>(clip.end));
    }
    return static_cast<float>(local);
}

glm::vec4 sampleChannel(const SkeletalAnimationChannel &channel, float time) {
    if (channel.times.empty() || channel.times.size() != channel.values.size()) {
        throw std::runtime_error("skeletal animation channel has invalid keyframe data");
    }
    if (time <= channel.times.front()) return channel.values.front();
    if (time >= channel.times.back()) return channel.values.back();
    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time);
    const auto next = static_cast<std::size_t>(upper - channel.times.begin());
    const auto previous = next - 1;
    if (channel.interpolation == AnimationInterpolation::step) return channel.values[previous];
    const float span = channel.times[next] - channel.times[previous];
    const float alpha = span > 0.0f ? (time - channel.times[previous]) / span : 0.0f;
    if (channel.path == AnimationPath::rotation) {
        const auto a = glm::normalize(glm::quat{channel.values[previous].w,
                                                channel.values[previous].x,
                                                channel.values[previous].y,
                                                channel.values[previous].z});
        auto b = glm::normalize(glm::quat{channel.values[next].w, channel.values[next].x,
                                         channel.values[next].y, channel.values[next].z});
        if (glm::dot(a, b) < 0.0f) b = -b;
        const auto q = glm::normalize(glm::slerp(a, b, alpha));
        return {q.x, q.y, q.z, q.w};
    }
    return glm::mix(channel.values[previous], channel.values[next], alpha);
}

glm::mat4 worldMatrix(std::size_t node, const std::vector<SkeletonNodeRestPose> &poses,
                      std::vector<glm::mat4> &world, std::vector<std::uint8_t> &state) {
    if (state[node] == 2) return world[node];
    if (state[node] == 1) throw std::runtime_error("glTF joint hierarchy contains a cycle");
    state[node] = 1;
    const auto local = localMatrix(poses[node]);
    const auto parent = poses[node].parent;
    if (parent >= 0) {
        if (parent >= static_cast<int>(poses.size())) {
            throw std::runtime_error("glTF joint hierarchy references an invalid parent node");
        }
        world[node] = worldMatrix(static_cast<std::size_t>(parent), poses, world, state) * local;
    } else {
        world[node] = local;
    }
    state[node] = 2;
    return world[node];
}

} // namespace

std::vector<glm::mat4> evaluateSkinPalette(const SkeletalModelData &model,
                                           const SkeletalAnimationClip *clip,
                                           double engine_time, double speed,
                                           bool loop, double start_time) {
    if (model.joint_nodes.size() != model.inverse_bind_matrices.size()) {
        throw std::runtime_error("glTF skin joint/inverse bind matrix count mismatch");
    }
    if (model.joint_nodes.size() > maxSkinJoints) {
        throw std::runtime_error("glTF skin exceeds the v1 joint palette limit of 128");
    }

    auto poses = model.nodes;
    if (clip != nullptr) {
        const float time = sampleTime(*clip, engine_time, speed, loop, start_time);
        for (const auto &channel : clip->channels) {
            if (channel.node < 0 || channel.node >= static_cast<int>(poses.size())) {
                throw std::runtime_error("glTF animation targets an invalid node");
            }
            const auto value = sampleChannel(channel, time);
            auto &pose = poses[static_cast<std::size_t>(channel.node)];
            switch (channel.path) {
            case AnimationPath::translation: pose.translation = glm::vec3{value}; break;
            case AnimationPath::scale: pose.scale = glm::vec3{value}; break;
            case AnimationPath::rotation:
                pose.rotation = glm::normalize(glm::quat{value.w, value.x, value.y, value.z});
                break;
            }
        }
    }

    std::vector<glm::mat4> world(poses.size(), glm::mat4{1.0f});
    std::vector<std::uint8_t> state(poses.size(), 0);
    for (std::size_t i = 0; i < poses.size(); ++i) worldMatrix(i, poses, world, state);

    std::vector<glm::mat4> palette;
    palette.reserve(model.joint_nodes.size());
    for (std::size_t i = 0; i < model.joint_nodes.size(); ++i) {
        const auto joint = model.joint_nodes[i];
        if (joint < 0 || joint >= static_cast<int>(world.size())) {
            throw std::runtime_error("glTF skin references an invalid joint node");
        }
        palette.push_back(world[static_cast<std::size_t>(joint)] * model.inverse_bind_matrices[i]);
    }
    return palette;
}

const SkeletalAnimationClip &findAnimationClip(const SkeletalModelData &model,
                                               const std::string &name) {
    const SkeletalAnimationClip *found = nullptr;
    for (const auto &clip : model.clips) {
        if (clip.name != name) continue;
        if (found != nullptr) throw std::runtime_error("Ambiguous GLB animation fragment name '" + name + "'");
        found = &clip;
    }
    if (found == nullptr) {
        throw std::runtime_error("Unknown GLB animation fragment '" + name + "'");
    }
    return *found;
}

} // namespace Pelican
