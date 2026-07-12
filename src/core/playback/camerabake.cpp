#include "camerabake.hpp"

#include "../renderer/camera.hpp"

#include <cctype>
#include <cmath>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {

namespace {

glm::vec3 normalizeOr(glm::vec3 value, glm::vec3 fallback) {
    const auto length_squared = glm::dot(value, value);
    return length_squared > 0.00000001f ? value * glm::inversesqrt(length_squared)
                                        : glm::normalize(fallback);
}

std::string sanitizeObjectName(std::string value) {
    if (value.empty()) {
        return "camera";
    }
    for (auto &character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '_') {
            character = '_';
        }
    }
    return value;
}

void writeCameraSequence(const std::filesystem::path &path, double fps, const std::string &camera_name,
                         const std::vector<CameraBakeSample> &samples) {
    if (!path.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            throw std::runtime_error("failed to create camera bake directory: " +
                                     path.parent_path().string() + " (" + error.message() + ")");
        }
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        throw std::runtime_error("failed to write camera transform_seq: " + path.string());
    }
    output << nlohmann::json{{"fps", fps},
                             {"generator", "pelican2-camera-bake"},
                             {"objects", nlohmann::json::array({camera_name})},
                             {"schema", "pelican.transform_seq"},
                             {"version", 1}}
                  .dump()
           << '\n';
    for (const auto &sample : samples) {
        const auto transform = nlohmann::json{{"pos", sample.pos},
                                               {"rot", sample.rotation_xyzw},
                                               {"scale", {1.0f, 1.0f, 1.0f}}};
        output << nlohmann::json{{"t", sample.time},
                                 {"transforms", nlohmann::json::array({transform})}}
                      .dump()
               << '\n';
    }
    if (!output) {
        throw std::runtime_error("failed while writing camera transform_seq: " + path.string());
    }
}

} // namespace

CameraBakeRecorder::~CameraBakeRecorder() {
    if (active) {
        try {
            finish();
        } catch (...) {
        }
    }
}

void CameraBakeRecorder::start(const std::filesystem::path &path, double sample_fps) {
    if (active) {
        throw std::runtime_error("camera bake is already active");
    }
    if (!std::isfinite(sample_fps) || sample_fps <= 0.0) {
        throw std::runtime_error("camera bake fps must be positive and finite");
    }
    output_path = path;
    fps = sample_fps;
    samples.clear();
    camera_name = "camera";
    active = true;
}

void CameraBakeRecorder::recordFrame() {
    if (!active) {
        return;
    }
    const auto &camera = GET_MODULE(Camera);
    if (samples.empty()) {
        camera_name = sanitizeObjectName(camera.activeCameraName());
    }
    const auto position = camera.getPos();
    const auto direction = normalizeOr(camera.getDir(), glm::vec3{0.0f, 0.0f, 1.0f});
    const auto right = normalizeOr(glm::cross(camera.getUp(), direction), glm::vec3{1.0f, 0.0f, 0.0f});
    const auto up = normalizeOr(glm::cross(direction, right), glm::vec3{0.0f, 1.0f, 0.0f});
    const auto rotation = glm::normalize(glm::quat_cast(glm::mat3{right, up, direction}));
    samples.push_back(CameraBakeSample{
        .time = static_cast<double>(samples.size()) / fps,
        .pos = {position.x, position.y, position.z},
        .rotation_xyzw = {rotation.x, rotation.y, rotation.z, rotation.w},
    });
}

void CameraBakeRecorder::finish() {
    if (!active) {
        throw std::runtime_error("camera bake is not active");
    }
    if (samples.empty()) {
        throw std::runtime_error("camera bake requires at least one replayed frame");
    }
    writeCameraSequence(output_path, fps, camera_name, samples);
    active = false;
}

} // namespace Pelican
