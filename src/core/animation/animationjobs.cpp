#include "animationjobs.hpp"

#include "animationprobe.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Pelican::Animation {
namespace {

constexpr std::uint32_t invalidNode = std::numeric_limits<std::uint32_t>::max();

template <class Handle>
Status validateGeneration(Handle handle, const std::shared_ptr<AnimationResourceGeneration> &state) {
    if (state && state->current.load(std::memory_order_acquire) != handle.generation)
        return Status::stale_generation;
    return Status::ok;
}

TransformV1 toPublic(const SkeletonNodeRestPose &pose) {
    return {{pose.translation.x, pose.translation.y, pose.translation.z, 0.0f},
            {pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w},
            {pose.scale.x, pose.scale.y, pose.scale.z, 0.0f}};
}

glm::mat4 toGlm(const Matrix4fV1 &value) {
    glm::mat4 result;
    std::memcpy(&result[0][0], value.column_major, sizeof(value.column_major));
    return result;
}

Matrix4fV1 toPublic(const glm::mat4 &value) {
    Matrix4fV1 result{};
    std::memcpy(result.column_major, &value[0][0], sizeof(result.column_major));
    return result;
}

glm::mat4 localMatrix(const PoseViewV1 &pose, std::size_t node) {
    const auto &t = pose.translations[node];
    const auto &r = pose.rotations[node];
    const auto &s = pose.scales[node];
    return glm::translate(glm::mat4{1.0f}, {t.x, t.y, t.z}) *
           glm::mat4_cast(glm::quat{r.w, r.x, r.y, r.z}) *
           glm::scale(glm::mat4{1.0f}, {s.x, s.y, s.z});
}

Status validateView(const PoseViewV1 &view, PoseLayoutHandle layout, std::size_t joint_count) {
    if (view.struct_size < sizeof(PoseViewV1) || view.version != descriptorVersionV1)
        return Status::invalid_argument;
    if (view.reserved0 != 0 || view.reserved1 != 0) return Status::reserved_not_zero;
    if (!isValid(view.pose) || !isValid(view.layout)) return Status::invalid_handle;
    if (const auto status = ProbeRuntime::validatePoseHandle(view.pose); status != Status::ok)
        return status;
    if (view.layout.identity != layout.identity || view.layout.generation != layout.generation)
        return Status::incompatible_layout;
    if (view.joint_count != joint_count || view.element_stride != sizeof(Vec4fV1) ||
        view.translations == nullptr || view.rotations == nullptr || view.scales == nullptr)
        return Status::invalid_argument;
    return Status::ok;
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
    if (channel.times.empty() || channel.times.size() != channel.values.size())
        throw std::runtime_error("skeletal animation channel has invalid keyframe data");
    if (time <= channel.times.front()) return channel.values.front();
    if (time >= channel.times.back()) return channel.values.back();
    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time);
    const auto next = static_cast<std::size_t>(upper - channel.times.begin());
    const auto previous = next - 1;
    if (channel.interpolation == AnimationInterpolation::step) return channel.values[previous];
    const float span = channel.times[next] - channel.times[previous];
    const float alpha = span > 0.0f ? (time - channel.times[previous]) / span : 0.0f;
    if (channel.path == AnimationPath::rotation) {
        const auto a = glm::normalize(glm::quat{channel.values[previous].w, channel.values[previous].x,
                                                channel.values[previous].y, channel.values[previous].z});
        auto b = glm::normalize(glm::quat{channel.values[next].w, channel.values[next].x,
                                         channel.values[next].y, channel.values[next].z});
        if (glm::dot(a, b) < 0.0f) b = -b;
        const auto q = glm::normalize(glm::slerp(a, b, alpha));
        return {q.x, q.y, q.z, q.w};
    }
    return glm::mix(channel.values[previous], channel.values[next], alpha);
}

void appendNode(const SkeletalModelData &model, std::uint32_t original, std::vector<std::uint8_t> &state,
                AnimationRig &rig) {
    if (state[original] == 2) return;
    if (state[original] == 1) throw std::runtime_error("glTF joint hierarchy contains a cycle");
    state[original] = 1;
    const auto parent = model.nodes[original].parent;
    if (parent >= 0) {
        if (parent >= static_cast<int>(model.nodes.size()))
            throw std::runtime_error("glTF joint hierarchy references an invalid parent node");
        appendNode(model, static_cast<std::uint32_t>(parent), state, rig);
    }
    const auto layout_node = static_cast<std::uint32_t>(rig.layout_to_original.size());
    rig.original_to_layout[original] = layout_node;
    rig.layout_to_original.push_back(original);
    rig.parents.push_back(parent < 0 ? -1 : static_cast<std::int32_t>(rig.original_to_layout[parent]));
    rig.rest_pose.push_back(toPublic(model.nodes[original]));
    rig.node_names.push_back(model.nodes[original].name);
    state[original] = 2;
}

} // namespace

AnimationAsset AnimationAssetRegistry::buildAsset(
    const SkeletalModelData &model,
    std::shared_ptr<AnimationResourceGeneration> generation_state,
    std::uint32_t generation, const AnimationAsset *previous) {
    if (model.nodes.empty()) throw std::runtime_error("skeletal animation rig has no nodes");
    if (model.joint_nodes.size() != model.inverse_bind_matrices.size())
        throw std::runtime_error("glTF skin joint/inverse bind matrix count mismatch");

    AnimationAsset asset;
    asset.source = &model;
    asset.rig.generation_state = generation_state;
    asset.rig.original_to_layout.assign(model.nodes.size(), invalidNode);
    std::vector<std::uint8_t> state(model.nodes.size());
    for (std::uint32_t node = 0; node < model.nodes.size(); ++node) appendNode(model, node, state, asset.rig);

    asset.rig.handle = {previous ? previous->rig.handle.identity : next_identity_++, generation, 0};
    const bool same_layout = previous &&
                             previous->rig.parents == asset.rig.parents &&
                             previous->rig.node_names == asset.rig.node_names &&
                             previous->rig.original_to_layout == asset.rig.original_to_layout &&
                             previous->rig.layout_to_original == asset.rig.layout_to_original;
    asset.rig.layout = {
        same_layout ? previous->rig.layout.identity : next_identity_++, generation, 0};

    auto addBinding = [&](std::uint32_t offset, std::uint32_t count,
                          std::size_t binding_index) {
        if (offset > model.joint_nodes.size() || count > model.joint_nodes.size() - offset)
            throw std::runtime_error("glTF skin binding range exceeds combined palette");
        AnimationSkinBinding binding;
        binding.handle = {
            previous && binding_index < previous->skin_bindings.size()
                ? previous->skin_bindings[binding_index].handle.identity
                : next_identity_++,
            generation, 0};
        binding.source_rig = asset.rig.handle;
        binding.layout = asset.rig.layout;
        binding.generation_state = generation_state;
        binding.palette_offset = offset;
        binding.joint_layout_nodes.reserve(count);
        binding.inverse_bind_matrices.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto original = model.joint_nodes[offset + i];
            if (original < 0 || original >= static_cast<int>(model.nodes.size()))
                throw std::runtime_error("glTF skin references an invalid joint node");
            binding.joint_layout_nodes.push_back(asset.rig.original_to_layout[original]);
            binding.inverse_bind_matrices.push_back(model.inverse_bind_matrices[offset + i]);
        }
        asset.skin_bindings.push_back(std::move(binding));
    };
    if (model.skin_bindings.empty()) {
        addBinding(0, static_cast<std::uint32_t>(model.joint_nodes.size()), 0);
    } else {
        std::uint32_t expected_offset = 0;
        for (std::size_t binding_index = 0;
             binding_index < model.skin_bindings.size(); ++binding_index) {
            const auto &binding = model.skin_bindings[binding_index];
            if (binding.palette_offset != expected_offset)
                throw std::runtime_error("glTF skin bindings do not exactly cover the combined palette");
            addBinding(binding.palette_offset, binding.joint_count, binding_index);
            expected_offset += binding.joint_count;
        }
        if (expected_offset != model.joint_nodes.size())
            throw std::runtime_error("glTF skin bindings do not exactly cover the combined palette");
    }
    for (const auto &clip : model.clips) {
        std::uint64_t identity{};
        if (previous) {
            const auto found = std::find_if(
                previous->clips.begin(), previous->clips.end(), [&](const auto &candidate) {
                    return candidate.source && candidate.source->name == clip.name;
                });
            if (found != previous->clips.end()) identity = found->handle.identity;
        }
        if (identity == 0) identity = next_identity_++;
        asset.clips.push_back(
            {{identity, generation, 0}, asset.rig.handle, generation_state, &clip});
    }
    return asset;
}

const AnimationAsset &AnimationAssetRegistry::getOrCreate(const SkeletalModelData &model) {
    std::scoped_lock lock{mutex_};
    if (const auto found = assets_.find(&model); found != assets_.end()) return found->second;
    auto generation_state = std::make_shared<AnimationResourceGeneration>();
    auto asset = buildAsset(model, generation_state, 1, nullptr);
    return assets_.emplace(&model, std::move(asset)).first->second;
}

const AnimationAsset &AnimationAssetRegistry::reloadAsset(
    const SkeletalModelData &previous, const SkeletalModelData &replacement) {
    std::scoped_lock lock{mutex_};
    const auto found = assets_.find(&previous);
    if (found == assets_.end()) {
        if (const auto current = assets_.find(&replacement); current != assets_.end())
            return current->second;
        auto generation_state = std::make_shared<AnimationResourceGeneration>();
        auto asset = buildAsset(replacement, generation_state, 1, nullptr);
        return assets_.emplace(&replacement, std::move(asset)).first->second;
    }

    auto generation_state = found->second.rig.generation_state;
    auto generation = generation_state->current.load(std::memory_order_acquire) + 1;
    if (generation == 0) ++generation;
    auto replacement_asset =
        buildAsset(replacement, generation_state, generation, &found->second);
    generation_state->current.store(generation, std::memory_order_release);
    assets_.erase(found);
    return assets_.emplace(&replacement, std::move(replacement_asset)).first->second;
}

void AnimationAssetRegistry::invalidateAsset(const SkeletalModelData &model) {
    std::scoped_lock lock{mutex_};
    const auto found = assets_.find(&model);
    if (found == assets_.end()) return;
    auto &generation = found->second.rig.generation_state->current;
    if (++generation == 0) ++generation;
    assets_.erase(found);
}

void AnimationAssetRegistry::clear() {
    std::scoped_lock lock{mutex_};
    for (auto &[_, asset] : assets_) {
        auto &generation = asset.rig.generation_state->current;
        if (++generation == 0) ++generation;
    }
    assets_.clear();
    ++next_identity_;
}

const AnimationClipResource &findClip(const AnimationAsset &asset, const SkeletalAnimationClip &clip) {
    for (const auto &candidate : asset.clips)
        if (candidate.source == &clip) return candidate;
    throw std::runtime_error("animation clip is not registered with the supplied rig");
}

Status samplePoseAt(const AnimationAsset &asset, const AnimationClipResource *clip,
                    double engine_time, double speed, bool loop, double start_time,
                    PoseViewV1 &out_pose) {
    if (!std::isfinite(engine_time) || !std::isfinite(speed) || !std::isfinite(start_time))
        return Status::invalid_argument;
    if (const auto status = validateGeneration(asset.rig.handle, asset.rig.generation_state);
        status != Status::ok)
        return status;
    if (clip != nullptr) {
        if (const auto status = validateGeneration(clip->handle, clip->generation_state); status != Status::ok)
            return status;
    }
    if (const auto status = validateView(out_pose, asset.rig.layout, asset.rig.rest_pose.size());
        status != Status::ok)
        return status;
    if (clip != nullptr && (clip->source == nullptr || clip->source_rig.identity != asset.rig.handle.identity ||
                           clip->source_rig.generation != asset.rig.handle.generation))
        return Status::incompatible_layout;

    auto produced = asset.rig.rest_pose;
    if (clip != nullptr) {
        const auto time = sampleTime(*clip->source, engine_time, speed, loop, start_time);
        try {
            for (const auto &channel : clip->source->channels) {
                if (channel.node < 0 || channel.node >= static_cast<int>(asset.rig.original_to_layout.size()))
                    return Status::invalid_argument;
                const auto node = asset.rig.original_to_layout[channel.node];
                const auto value = sampleChannel(channel, time);
                switch (channel.path) {
                case AnimationPath::translation: produced[node].translation = {value.x, value.y, value.z, 0.0f}; break;
                case AnimationPath::scale: produced[node].scale = {value.x, value.y, value.z, 0.0f}; break;
                case AnimationPath::rotation: {
                    const auto q = glm::normalize(glm::quat{value.w, value.x, value.y, value.z});
                    produced[node].rotation = {q.x, q.y, q.z, q.w};
                    break;
                }
                }
            }
        } catch (const std::runtime_error &) {
            return Status::invalid_argument;
        }
    }
    for (std::size_t node = 0; node < produced.size(); ++node) {
        out_pose.translations[node] = produced[node].translation;
        out_pose.rotations[node] = produced[node].rotation;
        out_pose.scales[node] = produced[node].scale;
    }
    return Status::ok;
}

Status blendNormal(const AnimationRig &rig, std::span<const NormalBlendInput> inputs, PoseViewV1 &out_pose) {
    if (const auto status = validateGeneration(rig.handle, rig.generation_state); status != Status::ok)
        return status;
    if (const auto status = validateView(out_pose, rig.layout, rig.rest_pose.size()); status != Status::ok)
        return status;
    for (const auto &input : inputs) {
        if (input.pose == nullptr) return Status::invalid_argument;
        if (const auto status = validateView(*input.pose, rig.layout, rig.rest_pose.size()); status != Status::ok)
            return status;
        if (!input.joint_weights.empty() && input.joint_weights.size() != rig.rest_pose.size())
            return Status::invalid_argument;
    }
    std::vector<QuatfV1> rotations(inputs.size());
    std::vector<float> weights(inputs.size());
    for (std::size_t joint = 0; joint < rig.rest_pose.size(); ++joint) {
        double total = 0.0, tx = 0.0, ty = 0.0, tz = 0.0, sx = 0.0, sy = 0.0, sz = 0.0;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            auto weight = std::isfinite(inputs[i].weight) && inputs[i].weight > 0.0f ? inputs[i].weight : 0.0f;
            if (!inputs[i].joint_weights.empty()) {
                const auto joint_weight = inputs[i].joint_weights[joint];
                weight *= std::isfinite(joint_weight) && joint_weight > 0.0f ? joint_weight : 0.0f;
            }
            weights[i] = weight;
            rotations[i] = inputs[i].pose->rotations[joint];
            total += weight;
            tx += weight * inputs[i].pose->translations[joint].x;
            ty += weight * inputs[i].pose->translations[joint].y;
            tz += weight * inputs[i].pose->translations[joint].z;
            sx += weight * inputs[i].pose->scales[joint].x;
            sy += weight * inputs[i].pose->scales[joint].y;
            sz += weight * inputs[i].pose->scales[joint].z;
        }
        if (!(total > 0.0) || !std::isfinite(total)) {
            out_pose.translations[joint] = rig.rest_pose[joint].translation;
            out_pose.rotations[joint] = rig.rest_pose[joint].rotation;
            out_pose.scales[joint] = rig.rest_pose[joint].scale;
            continue;
        }
        out_pose.translations[joint] = {static_cast<float>(tx / total), static_cast<float>(ty / total),
                                        static_cast<float>(tz / total), 0.0f};
        out_pose.scales[joint] = {static_cast<float>(sx / total), static_cast<float>(sy / total),
                                  static_cast<float>(sz / total), 0.0f};
        out_pose.rotations[joint] = ProbeRuntime::blendQuaternions(rotations, weights);
    }
    return Status::ok;
}

Status localToModel(const AnimationRig &rig, const PoseViewV1 &local_pose,
                    std::span<Matrix4fV1> model_matrices, PoseViewV1 *model_pose) {
    if (const auto status = validateGeneration(rig.handle, rig.generation_state); status != Status::ok)
        return status;
    if (const auto status = validateView(local_pose, rig.layout, rig.rest_pose.size()); status != Status::ok)
        return status;
    if (model_matrices.size() < rig.rest_pose.size()) return Status::buffer_too_small;
    if (model_pose != nullptr) {
        if (const auto status = validateView(*model_pose, rig.layout, rig.rest_pose.size()); status != Status::ok)
            return status;
    }
    std::vector<glm::mat4> matrices(rig.rest_pose.size());
    std::vector<TransformV1> produced_model_pose(rig.rest_pose.size());
    for (std::size_t node = 0; node < rig.rest_pose.size(); ++node) {
        const auto parent = rig.parents[node];
        if (parent < -1 || parent >= static_cast<std::int32_t>(node)) return Status::invalid_argument;
        const auto local = localMatrix(local_pose, node);
        matrices[node] = parent < 0 ? local : matrices[parent] * local;
        if (model_pose != nullptr) {
            const auto translation = glm::vec3{matrices[node][3]};
            const glm::vec3 scale{glm::length(glm::vec3{matrices[node][0]}),
                                  glm::length(glm::vec3{matrices[node][1]}),
                                  glm::length(glm::vec3{matrices[node][2]})};
            glm::mat3 rotation_matrix{matrices[node]};
            for (int column = 0; column < 3; ++column)
                if (scale[column] != 0.0f) rotation_matrix[column] /= scale[column];
            const auto rotation = glm::normalize(glm::quat_cast(rotation_matrix));
            produced_model_pose[node] = {{translation.x, translation.y, translation.z, 0.0f},
                                         {rotation.x, rotation.y, rotation.z, rotation.w},
                                         {scale.x, scale.y, scale.z, 0.0f}};
        }
    }
    for (std::size_t node = 0; node < matrices.size(); ++node) model_matrices[node] = toPublic(matrices[node]);
    if (model_pose != nullptr) {
        for (std::size_t node = 0; node < produced_model_pose.size(); ++node) {
            model_pose->translations[node] = produced_model_pose[node].translation;
            model_pose->rotations[node] = produced_model_pose[node].rotation;
            model_pose->scales[node] = produced_model_pose[node].scale;
        }
    }
    return Status::ok;
}

Status buildSkinPalette(const AnimationAsset &asset, std::span<const Matrix4fV1> model_matrices,
                        std::span<Matrix4fV1> palette) {
    if (const auto status = validateGeneration(asset.rig.handle, asset.rig.generation_state);
        status != Status::ok)
        return status;
    if (model_matrices.size() < asset.rig.rest_pose.size()) return Status::invalid_argument;
    const auto required = asset.source->joint_nodes.size();
    if (palette.size() < required) return Status::buffer_too_small;
    std::vector<Matrix4fV1> produced(required);
    for (const auto &binding : asset.skin_bindings) {
        if (const auto status = validateGeneration(binding.handle, binding.generation_state);
            status != Status::ok)
            return status;
        if (binding.joint_layout_nodes.size() != binding.inverse_bind_matrices.size())
            return Status::invalid_argument;
        if (binding.palette_offset > produced.size() ||
            binding.joint_layout_nodes.size() > produced.size() - binding.palette_offset)
            return Status::invalid_argument;
        for (std::size_t joint = 0; joint < binding.joint_layout_nodes.size(); ++joint) {
            const auto node = binding.joint_layout_nodes[joint];
            if (node >= model_matrices.size()) return Status::invalid_argument;
            produced[binding.palette_offset + joint] =
                toPublic(toGlm(model_matrices[node]) * binding.inverse_bind_matrices[joint]);
        }
    }
    std::copy(produced.begin(), produced.end(), palette.begin());
    return Status::ok;
}

} // namespace Pelican::Animation
