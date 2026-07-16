#include "vrmsemantic.hpp"

#include <tiny_gltf.h>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {
namespace {

using Object = tinygltf::Value::Object;
using Array = tinygltf::Value::Array;

constexpr std::array<std::string_view, 55> known_human_bones{
    "hips", "spine", "chest", "upperChest", "neck", "head", "leftEye", "rightEye",
    "jaw", "leftUpperLeg", "leftLowerLeg", "leftFoot", "leftToes", "rightUpperLeg",
    "rightLowerLeg", "rightFoot", "rightToes", "leftShoulder", "leftUpperArm",
    "leftLowerArm", "leftHand", "rightShoulder", "rightUpperArm", "rightLowerArm",
    "rightHand", "leftThumbMetacarpal", "leftThumbProximal", "leftThumbDistal",
    "leftIndexProximal", "leftIndexIntermediate", "leftIndexDistal", "leftMiddleProximal",
    "leftMiddleIntermediate", "leftMiddleDistal", "leftRingProximal",
    "leftRingIntermediate", "leftRingDistal", "leftLittleProximal",
    "leftLittleIntermediate", "leftLittleDistal", "rightThumbMetacarpal",
    "rightThumbProximal", "rightThumbDistal", "rightIndexProximal",
    "rightIndexIntermediate", "rightIndexDistal", "rightMiddleProximal",
    "rightMiddleIntermediate", "rightMiddleDistal", "rightRingProximal",
    "rightRingIntermediate", "rightRingDistal", "rightLittleProximal",
    "rightLittleIntermediate", "rightLittleDistal",
};

constexpr std::array<std::string_view, 15> required_human_bones{
    "hips",          "spine",         "head",          "leftUpperLeg",
    "leftLowerLeg",  "leftFoot",      "rightUpperLeg", "rightLowerLeg",
    "rightFoot",     "leftUpperArm",  "leftLowerArm",  "leftHand",
    "rightUpperArm", "rightLowerArm", "rightHand",
};

constexpr std::array<std::string_view, 18> preset_expressions{
    "happy", "angry", "sad", "relaxed", "surprised", "aa", "ih", "ou", "ee",
    "oh", "blink", "blinkLeft", "blinkRight", "lookUp", "lookDown", "lookLeft",
    "lookRight", "neutral",
};

template <class Collection>
bool contains(const Collection &values, std::string_view value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string contextPrefix(std::string_view source_name) {
    return "VRM semantic '" + std::string{source_name} + "': ";
}

[[noreturn]] void fail(std::string_view source_name, const std::string &message) {
    throw std::runtime_error(contextPrefix(source_name) + message);
}

const tinygltf::Value *member(const Object &object, std::string_view name) {
    const auto found = object.find(std::string{name});
    return found == object.end() ? nullptr : &found->second;
}

const Object &requireObject(const tinygltf::Value &value, std::string_view source_name,
                            const std::string &path) {
    if (!value.IsObject()) fail(source_name, path + " must be an object");
    return value.Get<Object>();
}

const Object &requireObjectMember(const Object &object, std::string_view name,
                                  std::string_view source_name, const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr) fail(source_name, path + "." + std::string{name} + " is required");
    return requireObject(*value, source_name, path + "." + std::string{name});
}

std::string requireString(const Object &object, std::string_view name,
                          std::string_view source_name, const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr || !value->IsString())
        fail(source_name, path + "." + std::string{name} + " must be a string");
    return value->Get<std::string>();
}

std::optional<std::string> optionalString(const Object &object, std::string_view name,
                                          std::string_view source_name,
                                          const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr) return std::nullopt;
    if (!value->IsString())
        fail(source_name, path + "." + std::string{name} + " must be a string");
    return value->Get<std::string>();
}

int requireInteger(const Object &object, std::string_view name, std::string_view source_name,
                   const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr || !value->IsInt())
        fail(source_name, path + "." + std::string{name} + " must be an integer");
    return value->Get<int>();
}

double requireNumber(const Object &object, std::string_view name, std::string_view source_name,
                     const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr || !(value->IsInt() || value->IsNumber()))
        fail(source_name, path + "." + std::string{name} + " must be a number");
    const double result = value->IsInt() ? static_cast<double>(value->Get<int>())
                                         : value->Get<double>();
    if (!std::isfinite(result))
        fail(source_name, path + "." + std::string{name} + " must be finite");
    return result;
}

std::optional<double> optionalNumber(const Object &object, std::string_view name,
                                     std::string_view source_name, const std::string &path) {
    if (member(object, name) == nullptr) return std::nullopt;
    return requireNumber(object, name, source_name, path);
}

bool optionalBool(const Object &object, std::string_view name, bool fallback,
                  std::string_view source_name, const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr) return fallback;
    if (!value->IsBool())
        fail(source_name, path + "." + std::string{name} + " must be a boolean");
    return value->Get<bool>();
}

const Array &requireArrayValue(const tinygltf::Value &value, std::string_view source_name,
                               const std::string &path) {
    if (!value.IsArray()) fail(source_name, path + " must be an array");
    return value.Get<Array>();
}

template <std::size_t N>
std::array<double, N> numberArray(const tinygltf::Value &value, std::string_view source_name,
                                  const std::string &path) {
    const auto &source = requireArrayValue(value, source_name, path);
    if (source.size() != N)
        fail(source_name, path + " must contain exactly " + std::to_string(N) + " numbers");
    std::array<double, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        if (!(source[i].IsInt() || source[i].IsNumber()))
            fail(source_name, path + "[" + std::to_string(i) + "] must be a number");
        result[i] = source[i].IsInt() ? static_cast<double>(source[i].Get<int>())
                                      : source[i].Get<double>();
        if (!std::isfinite(result[i]))
            fail(source_name, path + "[" + std::to_string(i) + "] must be finite");
    }
    return result;
}

template <std::size_t N>
std::optional<std::array<double, N>> optionalNumberArray(const Object &object,
                                                        std::string_view name,
                                                        std::string_view source_name,
                                                        const std::string &path) {
    const auto *value = member(object, name);
    if (value == nullptr) return std::nullopt;
    return numberArray<N>(*value, source_name, path + "." + std::string{name});
}

void requireNodeIndex(int node, const tinygltf::Model &model, std::string_view source_name,
                      const std::string &path) {
    if (node < 0 || node >= static_cast<int>(model.nodes.size()))
        fail(source_name, path + " references invalid glTF node index " + std::to_string(node));
}

void requireMaterialIndex(int material, const tinygltf::Model &model,
                          std::string_view source_name, const std::string &path) {
    if (material < 0 || material >= static_cast<int>(model.materials.size()))
        fail(source_name,
             path + " references invalid glTF material index " + std::to_string(material));
}

void requireEnum(std::string_view value, std::span<const std::string_view> allowed,
                 std::string_view source_name, const std::string &path) {
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
        fail(source_name, path + " has unsupported value '" + std::string{value} + "'");
}

VrmExpression parseExpression(const Object &object, const tinygltf::Model &model,
                              std::string_view source_name, const std::string &path) {
    VrmExpression result;
    result.is_binary = optionalBool(object, "isBinary", false, source_name, path);
    constexpr std::array<std::string_view, 3> overrides{"none", "block", "blend"};
    auto parseOverride = [&](std::string_view name) {
        auto value = optionalString(object, name, source_name, path).value_or("none");
        requireEnum(value, overrides, source_name, path + "." + std::string{name});
        return value;
    };
    result.override_blink = parseOverride("overrideBlink");
    result.override_look_at = parseOverride("overrideLookAt");
    result.override_mouth = parseOverride("overrideMouth");

    if (const auto *binds = member(object, "morphTargetBinds")) {
        const auto &array = requireArrayValue(*binds, source_name, path + ".morphTargetBinds");
        for (std::size_t i = 0; i < array.size(); ++i) {
            const auto item_path = path + ".morphTargetBinds[" + std::to_string(i) + "]";
            const auto &bind = requireObject(array[i], source_name, item_path);
            VrmMorphTargetBind decoded{
                .node = requireInteger(bind, "node", source_name, item_path),
                .index = requireInteger(bind, "index", source_name, item_path),
                .weight = requireNumber(bind, "weight", source_name, item_path),
            };
            requireNodeIndex(decoded.node, model, source_name, item_path + ".node");
            if (decoded.index < 0)
                fail(source_name, item_path + ".index must be non-negative");
            result.morph_target_binds.push_back(decoded);
        }
    }

    constexpr std::array<std::string_view, 6> material_types{
        "color", "emissionColor", "shadeColor", "matcapColor", "rimColor", "outlineColor",
    };
    if (const auto *binds = member(object, "materialColorBinds")) {
        const auto &array = requireArrayValue(*binds, source_name, path + ".materialColorBinds");
        for (std::size_t i = 0; i < array.size(); ++i) {
            const auto item_path = path + ".materialColorBinds[" + std::to_string(i) + "]";
            const auto &bind = requireObject(array[i], source_name, item_path);
            VrmMaterialColorBind decoded;
            decoded.material = requireInteger(bind, "material", source_name, item_path);
            decoded.type = requireString(bind, "type", source_name, item_path);
            requireEnum(decoded.type, material_types, source_name, item_path + ".type");
            const auto *target = member(bind, "targetValue");
            if (target == nullptr) fail(source_name, item_path + ".targetValue is required");
            decoded.target_value = numberArray<4>(*target, source_name, item_path + ".targetValue");
            requireMaterialIndex(decoded.material, model, source_name, item_path + ".material");
            result.material_color_binds.push_back(decoded);
        }
    }

    if (const auto *binds = member(object, "textureTransformBinds")) {
        const auto &array = requireArrayValue(*binds, source_name,
                                              path + ".textureTransformBinds");
        for (std::size_t i = 0; i < array.size(); ++i) {
            const auto item_path = path + ".textureTransformBinds[" + std::to_string(i) + "]";
            const auto &bind = requireObject(array[i], source_name, item_path);
            VrmTextureTransformBind decoded;
            decoded.material = requireInteger(bind, "material", source_name, item_path);
            if (const auto *scale = member(bind, "scale"))
                decoded.scale = numberArray<2>(*scale, source_name, item_path + ".scale");
            if (const auto *offset = member(bind, "offset"))
                decoded.offset = numberArray<2>(*offset, source_name, item_path + ".offset");
            requireMaterialIndex(decoded.material, model, source_name, item_path + ".material");
            result.texture_transform_binds.push_back(decoded);
        }
    }
    return result;
}

void parseExpressions(const Object &vrm, const tinygltf::Model &model,
                      std::string_view source_name, VrmSemanticData &result) {
    const auto *value = member(vrm, "expressions");
    if (value == nullptr) return;
    const auto &expressions = requireObject(*value, source_name, "VRMC_vrm.expressions");
    auto parseSet = [&](std::string_view set_name, bool preset,
                        std::map<std::string, VrmExpression> &destination) {
        const auto *set_value = member(expressions, set_name);
        if (set_value == nullptr) return;
        const auto &set = requireObject(*set_value, source_name,
                                        "VRMC_vrm.expressions." + std::string{set_name});
        for (const auto &[name, expression] : set) {
            if (preset && !isVrmPresetExpression(name))
                fail(source_name, "VRMC_vrm.expressions.preset contains unknown preset '" + name +
                                      "'");
            if (!preset && isVrmPresetExpression(name))
                fail(source_name, "VRMC_vrm.expressions.custom name '" + name +
                                      "' collides with a preset");
            const auto path = "VRMC_vrm.expressions." + std::string{set_name} + "." + name;
            destination.emplace(name,
                                parseExpression(requireObject(expression, source_name, path), model,
                                                source_name, path));
        }
    };
    parseSet("preset", true, result.preset_expressions);
    parseSet("custom", false, result.custom_expressions);
}

VrmLookAtRangeMap parseRangeMap(const tinygltf::Value &value, std::string_view source_name,
                                const std::string &path) {
    const auto &object = requireObject(value, source_name, path);
    VrmLookAtRangeMap result{
        .input_max_value = optionalNumber(object, "inputMaxValue", source_name, path),
        .output_scale = optionalNumber(object, "outputScale", source_name, path),
    };
    if (result.input_max_value && (*result.input_max_value < 0.0 || *result.input_max_value > 180.0))
        fail(source_name, path + ".inputMaxValue must be in [0, 180]");
    return result;
}

void parseLookAt(const Object &vrm, std::string_view source_name, VrmSemanticData &result) {
    const auto *value = member(vrm, "lookAt");
    if (value == nullptr) return;
    const auto path = std::string{"VRMC_vrm.lookAt"};
    const auto &object = requireObject(*value, source_name, path);
    VrmLookAtData decoded;
    decoded.offset_from_head_bone = optionalNumberArray<3>(object, "offsetFromHeadBone",
                                                            source_name, path);
    decoded.type = optionalString(object, "type", source_name, path);
    if (decoded.type) {
        constexpr std::array<std::string_view, 2> types{"bone", "expression"};
        requireEnum(*decoded.type, types, source_name, path + ".type");
    }
    auto parseOptionalRange = [&](std::string_view name,
                                  std::optional<VrmLookAtRangeMap> &destination) {
        if (const auto *range = member(object, name))
            destination = parseRangeMap(*range, source_name, path + "." + std::string{name});
    };
    parseOptionalRange("rangeMapHorizontalInner", decoded.horizontal_inner);
    parseOptionalRange("rangeMapHorizontalOuter", decoded.horizontal_outer);
    parseOptionalRange("rangeMapVerticalDown", decoded.vertical_down);
    parseOptionalRange("rangeMapVerticalUp", decoded.vertical_up);
    result.look_at = std::move(decoded);
}

void parseFirstPerson(const Object &vrm, const tinygltf::Model &model,
                      std::string_view source_name, VrmSemanticData &result) {
    const auto *value = member(vrm, "firstPerson");
    if (value == nullptr) return;
    const auto path = std::string{"VRMC_vrm.firstPerson"};
    const auto &object = requireObject(*value, source_name, path);
    VrmFirstPersonData decoded;
    if (const auto *annotations = member(object, "meshAnnotations")) {
        constexpr std::array<std::string_view, 4> types{
            "auto", "both", "thirdPersonOnly", "firstPersonOnly",
        };
        const auto &array = requireArrayValue(*annotations, source_name,
                                              path + ".meshAnnotations");
        for (std::size_t i = 0; i < array.size(); ++i) {
            const auto item_path = path + ".meshAnnotations[" + std::to_string(i) + "]";
            const auto &annotation = requireObject(array[i], source_name, item_path);
            VrmFirstPersonMeshAnnotation item{
                .node = requireInteger(annotation, "node", source_name, item_path),
                .type = requireString(annotation, "type", source_name, item_path),
            };
            requireNodeIndex(item.node, model, source_name, item_path + ".node");
            requireEnum(item.type, types, source_name, item_path + ".type");
            decoded.mesh_annotations.push_back(std::move(item));
        }
    }
    result.first_person = std::move(decoded);
}

void parseConstraints(const tinygltf::Model &model, std::string_view source_name,
                      VrmSemanticData &result, std::vector<VrmDiagnostic> &diagnostics) {
    for (std::size_t destination = 0; destination < model.nodes.size(); ++destination) {
        const auto &extensions = model.nodes[destination].extensions;
        const auto found = extensions.find("VRMC_node_constraint");
        if (found == extensions.end()) continue;
        const auto base_path = "nodes[" + std::to_string(destination) +
                               "].extensions.VRMC_node_constraint";
        const auto &extension = requireObject(found->second, source_name, base_path);
        const auto version = requireString(extension, "specVersion", source_name, base_path);
        if (version != "1.0") {
            diagnostics.push_back({
                VrmDiagnosticSeverity::warning,
                contextPrefix(source_name) + "unsupported VRMC_node_constraint specVersion '" +
                    version + "' at glTF node " + std::to_string(destination) +
                    "; constraint metadata ignored",
            });
            continue;
        }
        const auto &constraint = requireObjectMember(extension, "constraint", source_name,
                                                     base_path);
        constexpr std::array<std::pair<std::string_view, VrmNodeConstraintType>, 3> types{{
            {"roll", VrmNodeConstraintType::roll},
            {"aim", VrmNodeConstraintType::aim},
            {"rotation", VrmNodeConstraintType::rotation},
        }};
        const tinygltf::Value *selected = nullptr;
        std::string_view selected_name;
        VrmNodeConstraintType selected_type{};
        std::size_t selected_count = 0;
        for (const auto &[name, type] : types) {
            if (const auto *candidate = member(constraint, name)) {
                selected = candidate;
                selected_name = name;
                selected_type = type;
                ++selected_count;
            }
        }
        if (selected_count != 1)
            fail(source_name, base_path +
                                  ".constraint must contain exactly one of roll, aim, or rotation");
        const auto item_path = base_path + ".constraint." + std::string{selected_name};
        const auto &item = requireObject(*selected, source_name, item_path);
        VrmNodeConstraint decoded{
            .destination_node = static_cast<int>(destination),
            .source_node = requireInteger(item, "source", source_name, item_path),
            .spec_version = version,
            .type = selected_type,
            .weight = optionalNumber(item, "weight", source_name, item_path).value_or(1.0),
        };
        requireNodeIndex(decoded.source_node, model, source_name, item_path + ".source");
        if (decoded.source_node == decoded.destination_node)
            fail(source_name, item_path + ".source must not reference the destination node");
        if (decoded.weight < 0.0 || decoded.weight > 1.0)
            fail(source_name, item_path + ".weight must be in [0, 1]");
        if (selected_type == VrmNodeConstraintType::roll) {
            decoded.axis = requireString(item, "rollAxis", source_name, item_path);
            constexpr std::array<std::string_view, 3> axes{"X", "Y", "Z"};
            requireEnum(*decoded.axis, axes, source_name, item_path + ".rollAxis");
        } else if (selected_type == VrmNodeConstraintType::aim) {
            decoded.axis = requireString(item, "aimAxis", source_name, item_path);
            constexpr std::array<std::string_view, 6> axes{
                "PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ",
            };
            requireEnum(*decoded.axis, axes, source_name, item_path + ".aimAxis");
        }
        result.node_constraints.push_back(std::move(decoded));
    }
}

nlohmann::ordered_json expressionJson(const VrmExpression &expression) {
    nlohmann::ordered_json result = nlohmann::ordered_json::object();
    result["isBinary"] = expression.is_binary;
    result["overrideBlink"] = expression.override_blink;
    result["overrideLookAt"] = expression.override_look_at;
    result["overrideMouth"] = expression.override_mouth;
    if (!expression.morph_target_binds.empty()) {
        result["morphTargetBinds"] = nlohmann::ordered_json::array();
        for (const auto &bind : expression.morph_target_binds) {
            result["morphTargetBinds"].push_back(nlohmann::ordered_json{
                {"node", bind.node}, {"index", bind.index}, {"weight", bind.weight}});
        }
    }
    if (!expression.material_color_binds.empty()) {
        result["materialColorBinds"] = nlohmann::ordered_json::array();
        for (const auto &bind : expression.material_color_binds) {
            result["materialColorBinds"].push_back(nlohmann::ordered_json{
                {"material", bind.material},
                {"type", bind.type},
                {"targetValue", bind.target_value},
            });
        }
    }
    if (!expression.texture_transform_binds.empty()) {
        result["textureTransformBinds"] = nlohmann::ordered_json::array();
        for (const auto &bind : expression.texture_transform_binds) {
            result["textureTransformBinds"].push_back(nlohmann::ordered_json{
                {"material", bind.material}, {"scale", bind.scale}, {"offset", bind.offset}});
        }
    }
    return result;
}

nlohmann::ordered_json rangeMapJson(const VrmLookAtRangeMap &range) {
    nlohmann::ordered_json result = nlohmann::ordered_json::object();
    if (range.input_max_value) result["inputMaxValue"] = *range.input_max_value;
    if (range.output_scale) result["outputScale"] = *range.output_scale;
    return result;
}

std::string_view constraintName(VrmNodeConstraintType type) {
    switch (type) {
    case VrmNodeConstraintType::roll: return "roll";
    case VrmNodeConstraintType::aim: return "aim";
    case VrmNodeConstraintType::rotation: return "rotation";
    }
    return "rotation";
}

} // namespace

bool isKnownVrmHumanBone(std::string_view name) noexcept {
    return contains(known_human_bones, name);
}

bool isVrmPresetExpression(std::string_view name) noexcept {
    return contains(preset_expressions, name);
}

VrmSemanticDecodeResult decodeVrmSemantic(const tinygltf::Model &model,
                                          std::string source_name) {
    VrmSemanticDecodeResult decoded;
    const auto found = model.extensions.find("VRMC_vrm");
    if (found == model.extensions.end()) {
        if (model.extensions.contains("VRM") ||
            std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(), "VRM") !=
                model.extensionsUsed.end()) {
            decoded.diagnostics.push_back({
                VrmDiagnosticSeverity::info,
                contextPrefix(source_name) +
                    "VRM 0.x semantic is unsupported; model remains plain glTF "
                    "(conversion is planned for import-tools)",
            });
        }
        return decoded;
    }

    const auto &vrm = requireObject(found->second, source_name, "VRMC_vrm");
    const auto version = requireString(vrm, "specVersion", source_name, "VRMC_vrm");
    if (version != "1.0") {
        decoded.diagnostics.push_back({
            VrmDiagnosticSeverity::warning,
            contextPrefix(source_name) + "unsupported VRMC_vrm specVersion '" + version +
                "'; semantic metadata ignored and model remains plain glTF",
        });
        return decoded;
    }

    VrmSemanticData semantic;
    semantic.spec_version = version;
    const auto &humanoid = requireObjectMember(vrm, "humanoid", source_name, "VRMC_vrm");
    const auto &bones = requireObjectMember(humanoid, "humanBones", source_name,
                                            "VRMC_vrm.humanoid");
    std::unordered_set<std::string> present;
    std::unordered_map<int, std::string> assigned_nodes;
    for (const auto &[name, value] : bones) {
        const auto path = "VRMC_vrm.humanoid.humanBones." + name;
        const auto &bone = requireObject(value, source_name, path);
        const auto node = requireInteger(bone, "node", source_name, path);
        requireNodeIndex(node, model, source_name, path + ".node");
        if (const auto duplicate = assigned_nodes.find(node); duplicate != assigned_nodes.end())
            fail(source_name, path + ".node duplicates bone '" + duplicate->second +
                                  "' at glTF node " + std::to_string(node));
        assigned_nodes.emplace(node, name);
        present.insert(name);
        const bool recognized = isKnownVrmHumanBone(name);
        const bool required = contains(required_human_bones, name);
        semantic.human_bones.push_back({
            .name = name,
            .node = node,
            .node_name = model.nodes[static_cast<std::size_t>(node)].name,
            .recognized = recognized,
            .required = required,
        });
        if (!recognized) {
            decoded.diagnostics.push_back({
                VrmDiagnosticSeverity::warning,
                contextPrefix(source_name) + "unknown humanoid bone '" + name +
                    "' is retained",
            });
        }
    }
    for (const auto required : required_human_bones) {
        if (!present.contains(std::string{required}))
            fail(source_name, "VRMC_vrm.humanoid.humanBones is missing required bone '" +
                                  std::string{required} + "'");
    }
    std::sort(semantic.human_bones.begin(), semantic.human_bones.end(),
              [](const VrmHumanBone &a, const VrmHumanBone &b) { return a.name < b.name; });

    parseExpressions(vrm, model, source_name, semantic);
    parseLookAt(vrm, source_name, semantic);
    parseFirstPerson(vrm, model, source_name, semantic);
    parseConstraints(model, source_name, semantic, decoded.diagnostics);
    decoded.semantic = std::make_shared<const VrmSemanticData>(std::move(semantic));
    return decoded;
}

std::string dumpVrmSemanticCanonical(const VrmSemanticData &semantic) {
    nlohmann::ordered_json root = nlohmann::ordered_json::object();
    root["specVersion"] = semantic.spec_version;
    nlohmann::ordered_json bones = nlohmann::ordered_json::object();
    std::vector<const VrmHumanBone *> sorted_bones;
    sorted_bones.reserve(semantic.human_bones.size());
    for (const auto &bone : semantic.human_bones) sorted_bones.push_back(&bone);
    std::sort(sorted_bones.begin(), sorted_bones.end(), [](const auto *a, const auto *b) {
        return a->name < b->name;
    });
    for (const auto *bone : sorted_bones)
        bones[bone->name] = nlohmann::ordered_json{{"node", bone->node}};
    root["humanoid"] = nlohmann::ordered_json{{"humanBones", std::move(bones)}};

    if (!semantic.preset_expressions.empty() || !semantic.custom_expressions.empty()) {
        nlohmann::ordered_json expressions = nlohmann::ordered_json::object();
        if (!semantic.preset_expressions.empty()) {
            nlohmann::ordered_json values = nlohmann::ordered_json::object();
            for (const auto &[name, expression] : semantic.preset_expressions)
                values[name] = expressionJson(expression);
            expressions["preset"] = std::move(values);
        }
        if (!semantic.custom_expressions.empty()) {
            nlohmann::ordered_json values = nlohmann::ordered_json::object();
            for (const auto &[name, expression] : semantic.custom_expressions)
                values[name] = expressionJson(expression);
            expressions["custom"] = std::move(values);
        }
        root["expressions"] = std::move(expressions);
    }

    if (semantic.look_at) {
        nlohmann::ordered_json look_at = nlohmann::ordered_json::object();
        if (semantic.look_at->offset_from_head_bone)
            look_at["offsetFromHeadBone"] = *semantic.look_at->offset_from_head_bone;
        if (semantic.look_at->type) look_at["type"] = *semantic.look_at->type;
        if (semantic.look_at->horizontal_inner)
            look_at["rangeMapHorizontalInner"] = rangeMapJson(*semantic.look_at->horizontal_inner);
        if (semantic.look_at->horizontal_outer)
            look_at["rangeMapHorizontalOuter"] = rangeMapJson(*semantic.look_at->horizontal_outer);
        if (semantic.look_at->vertical_down)
            look_at["rangeMapVerticalDown"] = rangeMapJson(*semantic.look_at->vertical_down);
        if (semantic.look_at->vertical_up)
            look_at["rangeMapVerticalUp"] = rangeMapJson(*semantic.look_at->vertical_up);
        root["lookAt"] = std::move(look_at);
    }

    if (semantic.first_person) {
        nlohmann::ordered_json first_person = nlohmann::ordered_json::object();
        if (!semantic.first_person->mesh_annotations.empty()) {
            first_person["meshAnnotations"] = nlohmann::ordered_json::array();
            for (const auto &annotation : semantic.first_person->mesh_annotations) {
                first_person["meshAnnotations"].push_back(
                    nlohmann::ordered_json{{"node", annotation.node}, {"type", annotation.type}});
            }
        }
        root["firstPerson"] = std::move(first_person);
    }

    if (!semantic.node_constraints.empty()) {
        root["nodeConstraints"] = nlohmann::ordered_json::array();
        for (const auto &constraint : semantic.node_constraints) {
            nlohmann::ordered_json parameters = nlohmann::ordered_json::object();
            parameters["source"] = constraint.source_node;
            if (constraint.type == VrmNodeConstraintType::roll && constraint.axis)
                parameters["rollAxis"] = *constraint.axis;
            if (constraint.type == VrmNodeConstraintType::aim && constraint.axis)
                parameters["aimAxis"] = *constraint.axis;
            parameters["weight"] = constraint.weight;
            root["nodeConstraints"].push_back(nlohmann::ordered_json{
                {"node", constraint.destination_node},
                {"specVersion", constraint.spec_version},
                {"constraint", nlohmann::ordered_json{
                                   {constraintName(constraint.type), std::move(parameters)}}},
            });
        }
    }
    return root.dump(2) + '\n';
}

std::vector<VrmRigBoneMapping>
mapVrmHumanoidToRig(const VrmSemanticData &semantic,
                    std::span<const std::string> gltf_node_names,
                    std::span<const std::string> rig_node_names) {
    std::unordered_map<std::string, int> rig_by_name;
    std::unordered_set<std::string> ambiguous;
    for (std::size_t i = 0; i < rig_node_names.size(); ++i) {
        if (rig_node_names[i].empty()) continue;
        if (!rig_by_name.emplace(rig_node_names[i], static_cast<int>(i)).second)
            ambiguous.insert(rig_node_names[i]);
    }

    std::vector<VrmRigBoneMapping> result;
    result.reserve(semantic.human_bones.size());
    for (const auto &bone : semantic.human_bones) {
        if (bone.node < 0 || bone.node >= static_cast<int>(gltf_node_names.size()))
            throw std::runtime_error("VRM humanoid bone '" + bone.name +
                                     "' cannot map invalid glTF node " +
                                     std::to_string(bone.node));
        const auto &node_name = gltf_node_names[static_cast<std::size_t>(bone.node)];
        int rig_node = -1;
        if (!node_name.empty()) {
            if (ambiguous.contains(node_name))
                throw std::runtime_error("VRM humanoid bone '" + bone.name +
                                         "' maps to ambiguous rig node name '" + node_name + "'");
            if (const auto found = rig_by_name.find(node_name); found != rig_by_name.end())
                rig_node = found->second;
        }
        result.push_back({bone.name, bone.node, rig_node});
    }
    return result;
}

} // namespace Pelican
