#include "seqplayer.hpp"

#include "../launchconfig.hpp"
#include "../loader/fileio.hpp"
#include "../log.hpp"
#include "../material/materialcontainer.hpp"
#include "../material/standardmaterialresource.hpp"
#include "../model/gltf.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view transformSequenceSchema = "pelican.transform_seq";
constexpr int transformSequenceVersion = 1;
constexpr std::string_view builtinSphereMesh = "builtin:sphere";
constexpr float hiddenScaleFactor = 1.0e-6f;

glm::vec3 parseVec3(const nlohmann::json &json, const char *field) {
    if (!json.contains(field) || !json.at(field).is_array() || json.at(field).size() != 3) {
        throw std::runtime_error(std::string{"transform requires vec3 field: "} + field);
    }
    const auto &value = json.at(field);
    return glm::vec3{
        value.at(0).get<float>(),
        value.at(1).get<float>(),
        value.at(2).get<float>(),
    };
}

glm::quat parseQuatXyzw(const nlohmann::json &json, const char *field) {
    if (!json.contains(field) || !json.at(field).is_array() || json.at(field).size() != 4) {
        throw std::runtime_error(std::string{"transform requires quat field: "} + field);
    }
    const auto &value = json.at(field);
    return glm::quat{
        value.at(3).get<float>(),
        value.at(0).get<float>(),
        value.at(1).get<float>(),
        value.at(2).get<float>(),
    };
}

SequenceTransform parseTransform(const nlohmann::json &json) {
    if (!json.is_object()) {
        throw std::runtime_error("transform must be an object");
    }
    return SequenceTransform{
        .pos = parseVec3(json, "pos"),
        .rotation = parseQuatXyzw(json, "rot"),
        .scale = parseVec3(json, "scale"),
    };
}

std::vector<uint32_t> parseHidden(const nlohmann::json &frame, size_t object_count) {
    std::vector<uint32_t> hidden;
    if (!frame.contains("hidden")) {
        return hidden;
    }
    if (!frame.at("hidden").is_array()) {
        throw std::runtime_error("transform_seq frame hidden field must be an array");
    }
    for (const auto &entry : frame.at("hidden")) {
        if (!entry.is_number_unsigned()) {
            throw std::runtime_error("transform_seq hidden entries must be object indexes");
        }
        const auto index = entry.get<uint32_t>();
        if (index >= object_count) {
            throw std::runtime_error("transform_seq hidden index is out of range");
        }
        hidden.push_back(index);
    }
    std::sort(hidden.begin(), hidden.end());
    hidden.erase(std::unique(hidden.begin(), hidden.end()), hidden.end());
    return hidden;
}

TransformSequenceFrame parseFrame(const nlohmann::json &json, size_t object_count) {
    if (!json.is_object()) {
        throw std::runtime_error("transform_seq frame line must be an object");
    }
    if (!json.contains("transforms") || !json.at("transforms").is_array()) {
        throw std::runtime_error("transform_seq frame requires transforms array");
    }
    if (json.at("transforms").size() != object_count) {
        throw std::runtime_error("transform_seq transform count must match header objects");
    }

    TransformSequenceFrame frame;
    frame.time = json.value("t", 0.0);
    frame.transforms.reserve(object_count);
    for (const auto &transform : json.at("transforms")) {
        frame.transforms.push_back(parseTransform(transform));
    }
    frame.hidden = parseHidden(json, object_count);
    return frame;
}

std::vector<std::string> parseObjects(const nlohmann::json &header) {
    if (!header.contains("objects") || !header.at("objects").is_array() || header.at("objects").empty()) {
        throw std::runtime_error("transform_seq header requires non-empty objects array");
    }

    std::vector<std::string> objects;
    objects.reserve(header.at("objects").size());
    for (const auto &object : header.at("objects")) {
        if (!object.is_string()) {
            throw std::runtime_error("transform_seq object names must be strings");
        }
        objects.push_back(object.get<std::string>());
    }
    return objects;
}

CommonPolygonVertData makeSphereGeometry(uint32_t segments = 24, uint32_t rings = 12) {
    CommonPolygonVertData data;

    for (uint32_t ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float phi = v * std::numbers::pi_v<float>;
        const float y = std::cos(phi);
        const float radius = std::sin(phi);

        for (uint32_t segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float theta = u * std::numbers::pi_v<float> * 2.0f;
            const glm::vec3 normal{
                radius * std::cos(theta),
                y,
                radius * std::sin(theta),
            };
            data.pos.push_back(normal * 0.5f);
            data.normal.push_back(normal);
            data.texcoord.push_back(glm::vec2{u, v});
            data.color.push_back(glm::vec4{1.0f});
        }
    }

    const uint32_t stride = segments + 1;
    for (uint32_t ring = 0; ring < rings; ++ring) {
        for (uint32_t segment = 0; segment < segments; ++segment) {
            const uint32_t a = ring * stride + segment;
            const uint32_t b = a + stride;
            const uint32_t c = b + 1;
            const uint32_t d = a + 1;
            data.indices.insert(data.indices.end(), {a, b, d, d, b, c});
        }
    }

    return data;
}

ModelTemplate createBuiltinSphereModel() {
    auto &std_material = GET_MODULE(StandardMaterialResource);
    auto &material_container = GET_MODULE(MaterialContainer);
    const auto material = material_container.registerMaterial(MaterialInfo{
        .vert_shader = std_material.standardVertShader(),
        .frag_shader = std_material.standardFragShader(),
        .base_color_texture = std_material.whiteTexture(),
        .metallic_roughness_texture = std_material.metallicRoughnessDefaultTexture(),
        .normal_texture = std_material.normalDefaultTexture(),
        .emissive_texture = std_material.emissiveDefaultTexture(),
    });

    auto primitive = GET_MODULE(VertBufContainer).addPrimitiveEntry(makeSphereGeometry());

    ModelTemplate model;
    model.material_primitives.push_back(ModelTemplate::MaterialPrimitives{
        .material = material,
        .primitives = {primitive},
    });
    return model;
}

ModelTemplate loadSequenceMesh(const std::filesystem::path &mesh_path) {
    const auto mesh = mesh_path.string();
    if (mesh == builtinSphereMesh) {
        return createBuiltinSphereModel();
    }

    if (mesh_path.extension() == ".gltf") {
        return GET_MODULE(GltfLoader).loadGltf(mesh);
    }
    return GET_MODULE(GltfLoader).loadGltfBinary(mesh);
}

} // namespace

bool TransformSequenceFrame::isHidden(uint32_t object_index) const {
    return std::binary_search(hidden.begin(), hidden.end(), object_index);
}

TransformSequence TransformSequence::fromJsonLines(std::string_view json_lines) {
    std::istringstream stream{std::string{json_lines}};
    std::string line;
    size_t line_number = 0;

    TransformSequence sequence;
    bool header_read = false;

    while (std::getline(stream, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        nlohmann::json json;
        try {
            json = nlohmann::json::parse(line);
        } catch (const std::exception &ex) {
            throw std::runtime_error("transform_seq JSON parse failed on line " + std::to_string(line_number) +
                                     ": " + ex.what());
        }

        if (!header_read) {
            if (!json.is_object()) {
                throw std::runtime_error("transform_seq header must be an object");
            }
            if (json.value("schema", std::string{}) != transformSequenceSchema) {
                throw std::runtime_error("transform_seq schema is not supported");
            }
            if (json.value("version", 0) != transformSequenceVersion) {
                throw std::runtime_error("transform_seq version is not supported");
            }
            if (!json.contains("fps") || !json.at("fps").is_number()) {
                throw std::runtime_error("transform_seq header requires numeric fps");
            }
            sequence.sequence_fps = json.at("fps").get<double>();
            if (sequence.sequence_fps <= 0.0) {
                throw std::runtime_error("transform_seq fps must be positive");
            }
            sequence.object_names = parseObjects(json);
            header_read = true;
            continue;
        }

        sequence.frame_samples.push_back(parseFrame(json, sequence.object_names.size()));
    }

    if (!header_read) {
        throw std::runtime_error("transform_seq is empty");
    }
    if (sequence.frame_samples.empty()) {
        throw std::runtime_error("transform_seq requires at least one frame");
    }
    return sequence;
}

size_t TransformSequence::sampleIndex(double time, bool loop_sequence) const {
    if (frame_samples.empty()) {
        throw std::runtime_error("transform_seq has no frames");
    }

    const auto frame_count = frame_samples.size();
    double frame_position = std::floor(std::max(0.0, time) * sequence_fps);
    if (loop_sequence) {
        frame_position = std::fmod(frame_position, static_cast<double>(frame_count));
        if (frame_position < 0.0) {
            frame_position += static_cast<double>(frame_count);
        }
        return static_cast<size_t>(frame_position);
    }

    const auto index = static_cast<size_t>(frame_position);
    return std::min(index, frame_count - 1);
}

const TransformSequenceFrame &TransformSequence::sample(double time, bool loop_sequence) const {
    return frame_samples.at(sampleIndex(time, loop_sequence));
}

TransformSequence loadTransformSequenceFile(const std::filesystem::path &path) {
    return TransformSequence::fromJsonLines(readBinaryFile(path.string()));
}

SeqPlayer::SeqPlayer() {
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    applyCameraOverride();
    if (!launch_config.play_seq) {
        return;
    }

    sequence = loadTransformSequenceFile(*launch_config.play_seq);
    loop = launch_config.seq_loop;
    initializeInstances(launch_config.seq_mesh);
    enabled = true;

    LOG_INFO(logger, "SeqPlayer loaded {} objects from {}", sequence.objects().size(),
             launch_config.play_seq->string());
}

void SeqPlayer::applyCameraOverride() {
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    if (!launch_config.camera_override) {
        return;
    }

    auto &camera = GET_MODULE(Camera);
    const auto &override = *launch_config.camera_override;
    const glm::vec3 position{override.position[0], override.position[1], override.position[2]};
    const glm::vec3 target{override.target[0], override.target[1], override.target[2]};
    const auto dir = target - position;
    if (glm::length(dir) <= 0.0f) {
        throw std::runtime_error("--camera position and target must not be identical");
    }

    camera.setPos(position);
    camera.setDir(glm::normalize(dir));
    camera.setNearFar(override.fov_y, 0.01f, 1000.0f);
}

void SeqPlayer::initializeInstances(const std::filesystem::path &mesh_path) {
    sequence_model = loadSequenceMesh(mesh_path);
    auto &instance_container = GET_MODULE(PolygonInstanceContainer);
    instances.reserve(sequence.objects().size());
    for (size_t i = 0; i < sequence.objects().size(); ++i) {
        instances.push_back(instance_container.placeModelInstance(*sequence_model));
    }
}

void SeqPlayer::update(double time) {
    if (!enabled) {
        return;
    }

    const auto &frame = sequence.sample(time, loop);
    auto &instance_container = GET_MODULE(PolygonInstanceContainer);
    for (uint32_t i = 0; i < instances.size(); ++i) {
        const auto &transform = frame.transforms.at(i);
        const auto scale = frame.isHidden(i) ? glm::vec3{hiddenScaleFactor} : transform.scale;
        instance_container.setTrs(instances[i], transform.pos, transform.rotation, scale);
    }
}

void SeqPlayer::releaseInstancesForSceneLoad() {
    if (auto *instance_container =
            FastModuleContainer::tryGet<PolygonInstanceContainer>()) {
        for (const auto instance : instances) {
            (void)instance_container->removeModelInstance(instance);
        }
    }
    instances.clear();
    enabled = false;
}

} // namespace Pelican
