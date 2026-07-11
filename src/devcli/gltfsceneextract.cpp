#include "gltfsceneextract.hpp"

#include <argparse/argparse.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/quaternion_double.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <nlohmann/json.hpp>
#include <tiny_gltf.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican::DevCli {

namespace {

struct NodeTrs {
    glm::dvec3 pos{0.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 scale{1.0};
};

bool isR7Identifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= 'a' && ch <= 'z') || ch == '_';
    });
}

std::string nodeLabel(const tinygltf::Node &node, int node_index) {
    return node.name.empty() ? "node[" + std::to_string(node_index) + "]" :
                               "node '" + node.name + "'";
}

void requireNodeName(const tinygltf::Node &node, int node_index) {
    if (!isR7Identifier(node.name)) {
        throw std::runtime_error("glTF " + nodeLabel(node, node_index) +
                                 " must have a non-empty [a-zA-Z0-9_] name for scene extraction");
    }
}

nlohmann::json valueToJson(const tinygltf::Value &value) {
    if (value.Type() == tinygltf::NULL_TYPE) {
        return nullptr;
    }
    if (value.IsBool()) {
        return value.Get<bool>();
    }
    if (value.IsInt()) {
        return value.Get<int>();
    }
    if (value.IsNumber()) {
        return value.Get<double>();
    }
    if (value.IsString()) {
        return value.Get<std::string>();
    }
    if (value.IsArray()) {
        auto result = nlohmann::json::array();
        for (const auto &entry : value.Get<tinygltf::Value::Array>()) {
            result.push_back(valueToJson(entry));
        }
        return result;
    }
    if (value.IsObject()) {
        auto result = nlohmann::json::object();
        for (const auto &[name, entry] : value.Get<tinygltf::Value::Object>()) {
            result[name] = valueToJson(entry);
        }
        return result;
    }
    throw std::runtime_error("glTF extras contains an unsupported binary value");
}

glm::dmat4 matrixFromGltf(const std::vector<double> &values) {
    glm::dmat4 matrix{1.0};
    for (glm::length_t col = 0; col < 4; ++col) {
        for (glm::length_t row = 0; row < 4; ++row) {
            matrix[col][row] = values[static_cast<size_t>(col * 4 + row)];
        }
    }
    return matrix;
}

NodeTrs nodeTrs(const tinygltf::Node &node, int node_index) {
    if (!node.matrix.empty()) {
        if (node.matrix.size() != 16) {
            throw std::runtime_error("glTF " + nodeLabel(node, node_index) + " matrix must contain 16 values");
        }
        NodeTrs result;
        glm::dvec3 skew;
        glm::dvec4 perspective;
        if (!glm::decompose(matrixFromGltf(node.matrix), result.scale, result.rotation,
                            result.pos, skew, perspective)) {
            throw std::runtime_error("glTF " + nodeLabel(node, node_index) + " matrix is not decomposable as TRS");
        }
        constexpr double epsilon = 1.0e-9;
        if (glm::length(skew) > epsilon ||
            glm::length(perspective - glm::dvec4{0.0, 0.0, 0.0, 1.0}) > epsilon) {
            throw std::runtime_error("glTF " + nodeLabel(node, node_index) +
                                     " matrix contains shear or perspective not representable by scene v1 TRS");
        }
        result.rotation = glm::normalize(result.rotation);
        return result;
    }

    NodeTrs result;
    if (!node.translation.empty()) {
        if (node.translation.size() != 3) {
            throw std::runtime_error("glTF " + nodeLabel(node, node_index) + " translation must contain 3 values");
        }
        result.pos = {node.translation[0], node.translation[1], node.translation[2]};
    }
    if (!node.rotation.empty()) {
        if (node.rotation.size() != 4) {
            throw std::runtime_error("glTF " + nodeLabel(node, node_index) + " rotation must contain 4 values");
        }
        result.rotation = {node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]};
    }
    if (!node.scale.empty()) {
        if (node.scale.size() != 3) {
            throw std::runtime_error("glTF " + nodeLabel(node, node_index) + " scale must contain 3 values");
        }
        result.scale = {node.scale[0], node.scale[1], node.scale[2]};
    }
    return result;
}

glm::dmat4 trsMatrix(const NodeTrs &trs) {
    return glm::translate(glm::dmat4{1.0}, trs.pos) * glm::mat4_cast(trs.rotation) *
           glm::scale(glm::dmat4{1.0}, trs.scale);
}

nlohmann::json vec3Json(const glm::dvec3 &value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

nlohmann::json transformComponent(const NodeTrs &trs) {
    return nlohmann::json{
        {"name", "transform"},
        {"pos", vec3Json(trs.pos)},
        {"rotation", {trs.rotation.x, trs.rotation.y, trs.rotation.z, trs.rotation.w}},
        {"scale", vec3Json(trs.scale)},
    };
}

double linearToSrgb(double value) {
    const auto clamped = std::clamp(value, 0.0, 1.0);
    return clamped <= 0.0031308 ? clamped * 12.92 :
                                 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
}

nlohmann::json lightComponent(const tinygltf::Model &model, const tinygltf::Node &node,
                              int node_index, const glm::dmat4 &world) {
    if (node.light < 0 || node.light >= static_cast<int>(model.lights.size())) {
        throw std::runtime_error("glTF " + nodeLabel(node, node_index) + " references invalid light index " +
                                 std::to_string(node.light));
    }
    const auto &light = model.lights[node.light];
    nlohmann::json component{{"name", "light"}, {"type", light.type}};
    const auto color = light.color.size() == 3 ? light.color : std::vector<double>{1.0, 1.0, 1.0};
    component["color"] = {linearToSrgb(color[0]), linearToSrgb(color[1]), linearToSrgb(color[2])};
    component["intensity"] = light.intensity;
    if (light.range > 0.0) {
        component["range"] = light.range;
    }

    const auto position = glm::dvec3{world * glm::dvec4{0.0, 0.0, 0.0, 1.0}};
    const auto direction = glm::normalize(glm::dmat3{world} * glm::dvec3{0.0, 0.0, -1.0});
    if (light.type == "directional") {
        component["direction"] = vec3Json(direction);
    } else if (light.type == "point") {
        component["position"] = vec3Json(position);
    } else if (light.type == "spot") {
        component["position"] = vec3Json(position);
        component["direction"] = vec3Json(direction);
        component["innerConeAngle"] = light.spot.innerConeAngle * 180.0 / std::numbers::pi;
        component["outerConeAngle"] = light.spot.outerConeAngle * 180.0 / std::numbers::pi;
    } else {
        throw std::runtime_error("glTF light '" + light.name + "' has unsupported type '" + light.type + "'");
    }
    return component;
}

nlohmann::json cameraComponent(const tinygltf::Model &model, const tinygltf::Node &node,
                               int node_index) {
    if (node.camera < 0 || node.camera >= static_cast<int>(model.cameras.size())) {
        throw std::runtime_error("glTF " + nodeLabel(node, node_index) + " references invalid camera index " +
                                 std::to_string(node.camera));
    }
    const auto &camera = model.cameras[node.camera];
    nlohmann::json component{{"name", "camera"}, {"type", camera.type}};
    if (camera.type == "perspective") {
        component["yfov"] = camera.perspective.yfov;
        component["znear"] = camera.perspective.znear;
        if (camera.perspective.zfar > 0.0) {
            component["zfar"] = camera.perspective.zfar;
        }
        if (camera.perspective.aspectRatio > 0.0) {
            component["aspect"] = camera.perspective.aspectRatio;
        }
    } else if (camera.type == "orthographic") {
        component["xmag"] = camera.orthographic.xmag;
        component["ymag"] = camera.orthographic.ymag;
        component["znear"] = camera.orthographic.znear;
        component["zfar"] = camera.orthographic.zfar;
    } else {
        throw std::runtime_error("glTF camera '" + camera.name + "' has unsupported type '" + camera.type + "'");
    }
    return component;
}

const nlohmann::json *pelicanComponents(const nlohmann::json &extras) {
    if (!extras.is_object()) {
        return nullptr;
    }
    if (const auto found = extras.find("pelican.components"); found != extras.end()) {
        return &*found;
    }
    if (const auto found = extras.find("components"); found != extras.end()) {
        return &*found;
    }
    return nullptr;
}

std::string sceneId(const tinygltf::Scene &scene, size_t scene_index, int default_scene) {
    if (!scene.name.empty()) {
        if (!isR7Identifier(scene.name)) {
            throw std::runtime_error("glTF scene name '" + scene.name + "' must match [a-zA-Z0-9_]");
        }
        return scene.name;
    }
    return static_cast<int>(scene_index) == default_scene ? "default_scene" :
                                                            "scene_" + std::to_string(scene_index);
}

class SceneExtractor {
    const tinygltf::Model &model;
    std::string source_reference;
    std::vector<uint8_t> visiting;
    std::unordered_set<int> emitted_nodes;
    std::unordered_set<std::string> emitted_names;
    nlohmann::json objects = nlohmann::json::array();

    void appendNode(int node_index, const std::optional<std::string> &parent_name,
                    const std::string &parent_path, const glm::dmat4 &parent_world) {
        if (node_index < 0 || node_index >= static_cast<int>(model.nodes.size())) {
            throw std::runtime_error("glTF scene references invalid node index " + std::to_string(node_index));
        }
        const auto &node = model.nodes[node_index];
        requireNodeName(node, node_index);
        if (visiting[node_index] != 0) {
            throw std::runtime_error("glTF node hierarchy cycle includes '" + node.name + "'");
        }
        if (!emitted_nodes.insert(node_index).second) {
            throw std::runtime_error("glTF node '" + node.name + "' has multiple parents in one scene");
        }
        if (!emitted_names.insert(node.name).second) {
            throw std::runtime_error("glTF scene contains duplicate node name '" + node.name +
                                     "', which is ambiguous for scene parent references");
        }
        visiting[node_index] = 1;

        const auto trs = nodeTrs(node, node_index);
        const auto world = parent_world * trsMatrix(trs);
        const auto full_path = parent_path.empty() ? node.name : parent_path + "/" + node.name;
        nlohmann::json object{{"name", node.name}};
        if (parent_name) {
            object["parent"] = *parent_name;
        }
        auto components = nlohmann::json::array();
        components.push_back(transformComponent(trs));
        size_t extras_target = 0;
        if (node.mesh >= 0) {
            if (node.mesh >= static_cast<int>(model.meshes.size())) {
                throw std::runtime_error("glTF " + nodeLabel(node, node_index) + " references invalid mesh index " +
                                         std::to_string(node.mesh));
            }
            components.push_back(nlohmann::json{
                {"name", "simplemodelview"},
                {"model", source_reference + "#node/" + full_path},
            });
            extras_target = components.size() - 1;
        }
        if (node.light >= 0) {
            components.push_back(lightComponent(model, node, node_index, world));
            extras_target = components.size() - 1;
        }
        if (node.camera >= 0) {
            components.push_back(cameraComponent(model, node, node_index));
            extras_target = components.size() - 1;
        }

        if (node.extras.Type() != tinygltf::NULL_TYPE) {
            const auto extras = valueToJson(node.extras);
            if (const auto *extra_components = pelicanComponents(extras)) {
                if (!extra_components->is_array()) {
                    throw std::runtime_error("glTF " + nodeLabel(node, node_index) +
                                             " extras components must be an array");
                }
                for (const auto &component : *extra_components) {
                    if (!component.is_object() || !component.contains("name") ||
                        !component.at("name").is_string()) {
                        throw std::runtime_error("glTF " + nodeLabel(node, node_index) +
                                                 " extras component requires string name");
                    }
                    components.push_back(component);
                }
            } else {
                components.at(extras_target)["params"] = extras;
            }
        }
        object["components"] = std::move(components);
        objects.push_back(std::move(object));

        for (const auto child : node.children) {
            appendNode(child, node.name, full_path, world);
        }
        visiting[node_index] = 0;
    }

  public:
    SceneExtractor(const tinygltf::Model &model_value, std::string source)
        : model(model_value), source_reference(std::move(source)), visiting(model.nodes.size(), 0) {}

    nlohmann::json extract(const tinygltf::Scene &scene) {
        for (const auto root : scene.nodes) {
            appendNode(root, std::nullopt, {}, glm::dmat4{1.0});
        }
        return nlohmann::json{{"objects", std::move(objects)}};
    }
};

std::string normalizedReference(const std::filesystem::path &path, std::string_view requested) {
    if (!requested.empty()) {
        std::string reference{requested};
        std::replace(reference.begin(), reference.end(), '\\', '/');
        return reference;
    }
    return path.generic_string();
}

} // namespace

nlohmann::json extractGltfScene(const std::filesystem::path &glb_path,
                                std::string_view source_reference) {
    auto extension = glb_path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (extension != ".glb") {
        throw std::runtime_error("--extract-scene requires a .glb file: " + glb_path.string());
    }

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string errors;
    std::string warnings;
    if (!loader.LoadBinaryFromFile(&model, &errors, &warnings, glb_path.string())) {
        throw std::runtime_error("failed to parse GLB '" + glb_path.string() + "': " + errors);
    }
    if (!warnings.empty()) {
        std::cerr << "GLB warning: " << warnings << '\n';
    }
    if (model.scenes.empty()) {
        throw std::runtime_error("GLB has no scenes: " + glb_path.string());
    }

    const auto reference = normalizedReference(glb_path, source_reference);
    nlohmann::json scenes = nlohmann::json::object();
    std::unordered_set<std::string> scene_ids;
    const auto default_scene = model.defaultScene >= 0 ? model.defaultScene : 0;
    for (size_t i = 0; i < model.scenes.size(); ++i) {
        const auto id = sceneId(model.scenes[i], i, default_scene);
        if (!scene_ids.insert(id).second) {
            throw std::runtime_error("GLB contains duplicate scene name '" + id + "'");
        }
        scenes[id] = SceneExtractor{model, reference}.extract(model.scenes[i]);
    }
    return nlohmann::json{
        {"schema", "pelican.scene"},
        {"version", 1},
        {"scenes", std::move(scenes)},
    };
}

int runGltfImportCommand(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Cli import gltf");
    program.add_argument("--extract-scene").required().metavar("glb").help(
        "extract GLB hierarchy as deterministic pelican.scene v1 JSON");
    try {
        program.parse_args(argc, argv);
        const auto input = program.get<std::string>("--extract-scene");
        std::cout << extractGltfScene(input, input).dump(2) << '\n';
    } catch (const std::exception &err) {
        std::cerr << err.what() << '\n' << program;
        return -1;
    }
    return 0;
}

} // namespace Pelican::DevCli
