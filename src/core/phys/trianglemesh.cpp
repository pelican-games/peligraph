#include "trianglemesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

namespace Pelican::phys {
namespace {

struct Vector {
    double x, y, z;
};

Vector toDouble(vec3 value) { return {value.x, value.y, value.z}; }
Vector subtract(Vector a, Vector b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vector cross(Vector a, Vector b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
double dot(Vector a, Vector b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double component(vec3 value, int axis) {
    return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
}
bool finite(vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool intersectsBounds(const Ray &ray, vec3 minimum, vec3 maximum, double limit) {
    double near = 0.0;
    double far = limit;
    for (int axis = 0; axis < 3; ++axis) {
        const double origin = component(ray.origin, axis);
        const double direction = component(ray.direction, axis);
        const double low = component(minimum, axis);
        const double high = component(maximum, axis);
        if (direction == 0.0) {
            if (origin < low || origin > high) return false;
            continue;
        }
        double first = (low - origin) / direction;
        double last = (high - origin) / direction;
        if (first > last) std::swap(first, last);
        near = std::max(near, first);
        far = std::min(far, last);
        if (near > far) return false;
    }
    return true;
}

} // namespace

TriangleMesh::TriangleMesh(std::span<const vec3> vertices,
                           std::span<const std::uint32_t> triangle_indices)
    : vertices_(vertices.begin(), vertices.end()),
      indices_(triangle_indices.begin(), triangle_indices.end()) {
    if (indices_.size() % 3 != 0)
        throw std::invalid_argument("triangle mesh indices must contain complete triangles");
    for (const auto vertex : vertices_)
        if (!finite(vertex))
            throw std::invalid_argument("triangle mesh contains a non-finite vertex");
    for (const auto index : indices_)
        if (index >= vertices_.size())
            throw std::invalid_argument("triangle mesh index exceeds vertex count");
    triangles_.reserve(triangleCount());
    for (std::size_t triangle = 0; triangle < triangleCount(); ++triangle) {
        const auto a = toDouble(vertices_[indices_[triangle * 3]]);
        const auto b = toDouble(vertices_[indices_[triangle * 3 + 1]]);
        const auto c = toDouble(vertices_[indices_[triangle * 3 + 2]]);
        const auto normal = cross(subtract(b, a), subtract(c, a));
        if (dot(normal, normal) != 0.0) triangles_.push_back(triangle);
    }
    if (!triangles_.empty()) buildNode(0, triangles_.size());
}

std::size_t TriangleMesh::buildNode(std::size_t begin, std::size_t end) {
    const float infinity = std::numeric_limits<float>::infinity();
    Node node{.minimum = {infinity, infinity, infinity},
              .maximum = {-infinity, -infinity, -infinity},
              .begin = begin, .count = end - begin};
    for (std::size_t index = begin; index < end; ++index) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto vertex = vertices_[indices_[triangles_[index] * 3 + corner]];
            node.minimum = {std::min(node.minimum.x, vertex.x),
                            std::min(node.minimum.y, vertex.y),
                            std::min(node.minimum.z, vertex.z)};
            node.maximum = {std::max(node.maximum.x, vertex.x),
                            std::max(node.maximum.y, vertex.y),
                            std::max(node.maximum.z, vertex.z)};
        }
    }
    const std::size_t result = nodes_.size();
    nodes_.push_back(node);
    if (end - begin <= 4) return result;
    int axis = 0;
    for (int candidate = 1; candidate < 3; ++candidate)
        if (component(node.maximum, candidate) - component(node.minimum, candidate) >
            component(node.maximum, axis) - component(node.minimum, axis)) axis = candidate;
    const auto centroid = [&](std::size_t triangle) {
        double sum = 0.0;
        for (std::size_t corner = 0; corner < 3; ++corner)
            sum += component(vertices_[indices_[triangle * 3 + corner]], axis);
        return sum / 3.0;
    };
    std::sort(triangles_.begin() + static_cast<std::ptrdiff_t>(begin),
              triangles_.begin() + static_cast<std::ptrdiff_t>(end),
              [&](std::size_t a, std::size_t b) {
                  const double ca = centroid(a), cb = centroid(b);
                  return ca == cb ? a < b : ca < cb;
              });
    const std::size_t middle = begin + (end - begin) / 2;
    const auto left = buildNode(begin, middle);
    const auto right = buildNode(middle, end);
    nodes_[result].left = left;
    nodes_[result].right = right;
    nodes_[result].count = 0;
    return result;
}

std::optional<RaycastHit> TriangleMesh::raycast(const Ray &ray) const {
    if (nodes_.empty() || !finite(ray.origin) || !finite(ray.direction) ||
        !std::isfinite(ray.max_distance) || ray.max_distance < 0.0F ||
        (ray.direction.x == 0.0F && ray.direction.y == 0.0F && ray.direction.z == 0.0F))
        return std::nullopt;
    double nearest = ray.max_distance;
    std::size_t best_triangle = std::numeric_limits<std::size_t>::max();
    Vector best_normal{};
    const auto direction = toDouble(ray.direction);
    const auto origin = toDouble(ray.origin);
    // A median tree has at most size_t bit-count depth. Traverse left first;
    // retain one right sibling per level, without allocating during queries.
    std::array<std::size_t, std::numeric_limits<std::size_t>::digits + 1> pending{};
    std::size_t pending_count = 1;
    while (pending_count != 0) {
        const auto &node = nodes_[pending[--pending_count]];
        if (!intersectsBounds(ray, node.minimum, node.maximum, nearest)) continue;
        if (node.count == 0) {
            pending[pending_count++] = node.right;
            pending[pending_count++] = node.left;
            continue;
        }
        for (std::size_t index = node.begin; index < node.begin + node.count; ++index) {
            const auto triangle = triangles_[index];
            const auto a = toDouble(vertices_[indices_[triangle * 3]]);
            const auto b = toDouble(vertices_[indices_[triangle * 3 + 1]]);
            const auto c = toDouble(vertices_[indices_[triangle * 3 + 2]]);
            const auto edge1 = subtract(b, a), edge2 = subtract(c, a);
            const auto p = cross(direction, edge2);
            const double determinant = dot(edge1, p);
            if (determinant == 0.0) continue;
            const auto offset = subtract(origin, a);
            const double u = dot(offset, p) / determinant;
            if (u < 0.0 || u > 1.0) continue;
            const auto q = cross(offset, edge1);
            const double v = dot(direction, q) / determinant;
            if (v < 0.0 || u + v > 1.0) continue;
            const double distance = dot(edge2, q) / determinant;
            if (distance < 0.0 || distance > nearest ||
                (distance == nearest && triangle >= best_triangle)) continue;
            nearest = distance;
            best_triangle = triangle;
            best_normal = cross(edge1, edge2);
        }
    }
    if (best_triangle == std::numeric_limits<std::size_t>::max()) return std::nullopt;
    const double normal_length = std::sqrt(dot(best_normal, best_normal));
    return RaycastHit{
        static_cast<float>(nearest),
        {static_cast<float>(origin.x + direction.x * nearest),
         static_cast<float>(origin.y + direction.y * nearest),
         static_cast<float>(origin.z + direction.z * nearest)},
        {static_cast<float>(best_normal.x / normal_length),
         static_cast<float>(best_normal.y / normal_length),
         static_cast<float>(best_normal.z / normal_length)},
    };
}

std::optional<RaycastHit> TriangleMesh::raycast(
    const Ray &world_ray, const glm::mat4 &local_to_world) const {
    if (!finite(world_ray.origin) || !finite(world_ray.direction) ||
        !std::isfinite(world_ray.max_distance) || world_ray.max_distance < 0.0F)
        return std::nullopt;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(local_to_world[column][row])) return std::nullopt;
    if (local_to_world[0][3] != 0.0F || local_to_world[1][3] != 0.0F ||
        local_to_world[2][3] != 0.0F || local_to_world[3][3] != 1.0F)
        return std::nullopt;
    const double direction_length = std::hypot(
        static_cast<double>(world_ray.direction.x),
        static_cast<double>(world_ray.direction.y),
        static_cast<double>(world_ray.direction.z));
    if (direction_length == 0.0) return std::nullopt;
    const glm::dmat4 transform(local_to_world);
    const double determinant = glm::determinant(transform);
    if (!std::isfinite(determinant) || determinant == 0.0) return std::nullopt;
    const auto inverse = glm::inverse(transform);
    const glm::dvec4 direction{
        world_ray.direction.x / direction_length,
        world_ray.direction.y / direction_length,
        world_ray.direction.z / direction_length, 0.0};
    const glm::dvec4 origin{world_ray.origin.x, world_ray.origin.y, world_ray.origin.z, 1.0};
    const auto local_origin = inverse * origin;
    const auto local_direction = inverse * direction;
    const Ray local_ray{
        {static_cast<float>(local_origin.x), static_cast<float>(local_origin.y),
         static_cast<float>(local_origin.z)},
        {static_cast<float>(local_direction.x), static_cast<float>(local_direction.y),
         static_cast<float>(local_direction.z)},
        world_ray.max_distance,
    };
    const auto local_hit = raycast(local_ray);
    if (!local_hit) return std::nullopt;
    auto normal = glm::transpose(inverse) * glm::dvec4{
        local_hit->normal.x, local_hit->normal.y, local_hit->normal.z, 0.0};
    // A cross product gains determinant sign under a reflection, whereas
    // inverse-transpose alone preserves the orientation of a normal covector.
    const double normal_length = std::hypot(normal.x, normal.y, normal.z);
    if (!std::isfinite(normal_length) || normal_length == 0.0) return std::nullopt;
    const double divisor = determinant < 0.0 ? -normal_length : normal_length;
    const auto position = origin + direction * static_cast<double>(local_hit->distance);
    RaycastHit result{
        local_hit->distance,
        {static_cast<float>(position.x), static_cast<float>(position.y),
         static_cast<float>(position.z)},
        {static_cast<float>(normal.x / divisor), static_cast<float>(normal.y / divisor),
         static_cast<float>(normal.z / divisor)},
    };
    if (!finite(result.position) || !finite(result.normal)) return std::nullopt;
    return result;
}

} // namespace Pelican::phys
