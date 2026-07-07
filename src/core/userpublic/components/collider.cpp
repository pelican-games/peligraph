#include "collider.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace Pelican {

namespace {

vec3 readVec3Or(const nlohmann::json &json, const char *name, vec3 fallback) {
    const auto found = json.find(name);
    if (found == json.end()) {
        return fallback;
    }
    if (!found->is_array() || found->size() != 3) {
        throw std::runtime_error(std::string{"Collider field must be vec3: "} + name);
    }
    return vec3{
        found->at(0).get<float>(),
        found->at(1).get<float>(),
        found->at(2).get<float>(),
    };
}

vec3 readRequiredVec3(const nlohmann::json &json, const char *name) {
    if (!json.contains(name)) {
        throw std::runtime_error(std::string{"Collider requires vec3 field: "} + name);
    }
    return readVec3Or(json, name, {});
}

quat readQuatOr(const nlohmann::json &json, const char *name, quat fallback) {
    const auto found = json.find(name);
    if (found == json.end()) {
        return fallback;
    }
    if (!found->is_array() || found->size() != 4) {
        throw std::runtime_error(std::string{"Collider field must be quat: "} + name);
    }
    return quat{
        found->at(0).get<float>(),
        found->at(1).get<float>(),
        found->at(2).get<float>(),
        found->at(3).get<float>(),
    };
}

float readRequiredFloat(const nlohmann::json &json, const char *name) {
    if (!json.contains(name)) {
        throw std::runtime_error(std::string{"Collider requires numeric field: "} + name);
    }
    return json.at(name).get<float>();
}

bool allPositive(vec3 value) {
    return value.x > 0.0f && value.y > 0.0f && value.z > 0.0f;
}

vec3 halfExtentsFromSize(vec3 size) {
    return {size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
}

} // namespace

void ColliderComponent::loadFromJsonArchive(const JsonArchiveLoader &archive) {
    const auto &json = *static_cast<const nlohmann::json *>(archive.ptr);
    shape = json.at("shape").get<std::string>();
    pos = readVec3Or(json, "pos", {0.0f, 0.0f, 0.0f});
    rotation = readQuatOr(json, "rotation", {0.0f, 0.0f, 0.0f, 1.0f});

    if (shape == "sphere") {
        radius = readRequiredFloat(json, "radius");
    } else if (shape == "box") {
        half_extents = json.contains("half_extents") ? readRequiredVec3(json, "half_extents")
                                                     : halfExtentsFromSize(readRequiredVec3(json, "size"));
    } else if (shape == "capsule") {
        radius = readRequiredFloat(json, "radius");
        half_height = json.contains("half_height") ? readRequiredFloat(json, "half_height")
                                                   : readRequiredFloat(json, "height") * 0.5f;
    } else {
        throw std::runtime_error("Unknown collider shape: " + shape);
    }

    validate();
}

void ColliderComponent::validate() const {
    if (shape == "sphere") {
        if (radius <= 0.0f) {
            throw std::runtime_error("Sphere collider radius must be positive");
        }
    } else if (shape == "box") {
        if (!allPositive(half_extents)) {
            throw std::runtime_error("Box collider half_extents must be positive");
        }
    } else if (shape == "capsule") {
        if (radius <= 0.0f) {
            throw std::runtime_error("Capsule collider radius must be positive");
        }
        if (half_height < 0.0f) {
            throw std::runtime_error("Capsule collider half_height must be non-negative");
        }
    } else {
        throw std::runtime_error("Unknown collider shape: " + shape);
    }
}

void ColliderComponent::init() {
    validate();
}

void ColliderComponent::deinit() {}

}  // namespace Pelican
