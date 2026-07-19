#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

enum class VrmaInterpolation : std::uint8_t { linear, step, cubic_spline };
enum class VrmaBodyPath : std::uint8_t { translation, rotation };

// Values use xyzw storage. Translation tracks have component_count == 3 and
// rotation tracks have component_count == 4. Cubic-spline tangents are kept
// separately so the decoder does not lose glTF animation data.
struct VrmaVectorTrack {
    VrmaInterpolation interpolation = VrmaInterpolation::linear;
    std::uint8_t component_count = 0;
    std::vector<float> times;
    std::vector<std::array<float, 4>> values;
    std::vector<std::array<float, 4>> in_tangents;
    std::vector<std::array<float, 4>> out_tangents;
};

struct VrmaScalarTrack {
    VrmaInterpolation interpolation = VrmaInterpolation::linear;
    std::vector<float> times;
    std::vector<float> values;
    std::vector<float> in_tangents;
    std::vector<float> out_tangents;
};

struct VrmaBodyChannel {
    std::string human_bone;
    int source_node = -1;
    VrmaBodyPath path = VrmaBodyPath::rotation;
    VrmaVectorTrack track;
};

struct VrmaExpressionChannel {
    std::string expression;
    bool preset = false;
    int source_node = -1;
    VrmaScalarTrack weight;
};

struct VrmaGazeChannel {
    int source_node = -1;
    std::optional<std::array<float, 3>> offset_from_head_bone;
    VrmaVectorTrack rotation;
};

struct VrmaSourceNode {
    std::string name;
    int parent = -1;
    std::array<float, 3> translation{0.0f, 0.0f, 0.0f};
    // glTF quaternion order is xyzw.
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::optional<std::array<float, 16>> matrix;
};

struct VrmaHumanoidBone {
    std::string name;
    int source_node = -1;
};

struct VrmaSourceRig {
    std::vector<VrmaSourceNode> nodes;
    std::vector<VrmaHumanoidBone> human_bones;
};

struct VrmaClipMetadata {
    std::string source_uri;
    std::string source_fragment;
    std::string content_sha256;
    std::string import_profile;
    std::string tool_version;
};

// VRMA-C0 owns decode/storage only. In particular, there is intentionally no
// root-motion delta or target-rig/application state in this resource. Hips
// translation remains an ordinary body channel until a later explicit import
// policy decides otherwise.
struct VrmaClip {
    std::string spec_version;
    std::string name;
    float start = 0.0f;
    float end = 0.0f;
    std::shared_ptr<const VrmaSourceRig> source_rig;
    std::vector<VrmaBodyChannel> body_channels;
    std::vector<VrmaExpressionChannel> expression_channels;
    std::optional<VrmaGazeChannel> gaze_channel;
    VrmaClipMetadata metadata;
};

} // namespace Pelican
