#include "trianglemesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <glm/matrix.hpp>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/quaternion.hpp>

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

struct SphereTriangleStart {
    double distance = std::numeric_limits<double>::infinity();
    glm::dvec3 normal{0};
};

struct SphereTriangleHit {
    double time;
    glm::dvec3 position, normal;
};

std::optional<SphereTriangleHit> castSphereTriangle(
    glm::dvec3 origin, glm::dvec3 motion, double radius,
    glm::dvec3 a, glm::dvec3 b, glm::dvec3 c,
    const glm::dvec3 *initialOutwardNormal = nullptr,
    SphereTriangleStart *initial = nullptr) {
    const auto squared = [](glm::dvec3 v) { return glm::dot(v,v); };
    const auto closestEdge = [&](glm::dvec3 p, glm::dvec3 a, glm::dvec3 b) {
        const auto edge = b-a;
        const double length2 = squared(edge);
        return a+(length2 == 0 ? 0 : std::clamp(glm::dot(p-a,edge)/length2,0.0,1.0))*edge;
    };
    double nearest = 1;
    std::optional<SphereTriangleHit> result;
    auto faceNormal = glm::cross(b-a,c-a);
    const double normalLength = glm::length(faceNormal);
    if (normalLength == 0) return std::nullopt;
    faceNormal /= normalLength;
    const auto inside = [&](glm::dvec3 p) {
        return glm::dot(glm::cross(b-a,p-a),faceNormal) >= 0 &&
               glm::dot(glm::cross(c-b,p-b),faceNormal) >= 0 &&
               glm::dot(glm::cross(a-c,p-c),faceNormal) >= 0;
    };
    const auto accept = [&](double t, glm::dvec3 position, glm::dvec3 normal) {
        if (t < 0 || t > nearest || (t == nearest && result)) return;
        // Future grazing contact is a hit; stationary/separating
        // contact at the start is not a depenetration result.
        if (t == 0 && glm::dot(motion,normal) >= 0) return;
        nearest = t;
        result = SphereTriangleHit{t,position,normal};
    };
    const double height = glm::dot(origin-a,faceNormal);
    auto closest = origin-height*faceNormal;
    if (!inside(closest)) {
        closest = closestEdge(origin,a,b);
        for (auto p : {closestEdge(origin,b,c),closestEdge(origin,c,a)})
            if (squared(origin-p) < squared(origin-closest)) closest = p;
    }
    const double distance = glm::length(origin-closest);
    if (radius-distance > shapeCastContactEpsilon)
        throw std::domain_error("sphere mesh initial depenetration is unavailable");
    auto initialNormal = faceNormal;
    if (distance > 0) initialNormal = (origin-closest)/distance;
    else if (glm::dot(initialNormal,motion) > 0) initialNormal = -initialNormal;
    if (initialOutwardNormal) initialNormal = *initialOutwardNormal;
    if (initial) *initial = {distance,initialNormal};
    if (distance <= radius+shapeCastContactEpsilon) {
        accept(0,closest,initialNormal);
        return result;
    }
    // The rounded triangle consists of two offset faces, three finite
    // edge cylinders and three vertex spheres. Solve each analytically.
    const double velocity = glm::dot(motion,faceNormal);
    if (velocity != 0) {
        for (double side : {-1.0,1.0}) {
            const double t = (side*radius-height)/velocity;
            const auto normal = side*faceNormal;
            const auto p = origin+t*motion-radius*normal;
            if (glm::dot(motion,normal) <= 0 && inside(p)) accept(t,p,normal);
        }
    }
    const auto firstRoot = [&](glm::dvec3 offset, glm::dvec3 velocityVector) -> std::optional<double> {
        const double qa = squared(velocityVector), qb = glm::dot(offset,velocityVector);
        const double qc = squared(offset)-radius*radius;
        if (qa == 0 || qb >= 0) return std::nullopt;
        const double discriminant = qb*qb-qa*qc;
        if (discriminant < 0) return std::nullopt;
        // Stable smaller root avoids cancellation for near contacts.
        return qc/(-qb+std::sqrt(discriminant));
    };
    const std::array<glm::dvec3,3> corners{a,b,c};
    for (std::size_t edgeIndex = 0; edgeIndex < 3; ++edgeIndex) {
        const auto start = corners[edgeIndex], end = corners[(edgeIndex+1)%3];
        const auto edge = end-start;
        const double length2 = squared(edge);
        if (length2 != 0) {
            const auto offset = origin-start;
            const auto perpendicularOffset = offset-edge*(glm::dot(offset,edge)/length2);
            const auto perpendicularMotion = motion-edge*(glm::dot(motion,edge)/length2);
            if (const auto t = firstRoot(perpendicularOffset,perpendicularMotion)) {
                const auto center = origin+*t*motion;
                const double u = glm::dot(center-start,edge)/length2;
                if (u >= 0 && u <= 1) {
                    const auto p = start+u*edge;
                    const auto separation = center-p;
                    const double length = glm::length(separation);
                    accept(*t,p,length > 0 ? separation/length : (velocity > 0 ? -faceNormal : faceNormal));
                }
            }
        }
        if (const auto t = firstRoot(origin-start,motion)) {
            const auto separation = origin+*t*motion-start;
            const double length = glm::length(separation);
            accept(*t,start,length > 0 ? separation/length : (velocity > 0 ? -faceNormal : faceNormal));
        }
    }
    return result;
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

std::optional<ShapeCastHit> TriangleMesh::shapeCast(
    const Sphere &sphere, vec3 delta, const glm::mat4 &local_to_world) const {
    return shapeCast(Capsule{sphere.center,{0,0,0,1},0,sphere.radius},delta,local_to_world);
}

std::optional<ShapeCastHit> TriangleMesh::shapeCast(
    const Capsule &capsule, vec3 delta, const glm::mat4 &local_to_world) const {
    if (nodes_.empty() || !finite(capsule.center) || !finite(delta) ||
        !std::isfinite(capsule.radius) || capsule.radius < 0) return std::nullopt;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(local_to_world[c][r])) return std::nullopt;
    if (local_to_world[0][3] != 0 || local_to_world[1][3] != 0 ||
        local_to_world[2][3] != 0 || local_to_world[3][3] != 1) return std::nullopt;
    const glm::dmat4 matrix{local_to_world};
    const double determinant = glm::determinant(matrix);
    if (!std::isfinite(determinant) || determinant == 0) return std::nullopt;
    if (!std::isfinite(capsule.half_height) || capsule.half_height < 0) return std::nullopt;
    const glm::dquat rotation{capsule.rotation.w,capsule.rotation.x,capsule.rotation.y,capsule.rotation.z};
    const double rotation2 = glm::dot(rotation,rotation);
    if (!std::isfinite(rotation2) || rotation2 == 0) return std::nullopt;
    const auto offset = (rotation/std::sqrt(rotation2))*glm::dvec3{0,capsule.half_height,0};
    const glm::dvec3 origin{capsule.center.x, capsule.center.y, capsule.center.z};
    const glm::dvec3 motion{delta.x, delta.y, delta.z};
    const double radius = capsule.radius;
    const auto worldPoint = [&](vec3 p) { return glm::dvec3(matrix * glm::dvec4{p.x,p.y,p.z,1}); };
    const auto toFloat = [](glm::dvec3 v) { return vec3{static_cast<float>(v.x),static_cast<float>(v.y),static_cast<float>(v.z)}; };
    double nearest = 1.0;
    std::size_t bestTriangle = std::numeric_limits<std::size_t>::max();
    std::optional<ShapeCastHit> result;
    // Traverse the swept world-space AABB. This remains conservative under
    // shear and nonuniform scale while preserving the world-space capsule.
    const auto low = glm::min(origin,origin+motion)-(glm::abs(offset)+glm::dvec3(radius+shapeCastContactEpsilon));
    const auto high = glm::max(origin,origin+motion)+(glm::abs(offset)+glm::dvec3(radius+shapeCastContactEpsilon));
    std::array<std::size_t, std::numeric_limits<std::size_t>::digits + 1> pending{};
    std::size_t pendingCount = 1;
    while (pendingCount != 0) {
        const auto &node = nodes_[pending[--pendingCount]];
        glm::dvec3 boundsLow{std::numeric_limits<double>::infinity()};
        glm::dvec3 boundsHigh{-std::numeric_limits<double>::infinity()};
        for (int corner = 0; corner < 8; ++corner) {
            const auto p = worldPoint({(corner&1) ? node.maximum.x : node.minimum.x,
                                       (corner&2) ? node.maximum.y : node.minimum.y,
                                       (corner&4) ? node.maximum.z : node.minimum.z});
            boundsLow = glm::min(boundsLow,p);
            boundsHigh = glm::max(boundsHigh,p);
        }
        if (glm::any(glm::lessThan(boundsHigh,low)) || glm::any(glm::greaterThan(boundsLow,high))) continue;
        if (node.count == 0) {
            pending[pendingCount++] = node.right;
            pending[pendingCount++] = node.left;
            continue;
        }
        for (std::size_t index = node.begin; index < node.begin+node.count; ++index) {
            const auto triangle = triangles_[index];
            const auto a = worldPoint(vertices_[indices_[triangle*3]]);
            const auto b = worldPoint(vertices_[indices_[triangle*3+1]]);
            const auto c = worldPoint(vertices_[indices_[triangle*3+2]]);
            // capsule = axis segment + sphere. Move the segment to the
            // triangle using the Minkowski difference, yielding a triangular
            // prism. Cast its sphere against the prism boundary, without a
            // temporary mesh/BVH or temporal sampling.
            const std::array<glm::dvec3,6> prism{a-offset,b-offset,c-offset,
                                                a+offset,b+offset,c+offset};
            constexpr std::array<std::array<std::size_t,3>,8> facets{{
                {0,1,2},{3,4,5},{0,1,4},{0,4,3},
                {1,2,5},{1,5,4},{2,0,3},{2,3,5}}};
            const std::array originals{a,b,c};
            const std::size_t facetCount = capsule.half_height == 0 ? 1 : facets.size();
            // A center inside the prism can be far from every boundary facet,
            // yet its capsule axis already crosses the original triangle.
            bool insidePrism = false;
            const auto centroid = (a+b+c)/3.0;
            const auto outwardNormal = [&](glm::dvec3 p, glm::dvec3 q, glm::dvec3 r) {
                auto normal = glm::cross(q-p,r-p);
                const double length = glm::length(normal);
                if (length != 0) normal /= length;
                if (glm::dot(normal,centroid-p) > 0) normal = -normal;
                return normal;
            };
            if (glm::dot(offset,glm::cross(b-a,c-a)) != 0) {
                double maximumDistance = -std::numeric_limits<double>::infinity();
                bool approaching = false, separating = false;
                for (const auto &facet : facets) {
                    const auto p = prism[facet[0]], q = prism[facet[1]], r = prism[facet[2]];
                    const auto normal = outwardNormal(p,q,r);
                    if (glm::dot(normal,normal) == 0) continue;
                    const double distance = glm::dot(normal,origin-p);
                    maximumDistance = std::max(maximumDistance,distance);
                    if (distance >= -shapeCastContactEpsilon) {
                        approaching |= glm::dot(normal,motion) < 0;
                        separating |= glm::dot(normal,motion) > 0;
                    }
                }
                insidePrism = maximumDistance <= 0;
                if (insidePrism && radius-maximumDistance > shapeCastContactEpsilon)
                    throw std::domain_error("capsule mesh initial depenetration is unavailable");
                // A zero-radius segment touching the prism boundary must move
                // inward. Unlike a point against a two-sided triangle, moving
                // away from any active boundary is separating contact.
                if (insidePrism && (separating || !approaching)) continue;
            }
            SphereTriangleStart closestStart;
            double triangleTime = nearest;
            std::optional<ShapeCastHit> triangleHit;
            for (std::size_t f = 0; f < facetCount; ++f) {
                const auto &facet = facets[f];
                const auto p = prism[facet[0]], q = prism[facet[1]], r = prism[facet[2]];
                const auto outward = insidePrism ? outwardNormal(p,q,r) : glm::dvec3{0};
                SphereTriangleStart initial;
                const auto hit = castSphereTriangle(origin,motion,radius,p,q,r,
                    insidePrism ? &outward : nullptr, &initial);
                if (initial.distance < closestStart.distance) closestStart = initial;
                if (!hit || hit->time > triangleTime ||
                    (hit->time == triangleTime && (triangleHit || triangle >= bestTriangle))) continue;
                // Carry barycentric coordinates back to the original triangle:
                // the prism witness itself is not a point on the mesh.
                const auto normal = glm::cross(q-p,r-p);
                const double denominator = glm::dot(normal,normal);
                const double u = glm::dot(glm::cross(hit->position-p,r-p),normal)/denominator;
                const double v = glm::dot(glm::cross(q-p,hit->position-p),normal)/denominator;
                const auto witness = (1-u-v)*originals[facet[0]%3]+u*originals[facet[1]%3]+v*originals[facet[2]%3];
                const auto position = toFloat(witness), worldNormal = toFloat(hit->normal);
                if (!finite(position) || !finite(worldNormal)) continue;
                triangleTime = hit->time;
                triangleHit = ShapeCastHit{static_cast<float>(hit->time),0,position,worldNormal,false};
            }
            // The prism is convex: a tangential/separating initial contact
            // cannot become an entering hit at another facet later in the cast.
            if (closestStart.distance <= radius+shapeCastContactEpsilon &&
                glm::dot(motion,closestStart.normal) >= 0) continue;
            if (triangleHit) {
                nearest = triangleTime;
                bestTriangle = triangle;
                result = triangleHit;
            }
        }
    }
    return result;
}

bool TriangleMesh::overlaps(const TriangleMesh &other,
    const glm::mat4 &local_to_world, const glm::mat4 &other_to_world) const {
    if (nodes_.empty() || other.nodes_.empty()) return false;
    const auto valid = [](const glm::mat4 &m) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m[c][r])) return false;
        const double determinant = glm::determinant(glm::dmat4{m});
        return m[0][3] == 0 && m[1][3] == 0 && m[2][3] == 0 && m[3][3] == 1 &&
               std::isfinite(determinant) && determinant != 0;
    };
    if (!valid(local_to_world) || !valid(other_to_world)) return false;
    const glm::dmat4 firstMatrix{local_to_world}, secondMatrix{other_to_world};
    const auto point = [](vec3 p, const glm::dmat4 &m) { return glm::dvec3(m*glm::dvec4{p.x,p.y,p.z,1}); };
    const auto bounds = [&](const Node &node, const glm::dmat4 &matrix) {
        glm::dvec3 low{std::numeric_limits<double>::infinity()}, high{-std::numeric_limits<double>::infinity()};
        for (int corner = 0; corner < 8; ++corner) {
            const auto p = point({(corner&1) ? node.maximum.x : node.minimum.x,
                                  (corner&2) ? node.maximum.y : node.minimum.y,
                                  (corner&4) ? node.maximum.z : node.minimum.z},matrix);
            low = glm::min(low,p); high = glm::max(high,p);
        }
        return std::pair{low,high};
    };
    const auto triangle = [&](const TriangleMesh &mesh, std::size_t index, const glm::dmat4 &matrix) {
        const auto base = mesh.triangles_[index]*3;
        return std::array{point(mesh.vertices_[mesh.indices_[base]],matrix),
                          point(mesh.vertices_[mesh.indices_[base+1]],matrix),
                          point(mesh.vertices_[mesh.indices_[base+2]],matrix)};
    };
    const auto intersect = [](const auto &a, const auto &b) {
        const auto separated = [&](glm::dvec3 axis) {
            const std::array pa{0.0,glm::dot(a[1]-a[0],axis),glm::dot(a[2]-a[0],axis)};
            const std::array pb{glm::dot(b[0]-a[0],axis),glm::dot(b[1]-a[0],axis),glm::dot(b[2]-a[0],axis)};
            // Shared-origin projections avoid translation cancellation. Allow
            // only floating arithmetic roundoff from transformed coordinates,
            // not a fixed world-space thickness for the triangle surface.
            double magnitude = 0;
            for (const auto &p : a) magnitude = std::max(magnitude,glm::dot(glm::abs(p),glm::abs(axis)));
            for (const auto &p : b) magnitude = std::max(magnitude,glm::dot(glm::abs(p),glm::abs(axis)));
            const double error = 64*std::numeric_limits<double>::epsilon()*magnitude;
            return *std::min_element(pa.begin(),pa.end()) > *std::max_element(pb.begin(),pb.end())+error ||
                   *std::min_element(pb.begin(),pb.end()) > *std::max_element(pa.begin(),pa.end())+error;
        };
        const std::array ea{a[1]-a[0],a[2]-a[1],a[0]-a[2]};
        const std::array eb{b[1]-b[0],b[2]-b[1],b[0]-b[2]};
        const auto na = glm::cross(ea[0],ea[1]), nb = glm::cross(eb[0],eb[1]);
        if (separated(na) || separated(nb)) return false;
        for (const auto &x : ea)
            for (const auto &y : eb) if (separated(glm::cross(x,y))) return false;
        // For coplanar triangles the edge-edge axes collapse to their normal;
        // include in-plane edge normals to distinguish disjoint surfaces.
        for (const auto &edge : ea) if (separated(glm::cross(na,edge))) return false;
        for (const auto &edge : eb) if (separated(glm::cross(nb,edge))) return false;
        return true;
    };
    // Split one median tree at a time. At most the sum of their depths is live.
    std::array<std::pair<std::size_t,std::size_t>,2*std::numeric_limits<std::size_t>::digits+1> pending{};
    std::size_t count = 1;
    while (count) {
        const auto [first,second] = pending[--count];
        const auto &a = nodes_[first], &b = other.nodes_[second];
        const auto [aLow,aHigh] = bounds(a,firstMatrix);
        const auto [bLow,bHigh] = bounds(b,secondMatrix);
        const auto magnitude = glm::max(glm::max(glm::abs(aLow),glm::abs(aHigh)),
                                         glm::max(glm::abs(bLow),glm::abs(bHigh)));
        const auto error = 64*std::numeric_limits<double>::epsilon()*magnitude;
        if (glm::any(glm::greaterThan(aLow,bHigh+error)) || glm::any(glm::greaterThan(bLow,aHigh+error))) continue;
        if (a.count == 0) {
            pending[count++] = {a.left,second}; pending[count++] = {a.right,second}; continue;
        }
        if (b.count == 0) {
            pending[count++] = {first,b.left}; pending[count++] = {first,b.right}; continue;
        }
        for (std::size_t i = a.begin; i < a.begin+a.count; ++i)
            for (std::size_t j = b.begin; j < b.begin+b.count; ++j)
                if (intersect(triangle(*this,i,firstMatrix),triangle(other,j,secondMatrix))) return true;
    }
    return false;
}

bool TriangleMesh::overlaps(const Sphere &sphere, const glm::mat4 &local_to_world) const {
    return overlaps(Capsule{sphere.center, {0,0,0,1}, 0, sphere.radius}, local_to_world);
}

bool TriangleMesh::overlaps(const Capsule &capsule, const glm::mat4 &local_to_world) const {
    if (nodes_.empty() || !finite(capsule.center) || !std::isfinite(capsule.radius) || capsule.radius < 0)
        return false;
    if (!std::isfinite(capsule.half_height) || capsule.half_height < 0) return false;
    const glm::dquat rotation{capsule.rotation.w, capsule.rotation.x, capsule.rotation.y, capsule.rotation.z};
    const double rotation2 = glm::dot(rotation, rotation);
    if (!std::isfinite(rotation2) || rotation2 == 0) return false;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(local_to_world[c][r])) return false;
    if (local_to_world[0][3] != 0 || local_to_world[1][3] != 0 ||
        local_to_world[2][3] != 0 || local_to_world[3][3] != 1) return false;
    const glm::dmat4 matrix{local_to_world};
    const double determinant = glm::determinant(matrix);
    if (!std::isfinite(determinant) || determinant == 0) return false;
    const glm::dvec3 center{capsule.center.x, capsule.center.y, capsule.center.z};
    const auto offset = (rotation / std::sqrt(rotation2)) * glm::dvec3{0, capsule.half_height, 0};
    const auto start = center - offset, end = center + offset;
    const auto direction = end - start;
    const double segment2 = glm::dot(direction, direction);
    const double radius2 = static_cast<double>(capsule.radius) * capsule.radius;
    const auto worldPoint = [&](vec3 p) { return glm::dvec3(matrix * glm::dvec4{p.x,p.y,p.z,1}); };
    const auto squared = [](glm::dvec3 v) { return glm::dot(v,v); };
    const auto edgeDistance2 = [&](glm::dvec3 point, glm::dvec3 a, glm::dvec3 b) {
        const auto edge = b-a;
        const double length2 = squared(edge);
        const double t = length2 == 0 ? 0 : std::clamp(glm::dot(point-a,edge)/length2,0.0,1.0);
        return squared(point-a-t*edge);
    };
    const auto segmentEdgeDistance2 = [&](glm::dvec3 a, glm::dvec3 b) {
        double best = std::min({edgeDistance2(start,a,b), edgeDistance2(end,a,b),
                               edgeDistance2(a,start,end), edgeDistance2(b,start,end)});
        const auto edge = b-a, relative = start-a;
        const double edge2 = squared(edge), product = glm::dot(direction,edge);
        // Endpoint minima cover parallel/degenerate segments. Otherwise also
        // consider the interior stationary point of the squared distance.
        const double denominator = squared(glm::cross(direction,edge));
        if (denominator > 0) {
            const double d = glm::dot(direction,relative), e = glm::dot(edge,relative);
            const double t = (product*e-edge2*d)/denominator;
            const double u = (segment2*e-product*d)/denominator;
            if (t >= 0 && t <= 1 && u >= 0 && u <= 1)
                best = std::min(best,squared(relative+t*direction-u*edge));
        }
        return best;
    };
    const auto touches = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c) {
        if (std::min({segmentEdgeDistance2(a,b), segmentEdgeDistance2(b,c), segmentEdgeDistance2(c,a)}) <= radius2)
            return true;
        const auto normal = glm::cross(b-a,c-a);
        const double normal2 = squared(normal);
        if (normal2 == 0) return false;
        const auto inside = [&](glm::dvec3 p) {
            return glm::dot(glm::cross(b-a,p-a),normal) >= 0 &&
                   glm::dot(glm::cross(c-b,p-b),normal) >= 0 &&
                   glm::dot(glm::cross(a-c,p-c),normal) >= 0;
        };
        const double h0 = glm::dot(start-a,normal), h1 = glm::dot(end-a,normal);
        for (const auto p : {start,end}) {
            const double height = glm::dot(p-a,normal);
            if (height*height <= radius2*normal2 && inside(p-(height/normal2)*normal)) return true;
        }
        if (h0 != h1) {
            const double t = h0/(h0-h1);
            if (t >= 0 && t <= 1 && inside(start+t*direction)) return true;
        }
        return false;
    };
    const auto query_low = glm::min(start,end)-glm::dvec3(capsule.radius);
    const auto query_high = glm::max(start,end)+glm::dvec3(capsule.radius);
    std::array<std::size_t, std::numeric_limits<std::size_t>::digits + 1> stack{};
    std::size_t size = 1;
    while (size) {
        const auto &node = nodes_[stack[--size]];
        glm::dvec3 low{std::numeric_limits<double>::infinity()}, high{-std::numeric_limits<double>::infinity()};
        for (int corner = 0; corner < 8; ++corner) {
            const auto p = worldPoint({(corner & 1) ? node.maximum.x : node.minimum.x,
                (corner & 2) ? node.maximum.y : node.minimum.y,
                (corner & 4) ? node.maximum.z : node.minimum.z});
            low = glm::min(low,p); high = glm::max(high,p);
        }
        if (glm::any(glm::greaterThan(low,query_high)) || glm::any(glm::lessThan(high,query_low))) continue;
        if (!node.count) { stack[size++] = node.left; stack[size++] = node.right; continue; }
        for (std::size_t i = node.begin; i < node.begin + node.count; ++i) {
            const auto triangle = triangles_[i] * 3;
            if (touches(worldPoint(vertices_[indices_[triangle]]), worldPoint(vertices_[indices_[triangle+1]]),
                        worldPoint(vertices_[indices_[triangle+2]]))) return true;
        }
    }
    return false;
}

std::optional<ShapeCastHit> TriangleMesh::shapeCast(
    const Box &box, vec3 delta, const glm::mat4 &local_to_world) const {
    if (nodes_.empty() || !finite(box.center) || !finite(box.half_extents) || !finite(delta) ||
        box.half_extents.x < 0 || box.half_extents.y < 0 || box.half_extents.z < 0) return std::nullopt;
    glm::dquat rotation{box.rotation.w,box.rotation.x,box.rotation.y,box.rotation.z};
    const double rotation2 = glm::dot(rotation,rotation);
    if (!std::isfinite(rotation2) || rotation2 == 0) return std::nullopt;
    rotation /= std::sqrt(rotation2);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(local_to_world[c][r])) return std::nullopt;
    if (local_to_world[0][3] != 0 || local_to_world[1][3] != 0 ||
        local_to_world[2][3] != 0 || local_to_world[3][3] != 1) return std::nullopt;
    const glm::dmat4 matrix{local_to_world};
    const double determinant = glm::determinant(matrix);
    if (!std::isfinite(determinant) || determinant == 0) return std::nullopt;
    const auto inverseRotation = glm::conjugate(rotation);
    const glm::dvec3 center{box.center.x,box.center.y,box.center.z};
    const glm::dvec3 extent{box.half_extents.x,box.half_extents.y,box.half_extents.z};
    const auto motion = inverseRotation*glm::dvec3{delta.x,delta.y,delta.z};
    const auto point = [&](vec3 p) {
        return inverseRotation*(glm::dvec3(matrix*glm::dvec4{p.x,p.y,p.z,1})-center);
    };
    const auto toFloat = [](glm::dvec3 p) { return vec3{static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z)}; };
    const double infinity = std::numeric_limits<double>::infinity();
    const auto preferred = glm::length(motion) > 0 ? -motion/glm::length(motion) : inverseRotation*glm::dvec3{1,0,0};
    const auto prefer = [&](glm::dvec3 a, glm::dvec3 b) {
        const auto aw = rotation*a, bw = rotation*b;
        return std::array{glm::dot(a,preferred),aw.x,aw.y,aw.z} >
               std::array{glm::dot(b,preferred),bw.x,bw.y,bw.z};
    };
    const auto low = glm::min(glm::dvec3{0},motion)-extent-glm::dvec3(shapeCastContactEpsilon);
    const auto high = glm::max(glm::dvec3{0},motion)+extent+glm::dvec3(shapeCastContactEpsilon);
    const std::array<glm::dvec3,3> axes{{{1,0,0},{0,1,0},{0,0,1}}};
    double nearest = 1;
    std::size_t bestTriangle = std::numeric_limits<std::size_t>::max();
    std::optional<ShapeCastHit> result;
    std::array<std::size_t,std::numeric_limits<std::size_t>::digits+1> stack{};
    std::size_t size = 1;
    while (size) {
        const auto &node = nodes_[stack[--size]];
        glm::dvec3 boundsLow{infinity}, boundsHigh{-infinity};
        for (int corner = 0; corner < 8; ++corner) {
            const auto p = point({(corner&1) ? node.maximum.x : node.minimum.x,
                                  (corner&2) ? node.maximum.y : node.minimum.y,
                                  (corner&4) ? node.maximum.z : node.minimum.z});
            boundsLow = glm::min(boundsLow,p); boundsHigh = glm::max(boundsHigh,p);
        }
        if (glm::any(glm::greaterThan(boundsLow,high)) || glm::any(glm::lessThan(boundsHigh,low))) continue;
        if (!node.count) { stack[size++] = node.left; stack[size++] = node.right; continue; }
        for (std::size_t i = node.begin; i < node.begin+node.count; ++i) {
            const auto triangle = triangles_[i];
            const auto a = point(vertices_[indices_[triangle*3]]);
            const auto b = point(vertices_[indices_[triangle*3+1]]);
            const auto c = point(vertices_[indices_[triangle*3+2]]);
            double entry = -infinity, leave = infinity, initialDepth = infinity;
            glm::dvec3 entryNormal{0}, contactNormal{0};
            bool separatingContact = false;
            // Fixed orientation makes every SAT interval linear in time. Their
            // intersection is the exact continuous overlap interval, including
            // edge-edge axes that a swept AABB or face-only cast would miss.
            const auto interval = [&](glm::dvec3 axis) {
                const double length = glm::length(axis);
                if (length == 0) return;
                axis /= length;
                const double radius = glm::dot(glm::abs(axis),extent);
                const double pa = glm::dot(a,axis), pb = glm::dot(b,axis), pc = glm::dot(c,axis);
                const double lo = std::min({pa,pb,pc})-radius, hi = std::max({pa,pb,pc})+radius;
                const double velocity = glm::dot(motion,axis);
                initialDepth = std::min({initialDepth,-lo,hi});
                const double depth = std::min(-lo,hi);
                if (depth <= shapeCastContactEpsilon) {
                    auto normal = -lo < hi ? -axis : axis;
                    // A zero-width or epsilon-thin interval has two equally
                    // close sides. Select one using the existing normal tie
                    // rule, rather than treating both as separating contacts.
                    if (std::abs(-lo-hi) <= shapeCastTieEpsilon)
                        normal = prefer(-axis,axis) ? -axis : axis;
                    if (glm::dot(motion,normal) > 0) separatingContact = true;
                    if (prefer(normal,contactNormal)) contactNormal = normal;
                }
                if (velocity == 0) {
                    if (lo > 0 || hi < 0) { entry = infinity; leave = -infinity; }
                    return;
                }
                double first = lo/velocity, last = hi/velocity;
                auto normal = -axis;
                if (first > last) { std::swap(first,last); normal = axis; }
                if (first > entry || (first == entry && prefer(normal,entryNormal))) {
                    entry = first; entryNormal = normal;
                }
                leave = std::min(leave,last);
            };
            for (const auto &axis : axes) interval(axis);
            const std::array edges{b-a,c-b,a-c};
            interval(glm::cross(edges[0],edges[1]));
            for (const auto &edge : edges)
                for (const auto &axis : axes) interval(glm::cross(edge,axis));
            if (initialDepth > shapeCastContactEpsilon)
                throw std::domain_error("box mesh initial depenetration is unavailable");
            if (entry > leave || leave < 0 || entry > nearest) continue;
            double t = std::max(0.0,entry);
            auto normal = entryNormal;
            if (initialDepth >= -shapeCastContactEpsilon) {
                if (separatingContact || glm::dot(motion,contactNormal) >= 0) continue;
                t = 0; normal = contactNormal;
            }
            if (t == nearest && triangle >= bestTriangle) continue;
            // Clip the triangle against the box at impact. Averaging the
            // resulting polygon gives a witness on the stationary triangle,
            // including edge-edge contacts; projecting the box center does not.
            // A convex triangle clipped by six planes has at most nine vertices.
            std::array<glm::dvec3,9> polygon{a,b,c}, clipped{};
            std::size_t count = 3;
            const auto impact = t*motion;
            for (int axis = 0; axis < 3 && count != 0; ++axis) {
                for (int side : {-1,1}) {
                    std::size_t clippedCount = 0;
                    if (count == 0) break;
                    const double bound = extent[axis]+shapeCastContactEpsilon;
                    auto previous = polygon[count-1];
                    double previousDistance = side*(previous[axis]-impact[axis])-bound;
                    for (std::size_t vertex = 0; vertex < count; ++vertex) {
                        const auto current = polygon[vertex];
                        const double distance = side*(current[axis]-impact[axis])-bound;
                        if ((distance <= 0) != (previousDistance <= 0))
                            clipped[clippedCount++] = previous+(current-previous)*(previousDistance/(previousDistance-distance));
                        if (distance <= 0) clipped[clippedCount++] = current;
                        previous = current; previousDistance = distance;
                    }
                    polygon.swap(clipped);
                    count = clippedCount;
                }
            }
            if (count == 0) continue;
            glm::dvec3 witness{0};
            for (std::size_t vertex = 0; vertex < count; ++vertex) witness += polygon[vertex];
            witness /= static_cast<double>(count);
            const auto position = toFloat(center+rotation*witness), worldNormal = toFloat(rotation*normal);
            if (!finite(position) || !finite(worldNormal)) continue;
            nearest = t; bestTriangle = triangle;
            result = ShapeCastHit{static_cast<float>(t),0,position,worldNormal,false};
        }
    }
    return result;
}

bool TriangleMesh::overlaps(const Box &box, const glm::mat4 &local_to_world) const {
    if (nodes_.empty() || !finite(box.center) || !finite(box.half_extents) ||
        box.half_extents.x < 0 || box.half_extents.y < 0 || box.half_extents.z < 0) return false;
    const glm::dquat rotation{box.rotation.w, box.rotation.x, box.rotation.y, box.rotation.z};
    const double length2 = glm::dot(rotation, rotation);
    if (!std::isfinite(length2) || length2 == 0) return false;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(local_to_world[c][r])) return false;
    if (local_to_world[0][3] != 0 || local_to_world[1][3] != 0 ||
        local_to_world[2][3] != 0 || local_to_world[3][3] != 1) return false;
    const glm::dmat4 matrix{local_to_world};
    const double determinant = glm::determinant(matrix);
    if (!std::isfinite(determinant) || determinant == 0) return false;
    const auto inverse_rotation = glm::conjugate(rotation / std::sqrt(length2));
    const glm::dvec3 center{box.center.x, box.center.y, box.center.z};
    const glm::dvec3 extent{box.half_extents.x, box.half_extents.y, box.half_extents.z};
    const auto point = [&](vec3 p) {
        return inverse_rotation * (glm::dvec3(matrix * glm::dvec4{p.x,p.y,p.z,1}) - center);
    };
    const std::array<glm::dvec3, 3> axes{{{1,0,0}, {0,1,0}, {0,0,1}}};
    const auto intersects = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c) {
        const auto separated = [&](glm::dvec3 axis) {
            const double radius = glm::dot(glm::abs(axis), extent);
            const double pa = glm::dot(a, axis), pb = glm::dot(b, axis), pc = glm::dot(c, axis);
            return std::min({pa,pb,pc}) > radius || std::max({pa,pb,pc}) < -radius;
        };
        for (const auto &axis : axes) if (separated(axis)) return false;
        const std::array edges{b-a, c-b, a-c};
        if (separated(glm::cross(edges[0], edges[1]))) return false;
        for (const auto &edge : edges)
            for (const auto &axis : axes)
                if (separated(glm::cross(edge, axis))) return false;
        return true;
    };
    std::array<std::size_t, std::numeric_limits<std::size_t>::digits + 1> stack{};
    std::size_t size = 1;
    while (size) {
        const auto &node = nodes_[stack[--size]];
        glm::dvec3 low{std::numeric_limits<double>::infinity()}, high{-std::numeric_limits<double>::infinity()};
        for (int corner = 0; corner < 8; ++corner) {
            const auto p = point({(corner & 1) ? node.maximum.x : node.minimum.x,
                (corner & 2) ? node.maximum.y : node.minimum.y,
                (corner & 4) ? node.maximum.z : node.minimum.z});
            low = glm::min(low,p); high = glm::max(high,p);
        }
        if (glm::any(glm::greaterThan(low, extent)) || glm::any(glm::lessThan(high,-extent))) continue;
        if (!node.count) { stack[size++] = node.left; stack[size++] = node.right; continue; }
        for (std::size_t i = node.begin; i < node.begin + node.count; ++i) {
            const auto triangle = triangles_[i] * 3;
            if (intersects(point(vertices_[indices_[triangle]]), point(vertices_[indices_[triangle+1]]),
                           point(vertices_[indices_[triangle+2]]))) return true;
        }
    }
    return false;
}

} // namespace Pelican::phys
