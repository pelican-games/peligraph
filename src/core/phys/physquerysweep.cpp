#include "physquery.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Pelican::phys {
namespace {

constexpr float kDirectionEpsilon = 1.0e-6F;
constexpr float kDistanceTolerance = 2.0e-5F;
constexpr float kEpaTolerance = 1.0e-6F;
constexpr int kGjkIterations = 40;
constexpr int kEpaIterations = 256;
constexpr int kOverlapRefinementIterations = 24;
constexpr std::size_t kMaxSimplexVertices = 4;

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

bool finite(vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(quat value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

bool validShape(const Shape &shape) {
    return std::visit([](const auto &typed) {
        using ShapeType = std::remove_cvref_t<decltype(typed)>;
        if (!finite(typed.center)) return false;
        if constexpr (std::is_same_v<ShapeType, Sphere>) {
            return std::isfinite(typed.radius) && typed.radius >= 0.0F;
        } else if constexpr (std::is_same_v<ShapeType, Box>) {
            return finite(typed.rotation) && finite(typed.half_extents) &&
                   typed.half_extents.x >= 0.0F &&
                   typed.half_extents.y >= 0.0F &&
                   typed.half_extents.z >= 0.0F;
        } else {
            return finite(typed.rotation) &&
                   std::isfinite(typed.half_height) &&
                   std::isfinite(typed.radius) &&
                   typed.half_height >= 0.0F && typed.radius >= 0.0F;
        }
    }, shape);
}

vec3 normalizeOr(vec3 value, vec3 fallback) {
    const float value_length = length(value);
    if (!std::isfinite(value_length) || value_length <= kDirectionEpsilon) {
        return fallback;
    }
    return mul(value, 1.0F / value_length);
}

quat normalizedQuat(quat value) {
    const double length_value = std::hypot(
        std::hypot(static_cast<double>(value.x), static_cast<double>(value.y)),
        std::hypot(static_cast<double>(value.z), static_cast<double>(value.w)));
    if (!std::isfinite(length_value) || length_value <= kDirectionEpsilon) {
        return {0.0F, 0.0F, 0.0F, 1.0F};
    }
    return {
        static_cast<float>(static_cast<double>(value.x) / length_value),
        static_cast<float>(static_cast<double>(value.y) / length_value),
        static_cast<float>(static_cast<double>(value.z) / length_value),
        static_cast<float>(static_cast<double>(value.w) / length_value),
    };
}

vec3 rotateVector(quat rotation, vec3 value) {
    const quat q = normalizedQuat(rotation);
    const vec3 qv{q.x, q.y, q.z};
    const vec3 t = mul(cross(qv, value), 2.0F);
    return add(add(value, mul(t, q.w)), cross(qv, t));
}

vec3 shapeCenter(const Shape &shape) {
    return std::visit([](const auto &typed) { return typed.center; }, shape);
}

Shape translated(const Shape &shape, vec3 offset) {
    return std::visit([offset](auto typed) -> Shape {
        typed.center = add(typed.center, offset);
        return typed;
    }, shape);
}

float refineOverlapTime(const Shape &moving, vec3 delta,
                        const Shape &collider, float separated_time,
                        float overlapping_time) {
    float lower = separated_time;
    float upper = overlapping_time;
    for (int iteration = 0; iteration < kOverlapRefinementIterations;
         ++iteration) {
        const float middle = (lower + upper) * 0.5F;
        if (middle == lower || middle == upper) break;
        if (overlaps(translated(moving, mul(delta, middle)), collider)) {
            upper = middle;
        } else {
            lower = middle;
        }
    }
    return upper;
}

vec3 support(const Shape &shape, vec3 direction) {
    const vec3 unit_direction = normalizeOr(direction, {1.0F, 0.0F, 0.0F});
    return std::visit([&](const auto &typed) {
        using ShapeType = std::remove_cvref_t<decltype(typed)>;
        if constexpr (std::is_same_v<ShapeType, Sphere>) {
            return add(typed.center, mul(unit_direction, std::max(0.0F, typed.radius)));
        } else if constexpr (std::is_same_v<ShapeType, Box>) {
            const vec3 extents{
                std::abs(typed.half_extents.x),
                std::abs(typed.half_extents.y),
                std::abs(typed.half_extents.z),
            };
            const std::array axes{
                normalizeOr(rotateVector(typed.rotation, {1.0F, 0.0F, 0.0F}),
                            {1.0F, 0.0F, 0.0F}),
                normalizeOr(rotateVector(typed.rotation, {0.0F, 1.0F, 0.0F}),
                            {0.0F, 1.0F, 0.0F}),
                normalizeOr(rotateVector(typed.rotation, {0.0F, 0.0F, 1.0F}),
                            {0.0F, 0.0F, 1.0F}),
            };
            vec3 result = typed.center;
            for (std::size_t index = 0; index < axes.size(); ++index) {
                const float extent = index == 0 ? extents.x : index == 1 ? extents.y : extents.z;
                result = add(result, mul(axes[index], dot(direction, axes[index]) < 0.0F
                                                          ? -extent : extent));
            }
            return result;
        } else {
            const vec3 axis = normalizeOr(
                rotateVector(typed.rotation, {0.0F, 1.0F, 0.0F}),
                {0.0F, 1.0F, 0.0F});
            const float half_height = std::max(0.0F, typed.half_height);
            const vec3 segment_end = add(
                typed.center, mul(axis, dot(direction, axis) < 0.0F
                                            ? -half_height : half_height));
            return add(segment_end,
                       mul(unit_direction, std::max(0.0F, typed.radius)));
        }
    }, shape);
}

struct SupportVertex {
    vec3 difference{};
    vec3 point_on_moving{};
    vec3 point_on_collider{};
};

SupportVertex supportDifference(const Shape &moving, const Shape &collider,
                                vec3 direction) {
    const vec3 point_on_moving = support(moving, direction);
    const vec3 point_on_collider = support(collider, neg(direction));
    return {
        sub(point_on_moving, point_on_collider),
        point_on_moving,
        point_on_collider,
    };
}

bool solveLinear(int size, double matrix[3][4], std::array<double, 3> &solution) {
    for (int pivot = 0; pivot < size; ++pivot) {
        int best = pivot;
        for (int row = pivot + 1; row < size; ++row) {
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[best][pivot])) {
                best = row;
            }
        }
        if (std::abs(matrix[best][pivot]) <= 1.0e-12) {
            return false;
        }
        if (best != pivot) {
            for (int column = pivot; column <= size; ++column) {
                std::swap(matrix[pivot][column], matrix[best][column]);
            }
        }
        const double divisor = matrix[pivot][pivot];
        for (int column = pivot; column <= size; ++column) {
            matrix[pivot][column] /= divisor;
        }
        for (int row = 0; row < size; ++row) {
            if (row == pivot) continue;
            const double factor = matrix[row][pivot];
            for (int column = pivot; column <= size; ++column) {
                matrix[row][column] -= factor * matrix[pivot][column];
            }
        }
    }
    for (int index = 0; index < size; ++index) {
        solution[index] = matrix[index][size];
    }
    return true;
}

struct ClosestHullPoint {
    vec3 difference{};
    vec3 point_on_moving{};
    vec3 point_on_collider{};
    std::array<float, 4> weights{};
    std::uint32_t mask = 0;
    float distance_squared = std::numeric_limits<float>::infinity();
};

std::optional<ClosestHullPoint> closestForSubset(
    const std::vector<SupportVertex> &vertices, std::uint32_t mask) {
    if (vertices.empty() || vertices.size() > kMaxSimplexVertices) {
        throw std::logic_error("GJK closest-point input exceeds simplex capacity");
    }
    std::array<int, 4> indices{};
    int count = 0;
    for (int index = 0; index < static_cast<int>(vertices.size()); ++index) {
        if ((mask & (1U << index)) != 0) indices[count++] = index;
    }
    if (count == 0) return std::nullopt;

    std::array<double, 4> local_weights{};
    if (count == 1) {
        local_weights[0] = 1.0;
    } else {
        const vec3 base = vertices[indices[0]].difference;
        std::array<vec3, 3> columns{};
        for (int index = 1; index < count; ++index) {
            columns[index - 1] = sub(vertices[indices[index]].difference, base);
        }
        double matrix[3][4]{};
        for (int row = 0; row < count - 1; ++row) {
            for (int column = 0; column < count - 1; ++column) {
                matrix[row][column] = static_cast<double>(dot(columns[row], columns[column]));
            }
            matrix[row][count - 1] = -static_cast<double>(dot(columns[row], base));
        }
        std::array<double, 3> solution{};
        if (!solveLinear(count - 1, matrix, solution)) return std::nullopt;
        double sum = 0.0;
        for (int index = 1; index < count; ++index) {
            local_weights[index] = solution[index - 1];
            sum += local_weights[index];
        }
        local_weights[0] = 1.0 - sum;
    }

    double weight_sum = 0.0;
    for (int index = 0; index < count; ++index) {
        if (local_weights[index] < -1.0e-7) return std::nullopt;
        local_weights[index] = std::max(0.0, local_weights[index]);
        weight_sum += local_weights[index];
    }
    if (weight_sum <= 1.0e-12) return std::nullopt;

    ClosestHullPoint result;
    result.mask = mask;
    for (int index = 0; index < count; ++index) {
        const float weight = static_cast<float>(local_weights[index] / weight_sum);
        const int vertex_index = indices[index];
        result.weights[vertex_index] = weight;
        result.difference = add(result.difference,
                                mul(vertices[vertex_index].difference, weight));
        result.point_on_moving = add(result.point_on_moving,
                                     mul(vertices[vertex_index].point_on_moving, weight));
        result.point_on_collider = add(result.point_on_collider,
                                       mul(vertices[vertex_index].point_on_collider, weight));
    }
    result.distance_squared = lengthSquared(result.difference);
    return result;
}

ClosestHullPoint closestToOrigin(
    const std::vector<SupportVertex> &vertices,
    std::uint32_t maximum_subset_vertices = kMaxSimplexVertices) {
    if (vertices.empty() || vertices.size() > kMaxSimplexVertices) {
        throw std::logic_error("GJK simplex must contain between one and four vertices");
    }
    if (maximum_subset_vertices == 0 ||
        maximum_subset_vertices > kMaxSimplexVertices) {
        throw std::logic_error("GJK subset vertex limit is invalid");
    }
    ClosestHullPoint best;
    const std::uint32_t end_mask = 1U << static_cast<std::uint32_t>(vertices.size());
    for (std::uint32_t mask = 1; mask < end_mask; ++mask) {
        if (std::popcount(mask) >
            static_cast<int>(maximum_subset_vertices)) {
            continue;
        }
        const auto candidate = closestForSubset(vertices, mask);
        if (!candidate) continue;
        const bool smaller = candidate->distance_squared < best.distance_squared - 1.0e-12F;
        const bool richer_tie =
            std::abs(candidate->distance_squared - best.distance_squared) <= 1.0e-12F &&
            std::popcount(candidate->mask) > std::popcount(best.mask);
        if (smaller || richer_tie) best = *candidate;
    }
    return best;
}

void reduceSimplex(std::vector<SupportVertex> &vertices,
                   const ClosestHullPoint &closest) {
    if (vertices.size() > kMaxSimplexVertices) {
        throw std::logic_error("GJK reduction input exceeds simplex capacity");
    }
    std::vector<SupportVertex> reduced;
    reduced.reserve(kMaxSimplexVertices);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (closest.weights[index] > 1.0e-7F) reduced.push_back(vertices[index]);
    }
    if (reduced.empty() && !vertices.empty()) {
        reduced.push_back(vertices.front());
    }
    vertices = std::move(reduced);
}

bool duplicateVertex(const std::vector<SupportVertex> &vertices,
                     const SupportVertex &candidate) {
    return std::any_of(vertices.begin(), vertices.end(), [&](const SupportVertex &vertex) {
        return lengthSquared(sub(vertex.difference, candidate.difference)) <= 1.0e-12F;
    });
}

struct DistanceResult {
    float distance = 0.0F;
    vec3 point_on_moving{};
    vec3 point_on_collider{};
    vec3 normal{1.0F, 0.0F, 0.0F};
    bool touching_or_intersecting = false;
    std::vector<SupportVertex> simplex;
};

DistanceResult convexDistance(const Shape &moving, const Shape &collider) {
    vec3 direction = sub(shapeCenter(collider), shapeCenter(moving));
    direction = normalizeOr(direction, {1.0F, 0.0F, 0.0F});

    std::vector<SupportVertex> simplex;
    simplex.reserve(kMaxSimplexVertices);
    simplex.push_back(supportDifference(moving, collider, direction));
    ClosestHullPoint closest = closestToOrigin(simplex);

    for (int iteration = 0; iteration < kGjkIterations; ++iteration) {
        if (closest.distance_squared <= kDistanceTolerance * kDistanceTolerance) {
            return DistanceResult{
                .distance = std::sqrt(std::max(0.0F, closest.distance_squared)),
                .point_on_moving = closest.point_on_moving,
                .point_on_collider = closest.point_on_collider,
                .normal = normalizeOr(sub(closest.point_on_moving,
                                          closest.point_on_collider),
                                      normalizeOr(sub(shapeCenter(moving),
                                                      shapeCenter(collider)),
                                                  {1.0F, 0.0F, 0.0F})),
                .touching_or_intersecting = true,
                .simplex = simplex,
            };
        }

        direction = neg(closest.difference);
        const SupportVertex next = supportDifference(moving, collider, direction);
        const float progress = dot(next.difference, direction) -
                               dot(closest.difference, direction);
        if (progress <= kDistanceTolerance * std::max(1.0F, length(direction)) ||
            duplicateVertex(simplex, next)) {
            const float distance = std::sqrt(std::max(0.0F, closest.distance_squared));
            return DistanceResult{
                .distance = distance,
                .point_on_moving = closest.point_on_moving,
                .point_on_collider = closest.point_on_collider,
                .normal = normalizeOr(sub(closest.point_on_moving,
                                          closest.point_on_collider),
                                      {1.0F, 0.0F, 0.0F}),
                .touching_or_intersecting = distance <= kDistanceTolerance,
                .simplex = simplex,
            };
        }

        reduceSimplex(simplex, closest);
        if (simplex.size() == kMaxSimplexVertices) {
            // A four-vertex active simplex represents an origin-containing
            // tetrahedron and should have terminated above with distance zero.
            // Nearly singular tetrahedra can leave a larger float residual.
            // Confirm intersection using the shape-specific predicates; for a
            // separated numerical false positive, project onto the boundary so
            // a slot is always available for the next support point.
            if (overlaps(moving, collider)) {
                return DistanceResult{
                    .distance = 0.0F,
                    .point_on_moving = closest.point_on_moving,
                    .point_on_collider = closest.point_on_collider,
                    .normal = normalizeOr(
                        sub(closest.point_on_moving, closest.point_on_collider),
                        normalizeOr(sub(shapeCenter(moving), shapeCenter(collider)),
                                    {1.0F, 0.0F, 0.0F})),
                    .touching_or_intersecting = true,
                    .simplex = simplex,
                };
            }
            closest = closestToOrigin(simplex, kMaxSimplexVertices - 1);
            reduceSimplex(simplex, closest);
            continue;
        }
        simplex.push_back(next);
        closest = closestToOrigin(simplex);
    }

    const float distance = std::sqrt(std::max(0.0F, closest.distance_squared));
    return DistanceResult{
        .distance = distance,
        .point_on_moving = closest.point_on_moving,
        .point_on_collider = closest.point_on_collider,
        .normal = normalizeOr(sub(closest.point_on_moving,
                                  closest.point_on_collider),
                              {1.0F, 0.0F, 0.0F}),
        .touching_or_intersecting = distance <= kDistanceTolerance,
        .simplex = simplex,
    };
}

bool tetraContainsOrigin(const std::array<SupportVertex, 4> &tetra,
                         float &out_minimum_weight) {
    const vec3 base = tetra[0].difference;
    const std::array columns{
        sub(tetra[1].difference, base),
        sub(tetra[2].difference, base),
        sub(tetra[3].difference, base),
    };
    double matrix[3][4]{};
    for (int row = 0; row < 3; ++row) {
        matrix[row][0] = row == 0 ? columns[0].x : row == 1 ? columns[0].y : columns[0].z;
        matrix[row][1] = row == 0 ? columns[1].x : row == 1 ? columns[1].y : columns[1].z;
        matrix[row][2] = row == 0 ? columns[2].x : row == 1 ? columns[2].y : columns[2].z;
        matrix[row][3] = -(row == 0 ? base.x : row == 1 ? base.y : base.z);
    }
    std::array<double, 3> solution{};
    if (!solveLinear(3, matrix, solution)) return false;
    const std::array weights{
        1.0 - solution[0] - solution[1] - solution[2],
        solution[0], solution[1], solution[2],
    };
    out_minimum_weight = static_cast<float>(*std::min_element(weights.begin(), weights.end()));
    return out_minimum_weight >= -1.0e-6F;
}

std::optional<std::array<SupportVertex, 4>> enclosingTetrahedron(
    const Shape &moving, const Shape &collider,
    const std::vector<SupportVertex> &simplex) {
    std::vector<SupportVertex> vertices = simplex;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -1; z <= 1; ++z) {
                if (x == 0 && y == 0 && z == 0) continue;
                const auto candidate = supportDifference(
                    moving, collider,
                    normalizeOr(vec3{static_cast<float>(x), static_cast<float>(y),
                                     static_cast<float>(z)},
                                {1.0F, 0.0F, 0.0F}));
                if (!duplicateVertex(vertices, candidate)) vertices.push_back(candidate);
            }
        }
    }

    std::optional<std::array<SupportVertex, 4>> best;
    float best_minimum_weight = -std::numeric_limits<float>::infinity();
    for (std::size_t a = 0; a + 3 < vertices.size(); ++a) {
        for (std::size_t b = a + 1; b + 2 < vertices.size(); ++b) {
            for (std::size_t c = b + 1; c + 1 < vertices.size(); ++c) {
                for (std::size_t d = c + 1; d < vertices.size(); ++d) {
                    const std::array tetra{vertices[a], vertices[b], vertices[c], vertices[d]};
                    float minimum_weight = 0.0F;
                    if (tetraContainsOrigin(tetra, minimum_weight) &&
                        minimum_weight > best_minimum_weight) {
                        best = tetra;
                        best_minimum_weight = minimum_weight;
                    }
                }
            }
        }
    }
    return best;
}

struct EpaFace {
    int a = 0;
    int b = 0;
    int c = 0;
    vec3 normal{};
    float distance = 0.0F;
};

std::optional<EpaFace> makeFace(const std::vector<SupportVertex> &vertices,
                                int a, int b, int c) {
    vec3 normal = cross(sub(vertices[b].difference, vertices[a].difference),
                        sub(vertices[c].difference, vertices[a].difference));
    const float normal_length = length(normal);
    if (normal_length <= kDirectionEpsilon) return std::nullopt;
    normal = mul(normal, 1.0F / normal_length);
    float distance = dot(normal, vertices[a].difference);
    if (distance < 0.0F) {
        std::swap(b, c);
        normal = neg(normal);
        distance = -distance;
    }
    return EpaFace{a, b, c, normal, distance};
}

struct PenetrationResult {
    float depth = 0.0F;
    vec3 position{};
    vec3 normal{1.0F, 0.0F, 0.0F};
};

bool canonicalNormalGreater(vec3 lhs, vec3 rhs, vec3 preferred) {
    const std::array lhs_key{dot(lhs, preferred), lhs.x, lhs.y, lhs.z};
    const std::array rhs_key{dot(rhs, preferred), rhs.x, rhs.y, rhs.z};
    for (std::size_t index = 0; index < lhs_key.size(); ++index) {
        if (lhs_key[index] != rhs_key[index]) return lhs_key[index] > rhs_key[index];
    }
    return false;
}

std::vector<EpaFace>::const_iterator closestCanonicalFace(
    const std::vector<EpaFace> &faces, vec3 preferred) {
    if (faces.empty()) return faces.end();
    const float minimum_distance = std::min_element(
        faces.begin(), faces.end(), [](const EpaFace &lhs, const EpaFace &rhs) {
            return lhs.distance < rhs.distance;
        })->distance;

    auto selected = faces.end();
    for (auto candidate = faces.begin(); candidate != faces.end(); ++candidate) {
        if (candidate->distance > minimum_distance + shapeCastTieEpsilon) continue;
        if (selected == faces.end() ||
            canonicalNormalGreater(neg(candidate->normal), neg(selected->normal),
                                   preferred) ||
            (!canonicalNormalGreater(neg(selected->normal), neg(candidate->normal),
                                     preferred) &&
             std::array{candidate->a, candidate->b, candidate->c} <
                 std::array{selected->a, selected->b, selected->c})) {
            selected = candidate;
        }
    }
    return selected;
}

PenetrationResult facePenetration(const std::vector<SupportVertex> &vertices,
                                  const EpaFace &face) {
    const std::vector face_vertices{
        vertices[face.a], vertices[face.b], vertices[face.c]};
    const auto closest = closestToOrigin(face_vertices);
    return {
        face.distance,
        closest.point_on_collider,
        neg(face.normal),
    };
}

std::optional<PenetrationResult> penetration(
    const Shape &moving, const Shape &collider,
    const std::vector<SupportVertex> &simplex, vec3 preferred) {
    const auto tetra = enclosingTetrahedron(moving, collider, simplex);
    if (!tetra) return std::nullopt;

    std::vector<SupportVertex> vertices(tetra->begin(), tetra->end());
    std::vector<EpaFace> faces;
    for (const auto indices : std::array{
             std::array{0, 1, 2}, std::array{0, 3, 1},
             std::array{0, 2, 3}, std::array{1, 3, 2}}) {
        if (const auto face = makeFace(vertices, indices[0], indices[1], indices[2])) {
            faces.push_back(*face);
        }
    }
    if (faces.size() != 4) return std::nullopt;

    for (int iteration = 0; iteration < kEpaIterations; ++iteration) {
        const auto closest_face = closestCanonicalFace(faces, preferred);
        if (closest_face == faces.end()) return std::nullopt;

        const SupportVertex next = supportDifference(moving, collider, closest_face->normal);
        const float support_distance = dot(next.difference, closest_face->normal);
        if (support_distance - closest_face->distance <= kEpaTolerance ||
            duplicateVertex(vertices, next)) {
            return facePenetration(vertices, *closest_face);
        }

        struct Edge { int a; int b; };
        std::vector<Edge> boundary;
        auto addBoundaryEdge = [&](int a, int b) {
            const auto reverse = std::find_if(boundary.begin(), boundary.end(),
                                              [&](const Edge &edge) {
                                                  return edge.a == b && edge.b == a;
                                              });
            if (reverse != boundary.end()) boundary.erase(reverse);
            else boundary.push_back(Edge{a, b});
        };

        std::vector<EpaFace> retained;
        retained.reserve(faces.size());
        for (const auto &face : faces) {
            const bool visible =
                dot(face.normal, sub(next.difference,
                                     vertices[face.a].difference)) > kDistanceTolerance;
            if (!visible) {
                retained.push_back(face);
                continue;
            }
            addBoundaryEdge(face.a, face.b);
            addBoundaryEdge(face.b, face.c);
            addBoundaryEdge(face.c, face.a);
        }
        if (boundary.empty()) return facePenetration(vertices, *closest_face);

        const int next_index = static_cast<int>(vertices.size());
        vertices.push_back(next);
        faces = std::move(retained);
        for (const auto edge : boundary) {
            if (const auto face = makeFace(vertices, edge.a, edge.b, next_index)) {
                faces.push_back(*face);
            }
        }
    }

    const auto closest_face = closestCanonicalFace(faces, preferred);
    if (closest_face == faces.end()) return std::nullopt;
    return facePenetration(vertices, *closest_face);
}

vec3 fallbackNormal(const Shape &moving, const Shape &collider, vec3 delta) {
    return normalizeOr(sub(shapeCenter(moving), shapeCenter(collider)),
                       normalizeOr(neg(delta), {1.0F, 0.0F, 0.0F}));
}

vec3 preferredMtdNormal(vec3 delta) {
    return normalizeOr(neg(delta), {1.0F, 0.0F, 0.0F});
}

std::optional<ShapeCastHit> initialSphereContact(const Sphere &moving,
                                                 const Sphere &collider,
                                                 vec3 delta) {
    const vec3 center_delta = sub(moving.center, collider.center);
    const float center_distance = length(center_delta);
    const float signed_depth = moving.radius + collider.radius - center_distance;
    if (signed_depth < -shapeCastContactEpsilon) return std::nullopt;

    const vec3 normal = normalizeOr(center_delta, preferredMtdNormal(delta));
    const vec3 position = add(collider.center, mul(normal, collider.radius));
    if (signed_depth > shapeCastContactEpsilon) {
        return ShapeCastHit{0.0F, signed_depth, position, normal, true};
    }
    if (dot(delta, normal) < -kDirectionEpsilon) {
        return ShapeCastHit{0.0F, 0.0F, position, normal, false};
    }
    return std::nullopt;
}

} // namespace

std::optional<ShapeCastHit> shapeCast(const Shape &moving_shape, vec3 delta,
                                       const Shape &collider_shape) {
    if (!finite(delta) || !validShape(moving_shape) ||
        !validShape(collider_shape)) {
        return std::nullopt;
    }

    if (const auto *moving_sphere = std::get_if<Sphere>(&moving_shape)) {
        if (const auto *collider_sphere = std::get_if<Sphere>(&collider_shape)) {
            const float center_distance = length(
                sub(moving_sphere->center, collider_sphere->center));
            if (center_distance <= moving_sphere->radius + collider_sphere->radius +
                                       shapeCastContactEpsilon) {
                return initialSphereContact(*moving_sphere, *collider_sphere, delta);
            }
        }
    }

    const DistanceResult initial_distance = convexDistance(moving_shape, collider_shape);
    if (overlaps(moving_shape, collider_shape)) {
        const auto initial_penetration = penetration(
            moving_shape, collider_shape, initial_distance.simplex,
            preferredMtdNormal(delta));
        const float depth = initial_penetration ? initial_penetration->depth : 0.0F;
        const vec3 normal = normalizeOr(
            initial_penetration ? initial_penetration->normal
                                : initial_distance.normal,
            fallbackNormal(moving_shape, collider_shape, delta));
        const vec3 position = initial_penetration
                                  ? initial_penetration->position
                                  : initial_distance.point_on_collider;
        if (depth > shapeCastContactEpsilon) {
            return ShapeCastHit{0.0F, depth, position, normal, true};
        }
        if (dot(delta, normal) < -kDirectionEpsilon) {
            return ShapeCastHit{0.0F, 0.0F, position, normal, false};
        }
        return std::nullopt;
    }

    if (lengthSquared(delta) <= kDirectionEpsilon * kDirectionEpsilon) {
        return std::nullopt;
    }

    float time = 0.0F;
    float last_separated_time = 0.0F;
    for (int iteration = 0; iteration < kGjkIterations; ++iteration) {
        const Shape moved = translated(moving_shape, mul(delta, time));
        const DistanceResult distance = convexDistance(moved, collider_shape);
        if (overlaps(moved, collider_shape)) {
            const float refined_time = refineOverlapTime(
                moving_shape, delta, collider_shape, last_separated_time, time);
            const Shape contact_shape = translated(
                moving_shape, mul(delta, refined_time));
            const DistanceResult contact_distance = convexDistance(
                contact_shape, collider_shape);
            const vec3 normal = normalizeOr(
                contact_distance.normal,
                fallbackNormal(contact_shape, collider_shape, delta));
            return ShapeCastHit{
                std::clamp(refined_time, 0.0F, 1.0F),
                0.0F,
                contact_distance.point_on_collider,
                normal,
                false,
            };
        }
        if (distance.touching_or_intersecting ||
            distance.distance <= kDistanceTolerance) {
            const vec3 normal = normalizeOr(
                distance.normal, fallbackNormal(moved, collider_shape, delta));
            return ShapeCastHit{
                std::clamp(time, 0.0F, 1.0F),
                0.0F,
                distance.point_on_collider,
                normal,
                false,
            };
        }

        last_separated_time = time;

        const vec3 normal = normalizeOr(
            distance.normal, fallbackNormal(moved, collider_shape, delta));
        const float closing_speed = -dot(delta, normal);
        if (!std::isfinite(closing_speed) || closing_speed <= kDirectionEpsilon) {
            return std::nullopt;
        }

        const float step = std::max(
            (distance.distance - kDistanceTolerance) / closing_speed, 1.0e-6F);
        if (!std::isfinite(step)) return std::nullopt;
        time += step;
        if (time > 1.0F + kDistanceTolerance) {
            return std::nullopt;
        }
        time = std::min(time, 1.0F);
    }

    return std::nullopt;
}

} // namespace Pelican::phys
