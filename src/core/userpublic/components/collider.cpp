#include "collider.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
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

std::uint32_t readUint32Or(const nlohmann::json &json, const char *name,
                           std::uint32_t fallback) {
    const auto found = json.find(name);
    if (found == json.end()) return fallback;
    if (!found->is_number_integer() && !found->is_number_unsigned()) {
        throw std::runtime_error(std::string{"Collider field must be uint32: "} + name);
    }
    if (found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(std::string{"Collider field is outside uint32 range: "} +
                                     name);
        }
        return static_cast<std::uint32_t>(value);
    }
    const auto value = found->get<std::int64_t>();
    if (value < 0 ||
        static_cast<std::uint64_t>(value) >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string{"Collider field is outside uint32 range: "} +
                                 name);
    }
    return static_cast<std::uint32_t>(value);
}

bool readBoolOr(const nlohmann::json &json, const char *name, bool fallback) {
    const auto found = json.find(name);
    if (found == json.end()) return fallback;
    if (!found->is_boolean()) {
        throw std::runtime_error(std::string{"Collider field must be boolean: "} + name);
    }
    return found->get<bool>();
}

bool allPositive(vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && value.x > 0.0f && value.y > 0.0f &&
           value.z > 0.0f;
}

bool finite(vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(quat value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

} // namespace

void ColliderComponent::loadFromJsonArchive(const JsonArchiveLoader &archive) {
    const auto &json = *static_cast<const nlohmann::json *>(archive.ptr);
    shape = json.at("shape").get<std::string>();
    pos = readVec3Or(json, "pos", {0.0f, 0.0f, 0.0f});
    rotation = readQuatOr(json, "rotation", {0.0f, 0.0f, 0.0f, 1.0f});
    layer = readUint32Or(json, "layer", 1U);
    mask = readUint32Or(json, "mask", ~std::uint32_t{0});
    trigger = readBoolOr(json, "trigger", false);
    one_way = readBoolOr(json, "one_way", false);

    if (shape == "sphere") {
        radius = readRequiredFloat(json, "radius");
    } else if (shape == "box") {
        if (json.contains("size")) {
            throw std::runtime_error(
                "Box collider field 'size' is not supported in v1; use 'half_extents' (size divided by 2)");
        }
        half_extents = readRequiredVec3(json, "half_extents");
    } else if (shape == "capsule") {
        if (json.contains("height")) {
            throw std::runtime_error(
                "Capsule collider field 'height' is not supported in v1; use 'half_height' (height divided by 2)");
        }
        radius = readRequiredFloat(json, "radius");
        half_height = readRequiredFloat(json, "half_height");
    } else {
        throw std::runtime_error("Unknown collider shape: " + shape);
    }

    validate();
}

void ColliderComponent::validate() const {
    if (!finite(pos) || !finite(rotation)) {
        throw std::runtime_error("Collider pose must contain finite values");
    }
    if (shape == "sphere") {
        if (!std::isfinite(radius) || radius <= 0.0f) {
            throw std::runtime_error("Sphere collider radius must be positive");
        }
    } else if (shape == "box") {
        if (!allPositive(half_extents)) {
            throw std::runtime_error("Box collider half_extents must be positive");
        }
    } else if (shape == "capsule") {
        if (!std::isfinite(radius) || radius <= 0.0f) {
            throw std::runtime_error("Capsule collider radius must be positive");
        }
        if (!std::isfinite(half_height) || half_height < 0.0f) {
            throw std::runtime_error("Capsule collider half_height must be non-negative");
        }
    } else {
        throw std::runtime_error("Unknown collider shape: " + shape);
    }
}

void ColliderComponent::init() {
    validate();
}

void ColliderComponent::deinit() noexcept {}

}  // namespace Pelican
