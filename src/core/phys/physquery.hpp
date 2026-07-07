#pragma once

#include "../userpublic/geom/quat.hpp"
#include "../userpublic/geom/vec.hpp"

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Pelican::phys {

struct Ray {
    vec3 origin{0.0f, 0.0f, 0.0f};
    vec3 direction{0.0f, 0.0f, 1.0f};
    float max_distance = 3.4028234663852886e38f;
};

struct Sphere {
    vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 0.5f;
};

struct Box {
    vec3 center{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    vec3 half_extents{0.5f, 0.5f, 0.5f};
};

struct Capsule {
    vec3 center{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    float half_height = 0.5f;
    float radius = 0.5f;
};

using Shape = std::variant<Sphere, Box, Capsule>;

struct Collider {
    std::string id;
    Shape shape;
};

struct RaycastHit {
    float distance = 0.0f;
    vec3 position{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 1.0f, 0.0f};
};

struct ObjectRaycastHit {
    std::string id;
    float distance = 0.0f;
    vec3 position{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 1.0f, 0.0f};
};

std::optional<RaycastHit> raycast(const Ray &ray, const Sphere &sphere);
std::optional<RaycastHit> raycast(const Ray &ray, const Box &box);
std::optional<RaycastHit> raycast(const Ray &ray, const Capsule &capsule);
std::optional<RaycastHit> raycast(const Ray &ray, const Shape &shape);

bool overlaps(const Sphere &lhs, const Sphere &rhs);
bool overlaps(const Sphere &lhs, const Box &rhs);
bool overlaps(const Box &lhs, const Sphere &rhs);
bool overlaps(const Sphere &lhs, const Capsule &rhs);
bool overlaps(const Capsule &lhs, const Sphere &rhs);
bool overlaps(const Box &lhs, const Box &rhs);
bool overlaps(const Capsule &lhs, const Capsule &rhs);
bool overlaps(const Box &lhs, const Capsule &rhs);
bool overlaps(const Capsule &lhs, const Box &rhs);
bool overlaps(const Shape &lhs, const Shape &rhs);

std::optional<ObjectRaycastHit> raycastClosest(const Ray &ray, std::span<const Collider> colliders);
std::vector<std::string> overlapAll(const Shape &shape, std::span<const Collider> colliders);

} // namespace Pelican::phys
