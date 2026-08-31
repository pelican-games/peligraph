#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

namespace RayTracingOracleContract {
inline constexpr double coordinate_limit = 1024.0;
inline constexpr double t_min = 1.0e-3;
inline constexpr double t_max = 4096.0;
inline constexpr double minimum_triangle_angle_degrees = 5.0;
inline constexpr double minimum_triangle_area = 1.0e-4;
inline constexpr double minimum_incidence_dot = 0.0871;
inline constexpr double maximum_transform_condition = 100.0;
inline constexpr double minimum_transform_determinant = 1.0e-3;
inline constexpr double relative_t_epsilon = 1.0e-3;
inline constexpr double barycentric_epsilon = 1.0e-3;
inline constexpr double maximum_relative_interval_width = 1.0e-2;
inline constexpr int endpoint_f32_ulp_padding = 4;
} // namespace RayTracingOracleContract

struct RayTracingOracleVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    bool operator==(const RayTracingOracleVec3 &) const = default;
};

// Row-major affine object-to-world matrix (three rows by four columns).
struct RayTracingOracleTransform {
    std::array<double, 12> object_to_world{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
    };
};

struct RayTracingOracleRay {
    RayTracingOracleVec3 origin;
    RayTracingOracleVec3 direction;
};

struct RayTracingOracleTriangle {
    std::array<RayTracingOracleVec3, 3> vertices;
    RayTracingOracleTransform transform;
    std::uint32_t record_id = 0;
    std::uint32_t primitive_id = 0;
    std::uint32_t instance_custom_index = 0;
    std::uint32_t record_u32 = 0;
};

struct RayTracingAnyHitIntegerInputs {
    std::uint32_t primitive_id = 0;
    std::uint32_t instance_custom_index = 0;
    std::uint32_t record_u32 = 0;
};

enum class RayTracingAnyHitIntegerPredicateMode {
    always_accept,
    accept_matching,
    ignore_matching,
};

// Constructive fixture predicate. The same immutable object is the CPU
// oracle and the GLSL generator; its grammar has no floating-point operand.
struct RayTracingAnyHitIntegerPredicate {
    RayTracingAnyHitIntegerPredicateMode mode =
        RayTracingAnyHitIntegerPredicateMode::always_accept;
    RayTracingAnyHitIntegerInputs match;

    [[nodiscard]] bool accepts(
        RayTracingAnyHitIntegerInputs inputs) const noexcept;
    [[nodiscard]] std::string glslAcceptExpression(
        std::string_view record_u32_expression) const;

    static RayTracingAnyHitIntegerPredicate alwaysAccept() noexcept;
    static RayTracingAnyHitIntegerPredicate acceptMatching(
        RayTracingAnyHitIntegerInputs match) noexcept;
    static RayTracingAnyHitIntegerPredicate ignoreMatching(
        RayTracingAnyHitIntegerInputs match) noexcept;
};

struct RayTracingOracleInterval {
    double lower = 0.0;
    double upper = 0.0;

    bool contains(double value) const noexcept {
        return value >= lower && value <= upper;
    }
};

struct RayTracingOracleIntersection {
    std::uint32_t record_id = 0;
    double u = 0.0;
    double v = 0.0;
    double t = 0.0;
    RayTracingOracleInterval t_interval;
    bool inside = false;
};

using RayTracingOracleIntersectorForTesting = std::function<
    RayTracingOracleIntersection(
        std::uint32_t, const RayTracingOracleRay &,
        const std::array<RayTracingOracleVec3, 3> &)>;

struct RayTracingOracleHit {
    std::uint32_t record_id = 0;
    double t_mid = 0.0;
    std::array<double, 3> barycentric{};
    RayTracingOracleInterval t_interval;
};

struct RayTracingOracleResult {
    std::optional<RayTracingOracleHit> hit;
};

// Throws when the authored fixture is outside the deterministic conformance
// subset. All inputs are first quantized to f32, then evaluated in double.
void validateRayTracingOracleFixture(
    const RayTracingOracleRay &ray,
    std::span<const RayTracingOracleTriangle> triangles);
RayTracingOracleResult traceRayTracingOracle(
    const RayTracingOracleRay &ray,
    std::span<const RayTracingOracleTriangle> triangles);
RayTracingOracleResult traceRayTracingOracle(
    const RayTracingOracleRay &ray,
    std::span<const RayTracingOracleTriangle> triangles,
    const RayTracingAnyHitIntegerPredicate &predicate);

// Explicit test seam for oracle mutation and boundary fixtures. Production
// callers always use the normative Moller-Trumbore implementation above.
void validateRayTracingOracleFixtureWithIntersectorForTesting(
    const RayTracingOracleRay &ray,
    std::span<const RayTracingOracleTriangle> triangles,
    const RayTracingOracleIntersectorForTesting &intersector);
RayTracingOracleResult traceRayTracingOracleWithIntersectorForTesting(
    const RayTracingOracleRay &ray,
    std::span<const RayTracingOracleTriangle> triangles,
    const RayTracingOracleIntersectorForTesting &intersector);

RayTracingOracleInterval expandGpuRayT(float value);
bool rayTracingOracleContainsGpuHit(
    const RayTracingOracleHit &cpu, float gpu_t,
    std::uint32_t gpu_record_id) noexcept;

enum class RayTracingSentinelKind { hit, miss };

struct RayTracingOracleFixture {
    RayTracingOracleRay ray;
    std::vector<RayTracingOracleTriangle> triangles;
    RayTracingSentinelKind expected_kind =
        RayTracingSentinelKind::hit;
    std::optional<std::uint32_t> expected_record_id;
};

// Algebraic construction (no rejection sampling): the hit point is at
// barycentrics (1/4, 1/2, 1/4), all angles and separations have fixed slack.
RayTracingOracleFixture makeConstructiveRayTracingOracleFixture(
    RayTracingSentinelKind kind,
    std::uint32_t record_id = 0x364u,
    float hit_t = 8.0f);

struct RayTracingRecursionSentinel {
    std::uint32_t executed_depth = 0;
    std::uint32_t payload = 0;

    bool operator==(const RayTracingRecursionSentinel &) const = default;
};

RayTracingRecursionSentinel rayTracingRecursionSentinel(
    std::uint32_t max_recursion, std::uint32_t requested_depth,
    std::uint32_t initial_payload = 0);

} // namespace Pelican
