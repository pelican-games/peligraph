#pragma once

#include "physquery.hpp"

#include <glm/mat4x4.hpp>

#include <cstddef>

namespace Pelican::phys {

// Immutable CPU triangle geometry. Indices are explicit triples; an empty
// index stream is an empty mesh. Invalid indices/non-finite vertices throw
// invalid_argument. Zero-area triangles are retained in indices but not queried.
class TriangleMesh {
  public:
    TriangleMesh(std::span<const vec3> vertices,
                 std::span<const std::uint32_t> triangle_indices);

    // Unlike the standard-shape query, direction is not normalized: distance
    // and max_distance are the parameter t in origin + direction * t. This
    // preserves world distance after an affine inverse transforms a unit ray.
    // Both sides intersect; normals follow authored winding. Exact distance
    // ties select the earliest source triangle. Invalid rays return no hit.
    [[nodiscard]] std::optional<RaycastHit> raycast(const Ray &ray) const;
    // World directions are normalized before inverse transformation; distance
    // and max_distance are world units. Normals match the winding of transformed
    // vertices, including reflections. Invalid rays, non-finite, non-affine or
    // singular matrices, and unrepresentable local rays return no hit.
    [[nodiscard]] std::optional<RaycastHit> raycast(
        const Ray &world_ray, const glm::mat4 &local_to_world) const;
    // Surface overlap, inclusive of tangency. Radius stays in world units
    // under nonuniform scale/reflection. Invalid inputs return false.
    [[nodiscard]] bool overlaps(const Sphere &world_sphere,
                                const glm::mat4 &local_to_world) const;
    [[nodiscard]] bool overlaps(const Box &world_box,
                                const glm::mat4 &local_to_world) const;
    [[nodiscard]] bool overlaps(const Capsule &world_capsule,
                                const glm::mat4 &local_to_world) const;
    // Continuous two-sided surface cast, with world-space radius and delta.
    // Initial touching follows ShapeCastHit's approaching-only rule. Initial
    // penetration beyond contact epsilon throws domain_error: a nonconvex mesh
    // depenetration is not a single triangle's MTD. Invalid inputs return no hit.
    [[nodiscard]] std::optional<ShapeCastHit> shapeCast(
        const Sphere &world_sphere, vec3 delta,
        const glm::mat4 &local_to_world) const;
    [[nodiscard]] std::optional<ShapeCastHit> shapeCast(
        const Box &world_box, vec3 delta,
        const glm::mat4 &local_to_world) const;
    [[nodiscard]] std::span<const vec3> vertices() const noexcept { return vertices_; }
    [[nodiscard]] std::span<const std::uint32_t> indices() const noexcept { return indices_; }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices_.size() / 3; }

  private:
    struct Node {
        vec3 minimum{};
        vec3 maximum{};
        std::size_t begin = 0;
        std::size_t count = 0;
        std::size_t left = 0;
        std::size_t right = 0;
    };

    std::vector<vec3> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<std::size_t> triangles_;
    std::vector<Node> nodes_;

    std::size_t buildNode(std::size_t begin, std::size_t end);
};

} // namespace Pelican::phys
