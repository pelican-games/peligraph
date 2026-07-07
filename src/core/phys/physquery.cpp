#include "physquery.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Pelican::phys {
namespace {

constexpr float kEpsilon = 1.0e-5f;
constexpr float kTieEpsilon = 1.0e-5f;

struct NormalizedRay {
    vec3 origin;
    vec3 direction;
    float max_distance;
    bool valid;
};

float sqr(float value) {
    return value * value;
}

float clamp(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}

vec3 add(vec3 lhs, vec3 rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

vec3 sub(vec3 lhs, vec3 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

vec3 mul(vec3 value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

vec3 neg(vec3 value) {
    return {-value.x, -value.y, -value.z};
}

float dot(vec3 lhs, vec3 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

vec3 cross(vec3 lhs, vec3 rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

float lengthSquared(vec3 value) {
    return dot(value, value);
}

float length(vec3 value) {
    return std::sqrt(lengthSquared(value));
}

vec3 normalizeOr(vec3 value, vec3 fallback) {
    const float len = length(value);
    if (len <= kEpsilon) {
        return fallback;
    }
    return mul(value, 1.0f / len);
}

float component(vec3 value, int axis) {
    if (axis == 0) {
        return value.x;
    }
    if (axis == 1) {
        return value.y;
    }
    return value.z;
}

vec3 axisVector(int axis, float sign) {
    if (axis == 0) {
        return {sign, 0.0f, 0.0f};
    }
    if (axis == 1) {
        return {0.0f, sign, 0.0f};
    }
    return {0.0f, 0.0f, sign};
}

vec3 absExtents(vec3 value) {
    return {std::abs(value.x), std::abs(value.y), std::abs(value.z)};
}

quat normalizedQuat(quat value) {
    const float len_sq = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    if (len_sq <= kEpsilon * kEpsilon) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }

    const float inv_len = 1.0f / std::sqrt(len_sq);
    return {value.x * inv_len, value.y * inv_len, value.z * inv_len, value.w * inv_len};
}

vec3 rotateVector(quat rotation, vec3 value) {
    const quat q = normalizedQuat(rotation);
    const vec3 qv{q.x, q.y, q.z};
    const vec3 t = mul(cross(qv, value), 2.0f);
    return add(add(value, mul(t, q.w)), cross(qv, t));
}

vec3 inverseRotateVector(quat rotation, vec3 value) {
    const quat q = normalizedQuat(rotation);
    return rotateVector({-q.x, -q.y, -q.z, q.w}, value);
}

std::array<vec3, 3> boxAxes(const Box &box) {
    return {
        normalizeOr(rotateVector(box.rotation, {1.0f, 0.0f, 0.0f}), {1.0f, 0.0f, 0.0f}),
        normalizeOr(rotateVector(box.rotation, {0.0f, 1.0f, 0.0f}), {0.0f, 1.0f, 0.0f}),
        normalizeOr(rotateVector(box.rotation, {0.0f, 0.0f, 1.0f}), {0.0f, 0.0f, 1.0f}),
    };
}

NormalizedRay normalizeRay(const Ray &ray) {
    const float dir_len = length(ray.direction);
    if (dir_len <= kEpsilon || ray.max_distance < 0.0f) {
        return {ray.origin, {0.0f, 0.0f, 1.0f}, ray.max_distance, false};
    }

    return {ray.origin, mul(ray.direction, 1.0f / dir_len), ray.max_distance, true};
}

bool inRayRange(float t, float max_distance) {
    return t >= -kEpsilon && t <= max_distance + kEpsilon;
}

float sanitizeRayDistance(float t) {
    return t < 0.0f && t >= -kEpsilon ? 0.0f : t;
}

std::vector<float> raySphereRoots(const NormalizedRay &ray, vec3 center, float radius) {
    const float r = std::max(0.0f, radius);
    const vec3 oc = sub(ray.origin, center);
    const float b = dot(oc, ray.direction);
    const float c = dot(oc, oc) - r * r;
    const float discriminant = b * b - c;

    if (discriminant < -kEpsilon) {
        return {};
    }

    if (std::abs(discriminant) <= kEpsilon) {
        return {-b};
    }

    const float root = std::sqrt(std::max(0.0f, discriminant));
    return {-b - root, -b + root};
}

std::optional<RaycastHit> makeSphereHit(const NormalizedRay &ray, vec3 center, float radius) {
    if (!ray.valid) {
        return std::nullopt;
    }

    float best_t = std::numeric_limits<float>::infinity();
    for (float root : raySphereRoots(ray, center, radius)) {
        if (inRayRange(root, ray.max_distance)) {
            best_t = std::min(best_t, sanitizeRayDistance(root));
        }
    }

    if (!std::isfinite(best_t)) {
        return std::nullopt;
    }

    const vec3 position = add(ray.origin, mul(ray.direction, best_t));
    const vec3 normal = normalizeOr(sub(position, center), neg(ray.direction));
    return RaycastHit{best_t, position, normal};
}

vec3 closestPointOnSegment(vec3 point, vec3 a, vec3 b) {
    const vec3 ab = sub(b, a);
    const float len_sq = lengthSquared(ab);
    if (len_sq <= kEpsilon * kEpsilon) {
        return a;
    }

    const float t = clamp(dot(sub(point, a), ab) / len_sq, 0.0f, 1.0f);
    return add(a, mul(ab, t));
}

float pointAabbDistanceSquared(vec3 point, vec3 extents) {
    const vec3 e = absExtents(extents);
    float result = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float p = component(point, axis);
        const float high = component(e, axis);
        if (p < -high) {
            result += sqr(-high - p);
        } else if (p > high) {
            result += sqr(p - high);
        }
    }
    return result;
}

float evaluateSegmentAabbDistanceSquared(vec3 p0, vec3 direction, vec3 extents, float t) {
    return pointAabbDistanceSquared(add(p0, mul(direction, t)), extents);
}

float segmentAabbDistanceSquared(vec3 p0, vec3 p1, vec3 extents) {
    const vec3 e = absExtents(extents);
    const vec3 direction = sub(p1, p0);
    std::vector<float> breaks{0.0f, 1.0f};

    for (int axis = 0; axis < 3; ++axis) {
        const float start = component(p0, axis);
        const float delta = component(direction, axis);
        const float high = component(e, axis);
        if (std::abs(delta) <= kEpsilon) {
            continue;
        }

        const float enter = (-high - start) / delta;
        const float exit = (high - start) / delta;
        if (enter > 0.0f && enter < 1.0f) {
            breaks.push_back(enter);
        }
        if (exit > 0.0f && exit < 1.0f) {
            breaks.push_back(exit);
        }
    }

    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end(), [](float lhs, float rhs) {
                     return std::abs(lhs - rhs) <= kEpsilon;
                 }),
                 breaks.end());

    float best = std::numeric_limits<float>::infinity();
    for (float t : breaks) {
        best = std::min(best, evaluateSegmentAabbDistanceSquared(p0, direction, e, t));
    }

    for (size_t i = 0; i + 1 < breaks.size(); ++i) {
        const float low = breaks[i];
        const float high = breaks[i + 1];
        if (high - low <= kEpsilon) {
            continue;
        }

        const float mid = (low + high) * 0.5f;
        float numerator = 0.0f;
        float denominator = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float p = component(p0, axis);
            const float d = component(direction, axis);
            const float limit = component(e, axis);
            const float mid_value = p + d * mid;
            if (mid_value < -limit) {
                numerator += d * (p + limit);
                denominator += d * d;
            } else if (mid_value > limit) {
                numerator += d * (p - limit);
                denominator += d * d;
            }
        }

        if (denominator <= kEpsilon) {
            best = std::min(best, evaluateSegmentAabbDistanceSquared(p0, direction, e, mid));
            continue;
        }

        const float candidate = clamp(-numerator / denominator, low, high);
        best = std::min(best, evaluateSegmentAabbDistanceSquared(p0, direction, e, candidate));
    }

    return best;
}

float segmentSegmentDistanceSquared(vec3 p1, vec3 q1, vec3 p2, vec3 q2) {
    const vec3 d1 = sub(q1, p1);
    const vec3 d2 = sub(q2, p2);
    const vec3 r = sub(p1, p2);
    const float a = lengthSquared(d1);
    const float e = lengthSquared(d2);
    const float f = dot(d2, r);

    float s = 0.0f;
    float t = 0.0f;

    if (a <= kEpsilon && e <= kEpsilon) {
        return lengthSquared(sub(p1, p2));
    }

    if (a <= kEpsilon) {
        s = 0.0f;
        t = clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = dot(d1, r);
        if (e <= kEpsilon) {
            t = 0.0f;
            s = clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dot(d1, d2);
            const float denom = a * e - b * b;
            if (std::abs(denom) > kEpsilon) {
                s = clamp((b * f - c * e) / denom, 0.0f, 1.0f);
            } else {
                s = 0.0f;
            }

            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }

    const vec3 c1 = add(p1, mul(d1, s));
    const vec3 c2 = add(p2, mul(d2, t));
    return lengthSquared(sub(c1, c2));
}

std::pair<vec3, vec3> capsuleSegment(const Capsule &capsule) {
    const float half_height = std::max(0.0f, capsule.half_height);
    const vec3 axis = normalizeOr(rotateVector(capsule.rotation, {0.0f, 1.0f, 0.0f}), {0.0f, 1.0f, 0.0f});
    const vec3 offset = mul(axis, half_height);
    return {sub(capsule.center, offset), add(capsule.center, offset)};
}

bool intervalSeparated(float min_a, float max_a, float min_b, float max_b) {
    return max_a < min_b - kEpsilon || max_b < min_a - kEpsilon;
}

} // namespace

std::optional<RaycastHit> raycast(const Ray &ray, const Sphere &sphere) {
    return makeSphereHit(normalizeRay(ray), sphere.center, sphere.radius);
}

std::optional<RaycastHit> raycast(const Ray &ray, const Box &box) {
    const NormalizedRay world_ray = normalizeRay(ray);
    if (!world_ray.valid) {
        return std::nullopt;
    }

    const vec3 extents = absExtents(box.half_extents);
    const vec3 local_origin = inverseRotateVector(box.rotation, sub(world_ray.origin, box.center));
    const vec3 local_direction = inverseRotateVector(box.rotation, world_ray.direction);

    float enter_t = -std::numeric_limits<float>::infinity();
    float exit_t = std::numeric_limits<float>::infinity();
    vec3 enter_normal{0.0f, 0.0f, 0.0f};
    vec3 exit_normal{0.0f, 0.0f, 0.0f};

    for (int axis = 0; axis < 3; ++axis) {
        const float origin = component(local_origin, axis);
        const float direction = component(local_direction, axis);
        const float extent = component(extents, axis);

        if (std::abs(direction) <= kEpsilon) {
            if (origin < -extent - kEpsilon || origin > extent + kEpsilon) {
                return std::nullopt;
            }
            continue;
        }

        float axis_enter = 0.0f;
        float axis_exit = 0.0f;
        vec3 axis_enter_normal{0.0f, 0.0f, 0.0f};
        vec3 axis_exit_normal{0.0f, 0.0f, 0.0f};

        if (direction > 0.0f) {
            axis_enter = (-extent - origin) / direction;
            axis_exit = (extent - origin) / direction;
            axis_enter_normal = axisVector(axis, -1.0f);
            axis_exit_normal = axisVector(axis, 1.0f);
        } else {
            axis_enter = (extent - origin) / direction;
            axis_exit = (-extent - origin) / direction;
            axis_enter_normal = axisVector(axis, 1.0f);
            axis_exit_normal = axisVector(axis, -1.0f);
        }

        if (axis_enter > enter_t) {
            enter_t = axis_enter;
            enter_normal = axis_enter_normal;
        }
        if (axis_exit < exit_t) {
            exit_t = axis_exit;
            exit_normal = axis_exit_normal;
        }

        if (enter_t > exit_t + kEpsilon) {
            return std::nullopt;
        }
    }

    if (exit_t < -kEpsilon) {
        return std::nullopt;
    }

    const bool from_outside = enter_t >= -kEpsilon;
    const float hit_t = sanitizeRayDistance(from_outside ? enter_t : exit_t);
    if (hit_t > world_ray.max_distance + kEpsilon) {
        return std::nullopt;
    }

    const vec3 local_normal = from_outside ? enter_normal : exit_normal;
    const vec3 position = add(world_ray.origin, mul(world_ray.direction, hit_t));
    const vec3 normal = normalizeOr(rotateVector(box.rotation, local_normal), neg(world_ray.direction));
    return RaycastHit{hit_t, position, normal};
}

std::optional<RaycastHit> raycast(const Ray &ray, const Capsule &capsule) {
    const NormalizedRay world_ray = normalizeRay(ray);
    if (!world_ray.valid) {
        return std::nullopt;
    }

    const float radius = std::max(0.0f, capsule.radius);
    const auto [a, b] = capsuleSegment(capsule);
    const vec3 segment = sub(b, a);
    const float segment_length = length(segment);
    if (segment_length <= kEpsilon) {
        return makeSphereHit(world_ray, capsule.center, radius);
    }

    const vec3 axis = mul(segment, 1.0f / segment_length);
    std::optional<RaycastHit> best;

    auto consider = [&](float t, vec3 normal) {
        if (!inRayRange(t, world_ray.max_distance)) {
            return;
        }

        const float distance = sanitizeRayDistance(t);
        if (best && distance >= best->distance - kTieEpsilon) {
            return;
        }

        const vec3 position = add(world_ray.origin, mul(world_ray.direction, distance));
        best = RaycastHit{distance, position, normalizeOr(normal, neg(world_ray.direction))};
    };

    const vec3 m = sub(world_ray.origin, a);
    const float d_dot_axis = dot(world_ray.direction, axis);
    const float m_dot_axis = dot(m, axis);
    const vec3 d_perp = sub(world_ray.direction, mul(axis, d_dot_axis));
    const vec3 m_perp = sub(m, mul(axis, m_dot_axis));
    const float qa = lengthSquared(d_perp);
    const float qb = 2.0f * dot(d_perp, m_perp);
    const float qc = lengthSquared(m_perp) - radius * radius;

    if (qa > kEpsilon) {
        const float discriminant = qb * qb - 4.0f * qa * qc;
        if (discriminant >= -kEpsilon) {
            const float root = std::sqrt(std::max(0.0f, discriminant));
            std::array<float, 2> roots{
                (-qb - root) / (2.0f * qa),
                (-qb + root) / (2.0f * qa),
            };
            std::sort(roots.begin(), roots.end());
            for (float t : roots) {
                const float y = m_dot_axis + t * d_dot_axis;
                if (y >= -kEpsilon && y <= segment_length + kEpsilon) {
                    const vec3 position = add(world_ray.origin, mul(world_ray.direction, sanitizeRayDistance(t)));
                    const vec3 axis_point = add(a, mul(axis, clamp(y, 0.0f, segment_length)));
                    consider(t, sub(position, axis_point));
                }
            }
        }
    }

    for (int cap_index = 0; cap_index < 2; ++cap_index) {
        const vec3 cap_center = cap_index == 0 ? a : b;
        for (float t : raySphereRoots(world_ray, cap_center, radius)) {
            if (!inRayRange(t, world_ray.max_distance)) {
                continue;
            }
            const vec3 position = add(world_ray.origin, mul(world_ray.direction, sanitizeRayDistance(t)));
            const float cap_side = dot(sub(position, cap_center), axis);
            if ((cap_index == 0 && cap_side > kEpsilon) || (cap_index == 1 && cap_side < -kEpsilon)) {
                continue;
            }
            consider(t, sub(position, cap_center));
        }
    }

    return best;
}

std::optional<RaycastHit> raycast(const Ray &ray, const Shape &shape) {
    return std::visit([&ray](const auto &typed_shape) { return raycast(ray, typed_shape); }, shape);
}

bool overlaps(const Sphere &lhs, const Sphere &rhs) {
    const float radius = std::max(0.0f, lhs.radius) + std::max(0.0f, rhs.radius);
    return lengthSquared(sub(lhs.center, rhs.center)) <= radius * radius + kEpsilon;
}

bool overlaps(const Sphere &lhs, const Box &rhs) {
    const vec3 local_center = inverseRotateVector(rhs.rotation, sub(lhs.center, rhs.center));
    const float radius = std::max(0.0f, lhs.radius);
    return pointAabbDistanceSquared(local_center, rhs.half_extents) <= radius * radius + kEpsilon;
}

bool overlaps(const Box &lhs, const Sphere &rhs) {
    return overlaps(rhs, lhs);
}

bool overlaps(const Sphere &lhs, const Capsule &rhs) {
    const auto [a, b] = capsuleSegment(rhs);
    const vec3 closest = closestPointOnSegment(lhs.center, a, b);
    const float radius = std::max(0.0f, lhs.radius) + std::max(0.0f, rhs.radius);
    return lengthSquared(sub(lhs.center, closest)) <= radius * radius + kEpsilon;
}

bool overlaps(const Capsule &lhs, const Sphere &rhs) {
    return overlaps(rhs, lhs);
}

bool overlaps(const Box &lhs, const Box &rhs) {
    const std::array<vec3, 3> axes_a = boxAxes(lhs);
    const std::array<vec3, 3> axes_b = boxAxes(rhs);
    const vec3 extents_a = absExtents(lhs.half_extents);
    const vec3 extents_b = absExtents(rhs.half_extents);

    float rotation[3][3]{};
    float abs_rotation[3][3]{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = dot(axes_a[i], axes_b[j]);
            abs_rotation[i][j] = std::abs(rotation[i][j]) + kEpsilon;
        }
    }

    const vec3 center_delta = sub(rhs.center, lhs.center);
    float translation[3]{
        dot(center_delta, axes_a[0]),
        dot(center_delta, axes_a[1]),
        dot(center_delta, axes_a[2]),
    };

    for (int i = 0; i < 3; ++i) {
        const float ra = component(extents_a, i);
        const float rb = component(extents_b, 0) * abs_rotation[i][0] + component(extents_b, 1) * abs_rotation[i][1] +
                         component(extents_b, 2) * abs_rotation[i][2];
        if (std::abs(translation[i]) > ra + rb + kEpsilon) {
            return false;
        }
    }

    for (int j = 0; j < 3; ++j) {
        const float ra = component(extents_a, 0) * abs_rotation[0][j] + component(extents_a, 1) * abs_rotation[1][j] +
                         component(extents_a, 2) * abs_rotation[2][j];
        const float rb = component(extents_b, j);
        const float distance = std::abs(translation[0] * rotation[0][j] + translation[1] * rotation[1][j] +
                                        translation[2] * rotation[2][j]);
        if (distance > ra + rb + kEpsilon) {
            return false;
        }
    }

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int i1 = (i + 1) % 3;
            const int i2 = (i + 2) % 3;
            const int j1 = (j + 1) % 3;
            const int j2 = (j + 2) % 3;
            const float ra = component(extents_a, i1) * abs_rotation[i2][j] + component(extents_a, i2) * abs_rotation[i1][j];
            const float rb = component(extents_b, j1) * abs_rotation[i][j2] + component(extents_b, j2) * abs_rotation[i][j1];
            const float distance = std::abs(translation[i2] * rotation[i1][j] - translation[i1] * rotation[i2][j]);
            if (distance > ra + rb + kEpsilon) {
                return false;
            }
        }
    }

    return true;
}

bool overlaps(const Capsule &lhs, const Capsule &rhs) {
    const auto [lhs_a, lhs_b] = capsuleSegment(lhs);
    const auto [rhs_a, rhs_b] = capsuleSegment(rhs);
    const float radius = std::max(0.0f, lhs.radius) + std::max(0.0f, rhs.radius);
    return segmentSegmentDistanceSquared(lhs_a, lhs_b, rhs_a, rhs_b) <= radius * radius + kEpsilon;
}

bool overlaps(const Box &lhs, const Capsule &rhs) {
    const auto [a, b] = capsuleSegment(rhs);
    const vec3 local_a = inverseRotateVector(lhs.rotation, sub(a, lhs.center));
    const vec3 local_b = inverseRotateVector(lhs.rotation, sub(b, lhs.center));
    const float radius = std::max(0.0f, rhs.radius);
    return segmentAabbDistanceSquared(local_a, local_b, lhs.half_extents) <= radius * radius + kEpsilon;
}

bool overlaps(const Capsule &lhs, const Box &rhs) {
    return overlaps(rhs, lhs);
}

bool overlaps(const Shape &lhs, const Shape &rhs) {
    return std::visit([](const auto &typed_lhs, const auto &typed_rhs) { return overlaps(typed_lhs, typed_rhs); }, lhs, rhs);
}

std::optional<ObjectRaycastHit> raycastClosest(const Ray &ray, std::span<const Collider> colliders) {
    std::optional<ObjectRaycastHit> best;

    for (const Collider &collider : colliders) {
        const std::optional<RaycastHit> hit = raycast(ray, collider.shape);
        if (!hit) {
            continue;
        }

        const bool is_better_distance = !best || hit->distance < best->distance - kTieEpsilon;
        const bool is_tie_better_id = best && std::abs(hit->distance - best->distance) <= kTieEpsilon && collider.id < best->id;
        if (is_better_distance || is_tie_better_id) {
            best = ObjectRaycastHit{collider.id, hit->distance, hit->position, hit->normal};
        }
    }

    return best;
}

std::vector<std::string> overlapAll(const Shape &shape, std::span<const Collider> colliders) {
    std::vector<std::string> ids;
    for (const Collider &collider : colliders) {
        if (overlaps(shape, collider.shape)) {
            ids.push_back(collider.id);
        }
    }

    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace Pelican::phys
