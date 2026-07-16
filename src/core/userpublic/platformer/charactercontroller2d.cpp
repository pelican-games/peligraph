#include "charactercontroller2d.hpp"

#include "../gamecontext.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace Pelican::platformer {
namespace {

constexpr float pi = 3.14159265358979323846F;

bool finite(float value) {
    return std::isfinite(value);
}

bool finite(vec3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(quat value) {
    return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}

float dot(vec3 lhs, vec3 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

vec3 add(vec3 lhs, vec3 rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

vec3 subtract(vec3 lhs, vec3 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

vec3 multiply(vec3 value, float scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float lengthSquared(vec3 value) {
    return dot(value, value);
}

float length(vec3 value) {
    return std::sqrt(lengthSquared(value));
}

vec3 cross(vec3 lhs, vec3 rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

quat normalizedOrIdentity(quat value) {
    const float length_squared = value.x * value.x + value.y * value.y +
                                 value.z * value.z + value.w * value.w;
    if (!finite(value) || length_squared <= std::numeric_limits<float>::epsilon()) {
        return {0.0F, 0.0F, 0.0F, 1.0F};
    }
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    return {
        value.x * inverse_length,
        value.y * inverse_length,
        value.z * inverse_length,
        value.w * inverse_length,
    };
}

vec3 rotate(quat rotation, vec3 value) {
    rotation = normalizedOrIdentity(rotation);
    const vec3 imaginary{rotation.x, rotation.y, rotation.z};
    const vec3 doubled_cross = multiply(cross(imaginary, value), 2.0F);
    return add(value, add(multiply(doubled_cross, rotation.w),
                          cross(imaginary, doubled_cross)));
}

vec3 shapeCenter(const phys::Shape &shape) {
    return std::visit([](const auto &value) { return value.center; }, shape);
}

void translateShape(phys::Shape &shape, vec3 translation) {
    std::visit([translation](auto &value) { value.center = add(value.center, translation); }, shape);
}

float projectedRadius(const phys::Shape &shape, vec3 normal) {
    return std::visit(
        [normal](const auto &value) -> float {
            using ShapeType = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<ShapeType, phys::Sphere>) {
                return value.radius;
            } else if constexpr (std::is_same_v<ShapeType, phys::Box>) {
                const vec3 axis_x = rotate(value.rotation, {1.0F, 0.0F, 0.0F});
                const vec3 axis_y = rotate(value.rotation, {0.0F, 1.0F, 0.0F});
                const vec3 axis_z = rotate(value.rotation, {0.0F, 0.0F, 1.0F});
                return std::abs(dot(normal, axis_x)) * value.half_extents.x +
                       std::abs(dot(normal, axis_y)) * value.half_extents.y +
                       std::abs(dot(normal, axis_z)) * value.half_extents.z;
            } else {
                const vec3 axis = rotate(value.rotation, {0.0F, 1.0F, 0.0F});
                return std::abs(dot(normal, axis)) * value.half_height + value.radius;
            }
        },
        shape);
}

void validateShape(const phys::Shape &shape) {
    std::visit(
        [](const auto &value) {
            using ShapeType = std::remove_cvref_t<decltype(value)>;
            if (!finite(value.center)) {
                throw std::invalid_argument("moveAndSlide shape center must be finite");
            }
            if constexpr (std::is_same_v<ShapeType, phys::Sphere>) {
                if (!finite(value.radius) || value.radius < 0.0F) {
                    throw std::invalid_argument("moveAndSlide sphere radius is invalid");
                }
            } else if constexpr (std::is_same_v<ShapeType, phys::Box>) {
                if (!finite(value.rotation) || !finite(value.half_extents) ||
                    value.half_extents.x < 0.0F || value.half_extents.y < 0.0F ||
                    value.half_extents.z < 0.0F) {
                    throw std::invalid_argument("moveAndSlide box shape is invalid");
                }
            } else {
                if (!finite(value.rotation) || !finite(value.half_height) ||
                    !finite(value.radius) || value.half_height < 0.0F ||
                    value.radius < 0.0F) {
                    throw std::invalid_argument("moveAndSlide capsule shape is invalid");
                }
            }
        },
        shape);
}

void validateSettings(const MoveAndSlide2DSettings &settings) {
    if (!finite(settings.max_slope_degrees) || settings.max_slope_degrees < 0.0F ||
        settings.max_slope_degrees >= 90.0F) {
        throw std::invalid_argument("moveAndSlide max_slope_degrees must be in [0, 90)");
    }
    if (!finite(settings.skin_width) || settings.skin_width < 0.0F) {
        throw std::invalid_argument("moveAndSlide skin_width must be finite and non-negative");
    }
    if (!finite(settings.motion_epsilon) || settings.motion_epsilon < 0.0F) {
        throw std::invalid_argument("moveAndSlide motion_epsilon must be finite and non-negative");
    }
    if (!finite(settings.one_way_tolerance) || settings.one_way_tolerance < 0.0F) {
        throw std::invalid_argument("moveAndSlide one_way_tolerance must be finite and non-negative");
    }
    if (settings.max_iterations == 0 || settings.max_iterations > 32) {
        throw std::invalid_argument("moveAndSlide max_iterations must be in [1, 32]");
    }
}

std::optional<vec3> planarNormal(vec3 normal, float epsilon) {
    if (!finite(normal)) {
        throw std::runtime_error("moveAndSlide query returned a non-finite normal");
    }
    normal.z = 0.0F;
    const float planar_length = length(normal);
    if (planar_length <= epsilon) {
        return std::nullopt;
    }
    return multiply(normal, 1.0F / planar_length);
}

void validateHit(const phys::ShapeCastQueryHit &hit) {
    if (!finite(hit.time_of_impact) || hit.time_of_impact < -phys::shapeCastTieEpsilon ||
        hit.time_of_impact > 1.0F + phys::shapeCastTieEpsilon ||
        !finite(hit.penetration_depth) || hit.penetration_depth < 0.0F ||
        !finite(hit.position) || !finite(hit.normal)) {
        throw std::runtime_error("moveAndSlide query returned an invalid hit");
    }
}

ContactKind2D classifyContact(vec3 normal, float floor_normal_y) {
    if (normal.y >= floor_normal_y) return ContactKind2D::ground;
    if (normal.y < 0.0F) return ContactKind2D::ceiling;
    return ContactKind2D::wall;
}

bool acceptsOneWay(const phys::Shape &shape, vec3 delta,
                   const phys::ShapeCastQueryHit &hit, vec3 normal,
                   float floor_normal_y, const MoveAndSlide2DSettings &settings) {
    if (!hit.metadata.one_way) return true;
    if (!settings.collide_with_one_way || normal.y < floor_normal_y) return false;
    if (dot(delta, normal) >= -settings.motion_epsilon) return false;

    const float starting_support =
        dot(shapeCenter(shape), normal) - projectedRadius(shape, normal);
    const float surface_plane = dot(hit.position, normal);
    return starting_support + settings.one_way_tolerance >= surface_plane;
}

struct SelectedHit {
    phys::ShapeCastQueryHit hit;
    vec3 normal;
    ContactKind2D kind = ContactKind2D::wall;
};

std::optional<SelectedHit> selectBlockingHit(
    const phys::Shape &shape, vec3 delta,
    const std::vector<phys::ShapeCastQueryHit> &hits,
    float floor_normal_y, const MoveAndSlide2DSettings &settings) {
    for (const auto &hit : hits) {
        validateHit(hit);
        if (hit.metadata.trigger && !settings.collide_with_triggers) continue;
        const auto normal = planarNormal(hit.normal, settings.motion_epsilon);
        if (!normal) continue;
        if (!acceptsOneWay(shape, delta, hit, *normal, floor_normal_y, settings)) continue;
        return SelectedHit{hit, *normal, classifyContact(*normal, floor_normal_y)};
    }
    return std::nullopt;
}

vec3 removeInwardComponent(vec3 value, vec3 normal) {
    const float inward = dot(value, normal);
    if (inward < 0.0F) return subtract(value, multiply(normal, inward));
    return value;
}

vec3 slideRemainder(vec3 value, const SelectedHit &contact,
                    float motion_epsilon) {
    // Treat a non-walkable upward slope as a wall. Projecting onto its full
    // tangent would manufacture upward velocity and let the body climb slopes
    // above max_slope_degrees.
    if (contact.kind == ContactKind2D::wall && contact.normal.y > 0.0F) {
        vec3 wall_normal{contact.normal.x, 0.0F, 0.0F};
        const float wall_length = length(wall_normal);
        if (wall_length > motion_epsilon) {
            wall_normal = multiply(wall_normal, 1.0F / wall_length);
            return removeInwardComponent(value, wall_normal);
        }
    }
    return removeInwardComponent(value, contact.normal);
}

void recordContact(MoveAndSlide2DResult &result, const SelectedHit &selected) {
    result.contacts.push_back(CharacterContact2D{selected.hit, selected.normal, selected.kind});
    switch (selected.kind) {
    case ContactKind2D::ground:
        result.grounded = true;
        break;
    case ContactKind2D::wall:
        result.hit_wall = true;
        break;
    case ContactKind2D::ceiling:
        result.hit_ceiling = true;
        break;
    }
}

} // namespace

MoveAndSlide2DResult moveAndSlide(
    const phys::Shape &moving_shape, vec3 delta,
    const ShapeCastAll2DQuery &query, const phys::QueryFilter &filter,
    const MoveAndSlide2DSettings &settings) {
    validateShape(moving_shape);
    validateSettings(settings);
    if (!query) throw std::invalid_argument("moveAndSlide query must be callable");
    if (!finite(delta)) throw std::invalid_argument("moveAndSlide delta must be finite");
    if (std::abs(delta.z) > settings.motion_epsilon) {
        throw std::invalid_argument("moveAndSlide2D delta.z must be zero");
    }
    delta.z = 0.0F;

    MoveAndSlide2DResult result;
    result.shape = moving_shape;
    result.requested_delta = delta;
    result.remaining_delta = delta;
    result.contacts.reserve(settings.max_iterations);

    const float floor_normal_y =
        std::cos(settings.max_slope_degrees * pi / 180.0F);
    phys::QueryFilter movement_filter = filter;
    movement_filter.include_triggers =
        movement_filter.include_triggers && settings.collide_with_triggers;
    movement_filter.include_one_way =
        movement_filter.include_one_way && settings.collide_with_one_way;

    for (std::uint32_t iteration = 0; iteration < settings.max_iterations; ++iteration) {
        const auto hits = query(result.shape, result.remaining_delta, movement_filter);
        const auto selected = selectBlockingHit(result.shape, result.remaining_delta, hits,
                                                floor_normal_y, settings);
        if (!selected) {
            translateShape(result.shape, result.remaining_delta);
            result.applied_translation = add(result.applied_translation, result.remaining_delta);
            result.remaining_delta = {};
            return result;
        }

        recordContact(result, *selected);
        if (selected->hit.initial_overlap) {
            const float recovery_distance =
                selected->hit.penetration_depth + settings.skin_width;
            const vec3 recovery = multiply(selected->normal, recovery_distance);
            translateShape(result.shape, recovery);
            result.applied_translation = add(result.applied_translation, recovery);
            result.remaining_delta = slideRemainder(result.remaining_delta, *selected,
                                                    settings.motion_epsilon);
            continue;
        }

        const float motion_length = length(result.remaining_delta);
        if (motion_length <= settings.motion_epsilon) {
            result.remaining_delta = {};
            return result;
        }

        const float time_of_impact =
            std::clamp(selected->hit.time_of_impact, 0.0F, 1.0F);
        const float contact_distance = motion_length * time_of_impact;
        const float travel_distance =
            std::max(0.0F, contact_distance - settings.skin_width);
        const vec3 travel = multiply(result.remaining_delta,
                                     travel_distance / motion_length);
        translateShape(result.shape, travel);
        result.applied_translation = add(result.applied_translation, travel);

        vec3 remainder = multiply(result.remaining_delta, 1.0F - time_of_impact);
        result.remaining_delta = slideRemainder(remainder, *selected,
                                                settings.motion_epsilon);
        result.remaining_delta.z = 0.0F;
        if (length(result.remaining_delta) <= settings.motion_epsilon) {
            result.remaining_delta = {};
            return result;
        }
    }

    result.iteration_limit_reached = length(result.remaining_delta) > settings.motion_epsilon;
    return result;
}

MoveAndSlide2DResult moveAndSlide(
    GameContext &context, const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter, const MoveAndSlide2DSettings &settings) {
    const ShapeCastAll2DQuery query = [&context](const phys::Shape &shape,
                                                 vec3 movement,
                                                 const phys::QueryFilter &query_filter) {
        return context.shapeCastAll(shape, movement, query_filter);
    };
    return moveAndSlide(moving_shape, delta, query, filter, settings);
}

} // namespace Pelican::platformer
