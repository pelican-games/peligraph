#include "collider.hpp"

#include "../../loader/componentcodec.hpp"
#include "../../loader/resolutionresult.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

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

// One acceptance implementation. The collider codec is the authority on what a
// scene document may say, and this entry point -- the ECS registerer's JSON
// loader, reached from ComponentInfoManager::loadByJson -- decodes through it
// rather than keeping a second parser that can drift. The two live in the same
// library, and this file already depended on the loader for legacyReject.
void ColliderComponent::loadFromJsonArchive(const JsonArchiveLoader &archive) {
    const auto &json = *static_cast<const nlohmann::json *>(archive.ptr);
    const auto &codec = requireComponentCodec("collider");
    auto decoded = codec.decodeAuthored(json);
    if (!decoded) throw ResolutionFailureException{std::move(decoded).error()};
    codec.applyRuntime(decoded.value(), this);
}

void ColliderComponent::validate() const {
    if (!finite(pos) || !finite(rotation)) {
        legacyReject("collider.pose_non_finite",
                     "Collider pose must contain finite values");
    }
    if (shape == "sphere") {
        if (!std::isfinite(radius) || radius <= 0.0f) {
            legacyReject("collider.radius_non_positive",
                         "Sphere collider radius must be positive");
        }
    } else if (shape == "box") {
        if (!allPositive(half_extents)) {
            legacyReject("collider.half_extents_non_positive",
                         "Box collider half_extents must be positive");
        }
    } else if (shape == "capsule") {
        if (!std::isfinite(radius) || radius <= 0.0f) {
            legacyReject("collider.capsule_radius_non_positive",
                         "Capsule collider radius must be positive");
        }
        if (!std::isfinite(half_height) || half_height < 0.0f) {
            legacyReject("collider.half_height_negative",
                         "Capsule collider half_height must be non-negative");
        }
    } else {
        legacyReject("collider.shape_unknown",
                     "Unknown collider shape: " + shape);
    }
}

void ColliderComponent::init() {
    validate();
}

void ColliderComponent::deinit() noexcept {}

}  // namespace Pelican
