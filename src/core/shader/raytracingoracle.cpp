#include "raytracingoracle.hpp"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#if !defined(_MSC_VER)
#pragma STDC FP_CONTRACT OFF
#endif

namespace Pelican {
namespace {

using Vec3 = RayTracingOracleVec3;
using Interval = RayTracingOracleInterval;
namespace Contract = RayTracingOracleContract;

double quantize(double value) {
    return static_cast<double>(static_cast<float>(value));
}

Vec3 quantize(Vec3 value) {
    return {quantize(value.x), quantize(value.y), quantize(value.z)};
}

RayTracingOracleTransform quantize(
    RayTracingOracleTransform value) {
    for (auto &component : value.object_to_world) {
        component = quantize(component);
    }
    return value;
}

double nextDown(double value) {
    return std::nextafter(value,
                          -std::numeric_limits<double>::infinity());
}

double nextUp(double value) {
    return std::nextafter(value,
                          std::numeric_limits<double>::infinity());
}

Interval exact(double value) { return {value, value}; }

Interval add(Interval left, Interval right) {
    return {nextDown(left.lower + right.lower),
            nextUp(left.upper + right.upper)};
}

Interval subtract(Interval left, Interval right) {
    return {nextDown(left.lower - right.upper),
            nextUp(left.upper - right.lower)};
}

Interval multiply(Interval left, Interval right) {
    const std::array products{
        left.lower * right.lower, left.lower * right.upper,
        left.upper * right.lower, left.upper * right.upper};
    const auto [minimum, maximum] =
        std::minmax_element(products.begin(), products.end());
    return {nextDown(*minimum), nextUp(*maximum)};
}

Interval divide(Interval numerator, Interval denominator) {
    if (denominator.lower <= 0.0 && denominator.upper >= 0.0) {
        throw std::runtime_error(
            "ray tracing oracle determinant interval contains zero");
    }
    const Interval reciprocal{
        nextDown(1.0 / denominator.upper),
        nextUp(1.0 / denominator.lower)};
    return multiply(numerator, reciprocal);
}

struct IntervalVec3 {
    Interval x;
    Interval y;
    Interval z;
};

IntervalVec3 exact(Vec3 value) {
    return {exact(value.x), exact(value.y), exact(value.z)};
}

IntervalVec3 subtract(IntervalVec3 left, IntervalVec3 right) {
    return {subtract(left.x, right.x),
            subtract(left.y, right.y),
            subtract(left.z, right.z)};
}

IntervalVec3 cross(IntervalVec3 left, IntervalVec3 right) {
    return {
        subtract(multiply(left.y, right.z),
                 multiply(left.z, right.y)),
        subtract(multiply(left.z, right.x),
                 multiply(left.x, right.z)),
        subtract(multiply(left.x, right.y),
                 multiply(left.y, right.x)),
    };
}

Interval dot(IntervalVec3 left, IntervalVec3 right) {
    return add(add(multiply(left.x, right.x),
                   multiply(left.y, right.y)),
               multiply(left.z, right.z));
}

Vec3 subtract(Vec3 left, Vec3 right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 cross(Vec3 left, Vec3 right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

double dot(Vec3 left, Vec3 right) {
    return (left.x * right.x + left.y * right.y) +
           left.z * right.z;
}

double length(Vec3 value) { return std::sqrt(dot(value, value)); }

Vec3 transformPoint(const RayTracingOracleTransform &transform,
                    Vec3 point) {
    const auto &m = transform.object_to_world;
    return {
        ((m[0] * point.x + m[1] * point.y) + m[2] * point.z) + m[3],
        ((m[4] * point.x + m[5] * point.y) + m[6] * point.z) + m[7],
        ((m[8] * point.x + m[9] * point.y) + m[10] * point.z) + m[11],
    };
}

double determinant(const RayTracingOracleTransform &transform) {
    const auto &m = transform.object_to_world;
    return m[0] * (m[5] * m[10] - m[6] * m[9]) -
           m[1] * (m[4] * m[10] - m[6] * m[8]) +
           m[2] * (m[4] * m[9] - m[5] * m[8]);
}

double conditionInfinity(const RayTracingOracleTransform &transform) {
    const auto &m = transform.object_to_world;
    const auto det = determinant(transform);
    const auto norm = std::max({
        std::abs(m[0]) + std::abs(m[1]) + std::abs(m[2]),
        std::abs(m[4]) + std::abs(m[5]) + std::abs(m[6]),
        std::abs(m[8]) + std::abs(m[9]) + std::abs(m[10]),
    });
    const std::array inverse{
        (m[5] * m[10] - m[6] * m[9]) / det,
        (m[2] * m[9] - m[1] * m[10]) / det,
        (m[1] * m[6] - m[2] * m[5]) / det,
        (m[6] * m[8] - m[4] * m[10]) / det,
        (m[0] * m[10] - m[2] * m[8]) / det,
        (m[2] * m[4] - m[0] * m[6]) / det,
        (m[4] * m[9] - m[5] * m[8]) / det,
        (m[1] * m[8] - m[0] * m[9]) / det,
        (m[0] * m[5] - m[1] * m[4]) / det,
    };
    const auto inverse_norm = std::max({
        std::abs(inverse[0]) + std::abs(inverse[1]) +
            std::abs(inverse[2]),
        std::abs(inverse[3]) + std::abs(inverse[4]) +
            std::abs(inverse[5]),
        std::abs(inverse[6]) + std::abs(inverse[7]) +
            std::abs(inverse[8]),
    });
    return norm * inverse_norm;
}

bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(
            "ray tracing oracle fixture rejected: " +
            std::move(message));
    }
}

float roundDownToF32(double value) {
    auto rounded = static_cast<float>(value);
    if (static_cast<double>(rounded) > value) {
        rounded = std::nextafter(
            rounded, -std::numeric_limits<float>::infinity());
    }
    return rounded;
}

float roundUpToF32(double value) {
    auto rounded = static_cast<float>(value);
    if (static_cast<double>(rounded) < value) {
        rounded = std::nextafter(
            rounded, std::numeric_limits<float>::infinity());
    }
    return rounded;
}

Interval padEndpoints(Interval interval) {
    auto lower = roundDownToF32(interval.lower);
    auto upper = roundUpToF32(interval.upper);
    for (int index = 0;
         index < Contract::endpoint_f32_ulp_padding; ++index) {
        lower = std::nextafter(
            lower, -std::numeric_limits<float>::infinity());
        upper = std::nextafter(
            upper, std::numeric_limits<float>::infinity());
    }
    return {static_cast<double>(lower),
            static_cast<double>(upper)};
}

RayTracingOracleIntersection intersect(
    std::uint32_t record_id, const RayTracingOracleRay &ray,
    const std::array<Vec3, 3> &vertices) {
    // Normative Moller-Trumbore sequence. Keep each expression separate.
    const auto e1 = subtract(vertices[1], vertices[0]);
    const auto e2 = subtract(vertices[2], vertices[0]);
    const auto p = cross(ray.direction, e2);
    const auto det = dot(e1, p);
    const auto inv = 1.0 / det;
    const auto tvec = subtract(ray.origin, vertices[0]);
    const auto u = dot(tvec, p) * inv;
    const auto q = cross(tvec, e1);
    const auto v = dot(ray.direction, q) * inv;
    const auto t = dot(e2, q) * inv;

    const auto ie1 = subtract(exact(vertices[1]), exact(vertices[0]));
    const auto ie2 = subtract(exact(vertices[2]), exact(vertices[0]));
    const auto ip = cross(exact(ray.direction), ie2);
    const auto idet = dot(ie1, ip);
    const auto iinv = divide(exact(1.0), idet);
    const auto itvec = subtract(exact(ray.origin), exact(vertices[0]));
    const auto iu = multiply(dot(itvec, ip), iinv);
    const auto iq = cross(itvec, ie1);
    const auto iv = multiply(dot(exact(ray.direction), iq), iinv);
    const auto it = multiply(dot(ie2, iq), iinv);
    (void)iu;
    (void)iv;

    return {
        .record_id = record_id,
        .u = u,
        .v = v,
        .t = t,
        .t_interval = padEndpoints(it),
        .inside = u >= 0.0 && v >= 0.0 && u + v <= 1.0 &&
                  t >= 0.0,
    };
}

} // namespace

void validateRayTracingOracleFixtureWithIntersectorForTesting(
    const RayTracingOracleRay &authored_ray,
    std::span<const RayTracingOracleTriangle> authored_triangles,
    const RayTracingOracleIntersectorForTesting &intersector) {
    require(static_cast<bool>(intersector),
            "test intersector is empty");
    require(!authored_triangles.empty(), "scene has no triangles");
    const RayTracingOracleRay ray{
        quantize(authored_ray.origin),
        quantize(authored_ray.direction),
    };
    require(finite(ray.origin) && finite(ray.direction),
            "ray contains NaN or infinity");
    require(std::abs(length(ray.direction) - 1.0) <= 1.0e-6,
            "ray direction is not normalized after f32 quantization");

    std::vector<RayTracingOracleIntersection> hits;
    for (const auto &authored : authored_triangles) {
        const auto transform = quantize(authored.transform);
        for (const auto component : transform.object_to_world) {
            require(std::isfinite(component),
                    "object-to-world transform is not finite");
        }
        const auto transform_det = determinant(transform);
        require(std::abs(transform_det) >=
                    Contract::minimum_transform_determinant,
                "object-to-world |det| is below 1e-3");
        require(conditionInfinity(transform) <=
                    Contract::maximum_transform_condition,
                "object-to-world infinity-norm condition exceeds 100");

        std::array<Vec3, 3> world;
        for (std::size_t index = 0; index < world.size(); ++index) {
            world[index] = transformPoint(
                transform, quantize(authored.vertices[index]));
            require(finite(world[index]),
                    "world-space vertex is not finite");
            require(std::abs(world[index].x) <= Contract::coordinate_limit &&
                        std::abs(world[index].y) <= Contract::coordinate_limit &&
                        std::abs(world[index].z) <= Contract::coordinate_limit,
                    "world-space coordinate exceeds +/-2^10");
        }

        const auto e1 = subtract(world[1], world[0]);
        const auto e2 = subtract(world[2], world[0]);
        const auto normal = cross(e1, e2);
        const auto twice_area = length(normal);
        require(twice_area * 0.5 >=
                    Contract::minimum_triangle_area,
                "world-space triangle area is below 1e-4");
        const std::array edges{
            subtract(world[1], world[0]),
            subtract(world[2], world[1]),
            subtract(world[0], world[2]),
        };
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            const auto left = edges[vertex];
            const auto right = RayTracingOracleVec3{
                -edges[(vertex + 2) % 3].x,
                -edges[(vertex + 2) % 3].y,
                -edges[(vertex + 2) % 3].z,
            };
            const auto cosine = std::clamp(
                dot(left, right) / (length(left) * length(right)),
                -1.0, 1.0);
            const auto angle = std::acos(cosine) *
                               (180.0 / std::acos(-1.0));
            require(angle >= Contract::minimum_triangle_angle_degrees,
                    "world-space triangle angle is below 5 degrees");
        }
        const auto incidence =
            std::abs(dot(ray.direction, normal) / twice_area);
        require(incidence >= Contract::minimum_incidence_dot,
                "ray-plane incidence is below the 5 degree bound");

        const auto candidate = intersector(
            authored.record_id, ray, world);
        const auto w = 1.0 - candidate.u - candidate.v;
        const auto boundary_distance = std::min(
            {std::abs(candidate.u), std::abs(candidate.v),
             std::abs(w)});
        require(boundary_distance >= Contract::barycentric_epsilon,
                "intersection is within epsilon_b of an edge or vertex");
        if (!candidate.inside) continue;
        require(candidate.t >= Contract::t_min &&
                    candidate.t <= Contract::t_max,
                "hit t is outside [1e-3, 2^12]");
        const auto midpoint =
            (candidate.t_interval.lower + candidate.t_interval.upper) * 0.5;
        const auto denominator = std::max(1.0, std::abs(midpoint));
        require((candidate.t_interval.upper -
                 candidate.t_interval.lower) /
                    denominator <=
                    Contract::maximum_relative_interval_width,
                "relative t interval width exceeds 1e-2");
        hits.push_back(candidate);
    }
    std::ranges::sort(
        hits, {}, &RayTracingOracleIntersection::t);
    if (hits.size() >= 2) {
        const auto midpoint =
            (hits[0].t_interval.lower +
             hits[0].t_interval.upper) * 0.5;
        const auto epsilon = Contract::relative_t_epsilon *
                             std::max(1.0, std::abs(midpoint));
        require(std::abs(hits[1].t - hits[0].t) > epsilon,
                "nearest-hit uniqueness margin is not constructive");
    }
}

void validateRayTracingOracleFixture(
    const RayTracingOracleRay &authored_ray,
    std::span<const RayTracingOracleTriangle> authored_triangles) {
    validateRayTracingOracleFixtureWithIntersectorForTesting(
        authored_ray, authored_triangles, intersect);
}

RayTracingOracleResult traceRayTracingOracleWithIntersectorForTesting(
    const RayTracingOracleRay &authored_ray,
    std::span<const RayTracingOracleTriangle> authored_triangles,
    const RayTracingOracleIntersectorForTesting &intersector) {
    validateRayTracingOracleFixtureWithIntersectorForTesting(
        authored_ray, authored_triangles, intersector);
    const RayTracingOracleRay ray{
        quantize(authored_ray.origin),
        quantize(authored_ray.direction),
    };
    std::optional<RayTracingOracleIntersection> nearest;
    for (const auto &authored : authored_triangles) {
        const auto transform = quantize(authored.transform);
        std::array<Vec3, 3> world;
        for (std::size_t index = 0; index < world.size(); ++index) {
            world[index] = transformPoint(
                transform, quantize(authored.vertices[index]));
        }
        const auto candidate = intersector(
            authored.record_id, ray, world);
        if (candidate.inside &&
            (!nearest || candidate.t < nearest->t)) {
            nearest = candidate;
        }
    }
    if (!nearest) return {};
    return RayTracingOracleResult{
        .hit = RayTracingOracleHit{
            .record_id = nearest->record_id,
            .t_mid = (nearest->t_interval.lower +
                      nearest->t_interval.upper) * 0.5,
            .barycentric = {
                1.0 - nearest->u - nearest->v,
                nearest->u,
                nearest->v,
            },
            .t_interval = nearest->t_interval,
        },
    };
}

RayTracingOracleResult traceRayTracingOracle(
    const RayTracingOracleRay &authored_ray,
    std::span<const RayTracingOracleTriangle> authored_triangles) {
    return traceRayTracingOracleWithIntersectorForTesting(
        authored_ray, authored_triangles, intersect);
}

RayTracingOracleInterval expandGpuRayT(float value) {
    auto lower = value;
    auto upper = value;
    for (int index = 0;
         index < Contract::endpoint_f32_ulp_padding; ++index) {
        lower = std::nextafter(
            lower, -std::numeric_limits<float>::infinity());
        upper = std::nextafter(
            upper, std::numeric_limits<float>::infinity());
    }
    return {static_cast<double>(lower),
            static_cast<double>(upper)};
}

bool rayTracingOracleContainsGpuHit(
    const RayTracingOracleHit &cpu, float gpu_t,
    std::uint32_t gpu_record_id) noexcept {
    const auto gpu = expandGpuRayT(gpu_t);
    return cpu.record_id == gpu_record_id &&
           gpu.lower >= cpu.t_interval.lower &&
           gpu.upper <= cpu.t_interval.upper;
}

RayTracingOracleFixture makeConstructiveRayTracingOracleFixture(
    RayTracingSentinelKind kind, std::uint32_t record_id,
    float hit_t) {
    const auto shift =
        kind == RayTracingSentinelKind::hit ? 0.0 : 8.0;
    RayTracingOracleFixture fixture{
        .ray = {
            .origin = {0.0, 0.0, 0.0},
            .direction = {0.0, 0.0, 1.0},
        },
        .triangles = {
            RayTracingOracleTriangle{
                .vertices = {
                    RayTracingOracleVec3{-1.0 + shift, -1.0,
                                         hit_t},
                    RayTracingOracleVec3{1.0 + shift, -1.0,
                                         hit_t},
                    RayTracingOracleVec3{0.0 + shift, 1.0,
                                         hit_t},
                },
                .record_id = record_id,
            },
        },
        .expected_kind = kind,
        .expected_record_id =
            kind == RayTracingSentinelKind::hit
                ? std::optional<std::uint32_t>{record_id}
                : std::nullopt,
    };
    // This is an assertion of the construction, not rejection sampling.
    validateRayTracingOracleFixture(fixture.ray, fixture.triangles);
    const auto result = traceRayTracingOracle(
        fixture.ray, fixture.triangles);
    require(result.hit.has_value() ==
                (kind == RayTracingSentinelKind::hit),
            "constructive sentinel classification failed");
    return fixture;
}

RayTracingRecursionSentinel rayTracingRecursionSentinel(
    std::uint32_t max_recursion, std::uint32_t requested_depth,
    std::uint32_t initial_payload) {
    const auto executed = std::min(max_recursion, requested_depth);
    return {
        .executed_depth = executed,
        .payload = initial_payload + executed,
    };
}

} // namespace Pelican
