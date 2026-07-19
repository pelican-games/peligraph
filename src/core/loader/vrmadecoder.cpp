#include "vrmadecoder.hpp"

#include "../model/vrmsemantic.hpp"

#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <picosha2.h>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

using Object = tinygltf::Value::Object;

constexpr std::array<std::string_view, 15> required_human_bones{
    "hips",          "spine",         "head",          "leftUpperLeg",
    "leftLowerLeg",  "leftFoot",      "rightUpperLeg", "rightLowerLeg",
    "rightFoot",     "leftUpperArm",  "leftLowerArm",  "leftHand",
    "rightUpperArm", "rightLowerArm", "rightHand",
};

constexpr std::array<std::string_view, 4> look_at_expressions{
    "lookUp", "lookDown", "lookLeft", "lookRight",
};

template <class Collection>
bool contains(const Collection &values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string contextPrefix(std::string_view source_uri) {
    return "VRMA '" + std::string{source_uri} + "': ";
}

[[noreturn]] void fail(std::string_view source_uri, const std::string &message) {
    throw std::runtime_error(contextPrefix(source_uri) + message);
}

const tinygltf::Value *member(const Object &object, std::string_view name) {
    const auto found = object.find(std::string{name});
    return found == object.end() ? nullptr : &found->second;
}

const Object &requireObject(const tinygltf::Value &value, std::string_view source_uri,
                            const std::string &path) {
    if (!value.IsObject()) fail(source_uri, path + " must be an object");
    return value.Get<Object>();
}

const Object &requireObjectMember(const Object &object, std::string_view name,
                                  std::string_view source_uri, const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr) fail(source_uri, path + "." + std::string{name} + " is required");
    return requireObject(*value, source_uri, path + "." + std::string{name});
}

std::string requireString(const Object &object, std::string_view name,
                          std::string_view source_uri, const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr || !value->IsString())
        fail(source_uri, path + "." + std::string{name} + " must be a string");
    return value->Get<std::string>();
}

int requireInteger(const Object &object, std::string_view name,
                   std::string_view source_uri, const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr || !value->IsInt())
        fail(source_uri, path + "." + std::string{name} + " must be an integer");
    return value->Get<int>();
}

void requireNodeIndex(int node, const tinygltf::Model &model,
                      std::string_view source_uri, const std::string &path) {
    if (node < 0 || node >= static_cast<int>(model.nodes.size()))
        fail(source_uri, path + " references invalid glTF node index " +
                             std::to_string(node));
}

template <std::size_t N>
std::array<float, N> optionalNodeArray(const std::vector<double> &source,
                                       const std::array<float, N> &fallback,
                                       std::string_view source_uri,
                                       const std::string &path) {
    if (source.empty()) return fallback;
    if (source.size() != N)
        fail(source_uri, path + " must contain exactly " + std::to_string(N) +
                             " numbers");
    std::array<float, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        if (!std::isfinite(source[i]))
            fail(source_uri, path + "[" + std::to_string(i) + "] must be finite");
        result[i] = static_cast<float>(source[i]);
    }
    return result;
}

template <std::size_t N>
std::array<float, N> numberArray(const tinygltf::Value &value,
                                 std::string_view source_uri,
                                 const std::string &path) {
    if (!value.IsArray()) fail(source_uri, path + " must be an array");
    const auto &source = value.Get<tinygltf::Value::Array>();
    if (source.size() != N)
        fail(source_uri, path + " must contain exactly " + std::to_string(N) +
                             " numbers");
    std::array<float, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        if (!(source[i].IsInt() || source[i].IsNumber()))
            fail(source_uri, path + "[" + std::to_string(i) + "] must be a number");
        const auto value_at_i = source[i].IsInt()
                                    ? static_cast<double>(source[i].Get<int>())
                                    : source[i].Get<double>();
        if (!std::isfinite(value_at_i))
            fail(source_uri, path + "[" + std::to_string(i) + "] must be finite");
        result[i] = static_cast<float>(value_at_i);
    }
    return result;
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::string lowerExtension(std::string_view path) {
    auto extension = std::filesystem::path{path}.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

void validateAliasAndHeader(std::span<const std::uint8_t> bytes,
                            std::string_view container_name,
                            std::string_view source_uri) {
    if (lowerExtension(container_name) != ".vrma")
        fail(source_uri, ".vrma alias requires a container name ending in '.vrma'; got '" +
                             std::string{container_name} + "'");
    if (bytes.size() < 12 || readU32(bytes, 0) != 0x46546c67)
        fail(source_uri, ".vrma alias is not a binary glTF container (GLB magic missing)");
    if (readU32(bytes, 4) != 2)
        fail(source_uri, ".vrma alias requires GLB version 2");
    if (readU32(bytes, 8) != bytes.size())
        fail(source_uri, ".vrma GLB declared length does not match the decoded byte count");
}

VrmaSourceRig decodeSourceRig(const tinygltf::Model &model,
                              std::string_view source_uri) {
    VrmaSourceRig result;
    result.nodes.reserve(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const auto &node = model.nodes[i];
        const auto path = "nodes[" + std::to_string(i) + "]";
        if (!node.matrix.empty() &&
            (!node.translation.empty() || !node.rotation.empty() || !node.scale.empty()))
            fail(source_uri, path + " cannot define both matrix and TRS");
        VrmaSourceNode decoded{
            .name = node.name,
            .translation = optionalNodeArray<3>(
                node.translation, {0.0f, 0.0f, 0.0f}, source_uri, path + ".translation"),
            .rotation = optionalNodeArray<4>(
                node.rotation, {0.0f, 0.0f, 0.0f, 1.0f}, source_uri, path + ".rotation"),
            .scale = optionalNodeArray<3>(
                node.scale, {1.0f, 1.0f, 1.0f}, source_uri, path + ".scale"),
        };
        if (!node.matrix.empty())
            decoded.matrix = optionalNodeArray<16>(node.matrix, {}, source_uri,
                                                   path + ".matrix");
        result.nodes.push_back(std::move(decoded));
    }

    for (std::size_t parent = 0; parent < model.nodes.size(); ++parent) {
        for (const auto child : model.nodes[parent].children) {
            requireNodeIndex(child, model, source_uri,
                             "nodes[" + std::to_string(parent) + "].children");
            auto &parent_slot = result.nodes[static_cast<std::size_t>(child)].parent;
            if (parent_slot >= 0)
                fail(source_uri, "glTF node " + std::to_string(child) +
                                     " has multiple parents " + std::to_string(parent_slot) +
                                     " and " + std::to_string(parent));
            parent_slot = static_cast<int>(parent);
        }
    }
    std::vector<std::uint8_t> state(result.nodes.size(), 0);
    std::function<void(std::size_t)> visit = [&](std::size_t node) {
        if (state[node] == 2) return;
        if (state[node] == 1)
            fail(source_uri, "glTF source rig contains a node hierarchy cycle at node " +
                                 std::to_string(node));
        state[node] = 1;
        const auto parent = result.nodes[node].parent;
        if (parent >= 0) visit(static_cast<std::size_t>(parent));
        state[node] = 2;
    };
    for (std::size_t i = 0; i < result.nodes.size(); ++i) visit(i);
    return result;
}

enum class SemanticKind : std::uint8_t { body, expression, gaze };

struct SemanticMapping {
    SemanticKind kind = SemanticKind::body;
    std::string name;
    bool preset = false;
    std::optional<std::array<float, 3>> gaze_offset;
};

using SemanticMappings = std::unordered_map<int, SemanticMapping>;

void registerMapping(SemanticMappings &mappings, int node, SemanticMapping mapping,
                     std::string_view source_uri, const std::string &path) {
    if (const auto duplicate = mappings.find(node); duplicate != mappings.end())
        fail(source_uri, path + " reuses glTF node " + std::to_string(node) +
                             " already assigned to semantic '" + duplicate->second.name + "'");
    mappings.emplace(node, std::move(mapping));
}

SemanticMappings decodeSemanticMappings(const Object &extension,
                                        const tinygltf::Model &model,
                                        std::string_view source_uri,
                                        VrmaSourceRig &source_rig) {
    SemanticMappings mappings;
    if (const auto *humanoid_value = member(extension, "humanoid")) {
        const auto &humanoid = requireObject(*humanoid_value, source_uri,
                                             "VRMC_vrm_animation.humanoid");
        const auto &bones = requireObjectMember(humanoid, "humanBones", source_uri,
                                                "VRMC_vrm_animation.humanoid");
        std::unordered_set<std::string> present;
        for (const auto &[name, value] : bones) {
            const auto path = "VRMC_vrm_animation.humanoid.humanBones." + name;
            if (!isKnownVrmHumanBone(name))
                fail(source_uri, path + " has unknown humanoid bone name '" + name + "'");
            if (name == "leftEye" || name == "rightEye")
                fail(source_uri, path + " is forbidden in VRMC_vrm_animation; use lookAt");
            const auto &bone = requireObject(value, source_uri, path);
            const auto node = requireInteger(bone, "node", source_uri, path);
            requireNodeIndex(node, model, source_uri, path + ".node");
            registerMapping(mappings, node,
                            {.kind = SemanticKind::body, .name = name},
                            source_uri, path + ".node");
            source_rig.human_bones.push_back({name, node});
            present.insert(name);
        }
        for (const auto required : required_human_bones) {
            if (!present.contains(std::string{required}))
                fail(source_uri,
                     "VRMC_vrm_animation.humanoid.humanBones is missing required bone '" +
                         std::string{required} + "'");
        }
        std::sort(source_rig.human_bones.begin(), source_rig.human_bones.end(),
                  [](const VrmaHumanoidBone &a, const VrmaHumanoidBone &b) {
                      return a.name < b.name;
                  });
    }

    if (const auto *expressions_value = member(extension, "expressions")) {
        const auto &expressions = requireObject(*expressions_value, source_uri,
                                                "VRMC_vrm_animation.expressions");
        auto decode_set = [&](std::string_view set_name, bool preset) {
            const auto *set_value = member(expressions, set_name);
            if (set_value == nullptr) return;
            const auto base = "VRMC_vrm_animation.expressions." + std::string{set_name};
            const auto &set = requireObject(*set_value, source_uri, base);
            for (const auto &[name, value] : set) {
                const auto path = base + "." + name;
                if (preset && !isVrmPresetExpression(name))
                    fail(source_uri, path + " has unknown preset expression '" + name + "'");
                if (preset && contains(look_at_expressions, name))
                    fail(source_uri, path + " is forbidden in VRMC_vrm_animation; use lookAt");
                if (!preset && (name.empty() || isVrmPresetExpression(name)))
                    fail(source_uri, path + " custom expression name collides with a preset");
                const auto &expression = requireObject(value, source_uri, path);
                const auto node = requireInteger(expression, "node", source_uri, path);
                requireNodeIndex(node, model, source_uri, path + ".node");
                registerMapping(mappings, node,
                                {.kind = SemanticKind::expression,
                                 .name = name,
                                 .preset = preset},
                                source_uri, path + ".node");
            }
        };
        decode_set("preset", true);
        decode_set("custom", false);
    }

    if (const auto *look_at_value = member(extension, "lookAt")) {
        const auto path = std::string{"VRMC_vrm_animation.lookAt"};
        const auto &look_at = requireObject(*look_at_value, source_uri, path);
        const auto node = requireInteger(look_at, "node", source_uri, path);
        requireNodeIndex(node, model, source_uri, path + ".node");
        std::optional<std::array<float, 3>> offset;
        if (const auto *value = member(look_at, "offsetFromHeadBone"))
            offset = numberArray<3>(*value, source_uri, path + ".offsetFromHeadBone");
        registerMapping(mappings, node,
                        {.kind = SemanticKind::gaze,
                         .name = "lookAt",
                         .gaze_offset = offset},
                        source_uri, path + ".node");
    }
    return mappings;
}

std::vector<std::array<float, 4>>
readFloatAccessor(const tinygltf::Model &model, int accessor_index,
                  int expected_type, std::uint8_t component_count,
                  std::string_view source_uri, const std::string &path) {
    if (accessor_index < 0 || accessor_index >= static_cast<int>(model.accessors.size()))
        fail(source_uri, path + " references invalid accessor index " +
                             std::to_string(accessor_index));
    const auto &accessor = model.accessors[static_cast<std::size_t>(accessor_index)];
    if (accessor.type != expected_type || accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
        accessor.normalized)
        fail(source_uri, path + " accessor must be non-normalized FLOAT with the expected type");
    if (accessor.sparse.isSparse)
        fail(source_uri, path + " uses a sparse accessor, unsupported by VRMA-C0");
    if (accessor.bufferView < 0 ||
        accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
        fail(source_uri, path + " accessor has no valid bufferView");
    const auto &view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
        fail(source_uri, path + " bufferView references an invalid buffer");
    const auto &buffer = model.buffers[static_cast<std::size_t>(view.buffer)].data;
    const auto stride_value = accessor.ByteStride(view);
    const std::size_t element_size = sizeof(float) * component_count;
    if (stride_value < static_cast<int>(element_size))
        fail(source_uri, path + " accessor has an invalid byte stride");
    const auto stride = static_cast<std::size_t>(stride_value);
    const auto view_begin = static_cast<std::size_t>(view.byteOffset);
    const auto view_size = static_cast<std::size_t>(view.byteLength);
    if (view_begin > buffer.size() || view_size > buffer.size() - view_begin)
        fail(source_uri, path + " bufferView exceeds its buffer");
    const auto accessor_offset = static_cast<std::size_t>(accessor.byteOffset);
    if (accessor_offset > view_size)
        fail(source_uri, path + " accessor byteOffset exceeds its bufferView");
    const auto begin = view_begin + accessor_offset;
    const auto available = view_size - accessor_offset;
    const auto count = static_cast<std::size_t>(accessor.count);
    if (count == 0) fail(source_uri, path + " accessor must contain at least one value");
    if ((count - 1) > (std::numeric_limits<std::size_t>::max() - element_size) / stride)
        fail(source_uri, path + " accessor byte range overflows");
    const auto required = (count - 1) * stride + element_size;
    if (required > available || required > buffer.size() - begin)
        fail(source_uri, path + " accessor data exceeds its bufferView");

    std::vector<std::array<float, 4>> result(count);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t component = 0; component < component_count; ++component) {
            float value = 0.0f;
            std::memcpy(&value, buffer.data() + begin + i * stride + component * sizeof(float),
                        sizeof(float));
            if (!std::isfinite(value))
                fail(source_uri, path + " accessor contains a non-finite value");
            result[i][component] = value;
        }
    }
    return result;
}

struct DecodedTrack {
    VrmaInterpolation interpolation = VrmaInterpolation::linear;
    std::vector<float> times;
    std::vector<std::array<float, 4>> values;
    std::vector<std::array<float, 4>> in_tangents;
    std::vector<std::array<float, 4>> out_tangents;
};

DecodedTrack decodeTrack(const tinygltf::Model &model,
                         const tinygltf::AnimationSampler &sampler,
                         int expected_output_type, std::uint8_t component_count,
                         std::string_view source_uri, const std::string &path) {
    DecodedTrack result;
    if (sampler.interpolation.empty() || sampler.interpolation == "LINEAR")
        result.interpolation = VrmaInterpolation::linear;
    else if (sampler.interpolation == "STEP")
        result.interpolation = VrmaInterpolation::step;
    else if (sampler.interpolation == "CUBICSPLINE")
        result.interpolation = VrmaInterpolation::cubic_spline;
    else
        fail(source_uri, path + " has unsupported interpolation '" +
                             sampler.interpolation + "'");

    const auto time_values = readFloatAccessor(model, sampler.input, TINYGLTF_TYPE_SCALAR,
                                               1, source_uri, path + ".input");
    result.times.reserve(time_values.size());
    for (const auto &value : time_values) result.times.push_back(value[0]);
    for (std::size_t i = 1; i < result.times.size(); ++i) {
        if (!(result.times[i] > result.times[i - 1]))
            fail(source_uri, path + ".input keyframe times must be strictly increasing");
    }

    auto output = readFloatAccessor(model, sampler.output, expected_output_type,
                                    component_count, source_uri, path + ".output");
    const auto multiplier = result.interpolation == VrmaInterpolation::cubic_spline ? 3u : 1u;
    if (output.size() != result.times.size() * multiplier)
        fail(source_uri, path + " has mismatched input/output keyframe counts");
    result.values.reserve(result.times.size());
    if (result.interpolation == VrmaInterpolation::cubic_spline) {
        result.in_tangents.reserve(result.times.size());
        result.out_tangents.reserve(result.times.size());
        for (std::size_t i = 0; i < result.times.size(); ++i) {
            result.in_tangents.push_back(output[i * 3]);
            result.values.push_back(output[i * 3 + 1]);
            result.out_tangents.push_back(output[i * 3 + 2]);
        }
    } else {
        result.values = std::move(output);
    }
    return result;
}

VrmaVectorTrack vectorTrack(DecodedTrack track, std::uint8_t component_count) {
    return {
        .interpolation = track.interpolation,
        .component_count = component_count,
        .times = std::move(track.times),
        .values = std::move(track.values),
        .in_tangents = std::move(track.in_tangents),
        .out_tangents = std::move(track.out_tangents),
    };
}

VrmaScalarTrack expressionTrack(DecodedTrack track) {
    VrmaScalarTrack result{
        .interpolation = track.interpolation,
        .times = std::move(track.times),
    };
    result.values.reserve(track.values.size());
    for (const auto &value : track.values)
        result.values.push_back(std::clamp(value[0], 0.0f, 1.0f));
    result.in_tangents.reserve(track.in_tangents.size());
    for (const auto &value : track.in_tangents) result.in_tangents.push_back(value[0]);
    result.out_tangents.reserve(track.out_tangents.size());
    for (const auto &value : track.out_tangents) result.out_tangents.push_back(value[0]);
    return result;
}

void includeTimeRange(VrmaClip &clip, const std::vector<float> &times,
                      bool &has_time_range) {
    if (times.empty()) return;
    if (!has_time_range) {
        clip.start = times.front();
        clip.end = times.back();
        has_time_range = true;
    } else {
        clip.start = std::min(clip.start, times.front());
        clip.end = std::max(clip.end, times.back());
    }
}

void decodeChannels(const tinygltf::Model &model,
                    const tinygltf::Animation &animation,
                    const SemanticMappings &mappings,
                    std::string_view source_uri, VrmaClip &clip) {
    bool has_time_range = false;
    std::unordered_set<std::string> decoded_channels;
    for (std::size_t channel_index = 0; channel_index < animation.channels.size();
         ++channel_index) {
        const auto &channel = animation.channels[channel_index];
        const auto path = "#animation/0 channel[" + std::to_string(channel_index) + "]";
        if (channel.sampler < 0 ||
            channel.sampler >= static_cast<int>(animation.samplers.size()))
            fail(source_uri, path + " references invalid sampler index " +
                                 std::to_string(channel.sampler));
        requireNodeIndex(channel.target_node, model, source_uri, path + ".target.node");
        const auto mapping = mappings.find(channel.target_node);
        if (mapping == mappings.end())
            fail(source_uri, path + " targets glTF node " +
                                 std::to_string(channel.target_node) +
                                 " without a VRMC_vrm_animation humanoid/expression/lookAt mapping");
        const auto &sampler = animation.samplers[static_cast<std::size_t>(channel.sampler)];
        const auto duplicate_key = std::to_string(static_cast<int>(mapping->second.kind)) + ":" +
                                   mapping->second.name + ":" + channel.target_path;
        if (!decoded_channels.insert(duplicate_key).second)
            fail(source_uri, path + " duplicates typed channel '" + mapping->second.name +
                                 "' path '" + channel.target_path + "'");

        switch (mapping->second.kind) {
        case SemanticKind::body: {
            VrmaBodyChannel decoded{
                .human_bone = mapping->second.name,
                .source_node = channel.target_node,
            };
            if (channel.target_path == "rotation") {
                decoded.path = VrmaBodyPath::rotation;
                decoded.track = vectorTrack(
                    decodeTrack(model, sampler, TINYGLTF_TYPE_VEC4, 4, source_uri, path), 4);
            } else if (channel.target_path == "translation" &&
                       mapping->second.name == "hips") {
                decoded.path = VrmaBodyPath::translation;
                decoded.track = vectorTrack(
                    decodeTrack(model, sampler, TINYGLTF_TYPE_VEC3, 3, source_uri, path), 3);
            } else if (channel.target_path == "translation") {
                fail(source_uri, path + " translates humanoid bone '" + mapping->second.name +
                                     "'; only hips translation is valid VRMA body data");
            } else {
                fail(source_uri, path + " has invalid body channel path '" +
                                     channel.target_path + "' for humanoid bone '" +
                                     mapping->second.name + "'");
            }
            includeTimeRange(clip, decoded.track.times, has_time_range);
            clip.body_channels.push_back(std::move(decoded));
            break;
        }
        case SemanticKind::expression: {
            if (channel.target_path != "translation")
                fail(source_uri, path + " has invalid expression channel path '" +
                                     channel.target_path + "' for expression '" +
                                     mapping->second.name + "'; expected translation");
            auto track = expressionTrack(
                decodeTrack(model, sampler, TINYGLTF_TYPE_VEC3, 3, source_uri, path));
            includeTimeRange(clip, track.times, has_time_range);
            clip.expression_channels.push_back({
                .expression = mapping->second.name,
                .preset = mapping->second.preset,
                .source_node = channel.target_node,
                .weight = std::move(track),
            });
            break;
        }
        case SemanticKind::gaze: {
            if (channel.target_path != "rotation")
                fail(source_uri, path + " has invalid gaze channel path '" +
                                     channel.target_path + "'; expected rotation");
            if (clip.gaze_channel)
                fail(source_uri, path + " duplicates the gaze rotation channel");
            auto track = vectorTrack(
                decodeTrack(model, sampler, TINYGLTF_TYPE_VEC4, 4, source_uri, path), 4);
            includeTimeRange(clip, track.times, has_time_range);
            clip.gaze_channel = VrmaGazeChannel{
                .source_node = channel.target_node,
                .offset_from_head_bone = mapping->second.gaze_offset,
                .rotation = std::move(track),
            };
            break;
        }
        }
    }
}

} // namespace

std::shared_ptr<const VrmaClip>
decodeVrmaAnimation(std::span<const std::uint8_t> bytes,
                    std::string container_name,
                    VrmaDecodeOptions options) {
    if (options.source_uri.empty()) options.source_uri = container_name;
    if (options.import_profile.empty())
        fail(options.source_uri, "DCC provenance import_profile must not be empty");
    if (options.tool_version.empty())
        fail(options.source_uri, "DCC provenance tool_version must not be empty");
    validateAliasAndHeader(bytes, container_name, options.source_uri);
    if (bytes.size() > std::numeric_limits<unsigned int>::max())
        fail(options.source_uri, ".vrma GLB exceeds the decoder byte-size limit");

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string errors;
    std::string warnings;
    if (!loader.LoadBinaryFromMemory(&model, &errors, &warnings, bytes.data(),
                                     static_cast<unsigned int>(bytes.size())))
        fail(options.source_uri, "invalid .vrma GLB: " + errors);

    if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(),
                  "VRMC_vrm_animation") == model.extensionsUsed.end())
        fail(options.source_uri,
             "extensionsUsed is missing required 'VRMC_vrm_animation' alias declaration");
    const auto found = model.extensions.find("VRMC_vrm_animation");
    if (found == model.extensions.end())
        fail(options.source_uri, "root extension 'VRMC_vrm_animation' is required");
    const auto &extension = requireObject(found->second, options.source_uri,
                                          "VRMC_vrm_animation");
    const auto version = requireString(extension, "specVersion", options.source_uri,
                                       "VRMC_vrm_animation");
    if (version != "1.0")
        fail(options.source_uri, "unsupported VRMC_vrm_animation specVersion '" + version +
                                     "' (supported: '1.0')");
    if (model.animations.empty())
        fail(options.source_uri,
             "no glTF animation is available for the default '#animation/0'");

    auto source_rig = decodeSourceRig(model, options.source_uri);
    const auto mappings = decodeSemanticMappings(extension, model, options.source_uri,
                                                 source_rig);
    const auto &animation = model.animations.front();
    VrmaClip clip{
        .spec_version = version,
        .name = animation.name.empty() ? "#animation/0" : animation.name,
        .source_rig = std::make_shared<const VrmaSourceRig>(std::move(source_rig)),
        .metadata = {
            .source_uri = std::move(options.source_uri),
            .source_fragment = "#animation/0",
            .content_sha256 = picosha2::hash256_hex_string(bytes.begin(), bytes.end()),
            .import_profile = std::move(options.import_profile),
            .tool_version = std::move(options.tool_version),
        },
    };
    decodeChannels(model, animation, mappings, clip.metadata.source_uri, clip);
    return std::make_shared<const VrmaClip>(std::move(clip));
}

std::shared_ptr<const VrmaClip>
loadVrmaAnimation(const std::filesystem::path &path, VrmaDecodeOptions options) {
    std::ifstream file{path, std::ios::binary};
    if (!file)
        throw std::runtime_error("VRMA '" + path.generic_string() +
                                 "': could not open source container");
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file},
                                    std::istreambuf_iterator<char>{}};
    return decodeVrmaAnimation(bytes, path.generic_string(), std::move(options));
}

} // namespace Pelican
