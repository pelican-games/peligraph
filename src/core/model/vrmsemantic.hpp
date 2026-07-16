#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tinygltf {
class Model;
}

namespace Pelican {

enum class VrmDiagnosticSeverity : std::uint8_t { info, warning };

struct VrmDiagnostic {
    VrmDiagnosticSeverity severity = VrmDiagnosticSeverity::warning;
    std::string message;
};

struct VrmHumanBone {
    std::string name;
    int node = -1;
    std::string node_name;
    bool recognized = false;
    bool required = false;
};

struct VrmMorphTargetBind {
    int node = -1;
    int index = -1;
    double weight = 0.0;
};

struct VrmMaterialColorBind {
    int material = -1;
    std::string type;
    std::array<double, 4> target_value{};
};

struct VrmTextureTransformBind {
    int material = -1;
    std::array<double, 2> scale{1.0, 1.0};
    std::array<double, 2> offset{0.0, 0.0};
};

struct VrmExpression {
    bool is_binary = false;
    std::string override_blink = "none";
    std::string override_look_at = "none";
    std::string override_mouth = "none";
    std::vector<VrmMorphTargetBind> morph_target_binds;
    std::vector<VrmMaterialColorBind> material_color_binds;
    std::vector<VrmTextureTransformBind> texture_transform_binds;
};

struct VrmLookAtRangeMap {
    std::optional<double> input_max_value;
    std::optional<double> output_scale;
};

struct VrmLookAtData {
    std::optional<std::array<double, 3>> offset_from_head_bone;
    std::optional<std::string> type;
    std::optional<VrmLookAtRangeMap> horizontal_inner;
    std::optional<VrmLookAtRangeMap> horizontal_outer;
    std::optional<VrmLookAtRangeMap> vertical_down;
    std::optional<VrmLookAtRangeMap> vertical_up;
};

struct VrmFirstPersonMeshAnnotation {
    int node = -1;
    std::string type;
};

struct VrmFirstPersonData {
    std::vector<VrmFirstPersonMeshAnnotation> mesh_annotations;
};

enum class VrmNodeConstraintType : std::uint8_t { roll, aim, rotation };

struct VrmNodeConstraint {
    int destination_node = -1;
    int source_node = -1;
    std::string spec_version;
    VrmNodeConstraintType type = VrmNodeConstraintType::rotation;
    std::optional<std::string> axis;
    double weight = 1.0;
};

// This object is created completely by decodeVrmSemantic before publication.
// Runtime owners expose it through shared_ptr<const VrmSemanticData> so the
// decoded container semantics cannot be changed by an instance/application path.
struct VrmSemanticData {
    std::string spec_version;
    std::vector<VrmHumanBone> human_bones;
    std::map<std::string, VrmExpression> preset_expressions;
    std::map<std::string, VrmExpression> custom_expressions;
    std::optional<VrmLookAtData> look_at;
    std::optional<VrmFirstPersonData> first_person;
    std::vector<VrmNodeConstraint> node_constraints;
};

struct VrmSemanticDecodeResult {
    std::shared_ptr<const VrmSemanticData> semantic;
    std::vector<VrmDiagnostic> diagnostics;
};

struct VrmRigBoneMapping {
    std::string bone;
    int gltf_node = -1;
    int rig_node = -1;
};

bool isKnownVrmHumanBone(std::string_view name) noexcept;
bool isVrmPresetExpression(std::string_view name) noexcept;

// Decodes only VRMC_vrm 1.0. A legacy VRM extension or an unsupported
// VRMC_vrm version produces diagnostics and no semantic object.
VrmSemanticDecodeResult decodeVrmSemantic(const tinygltf::Model &model,
                                          std::string source_name);

// Canonical semantic JSON used by `pelican_cli vrm dump`. Key and collection
// ordering is fixed and the returned string always ends in one newline.
std::string dumpVrmSemanticCanonical(const VrmSemanticData &semantic);

// Maps glTF-space humanoid node references to an A1 AnimationRig node-name
// layout. Unnamed or absent rig nodes remain -1; ambiguous rig names are an
// error because silently choosing one would make retargeting nondeterministic.
std::vector<VrmRigBoneMapping>
mapVrmHumanoidToRig(const VrmSemanticData &semantic,
                    std::span<const std::string> gltf_node_names,
                    std::span<const std::string> rig_node_names);

} // namespace Pelican
