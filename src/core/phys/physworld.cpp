#include "physworld.hpp"

#include "../ecs/predefined/transform.hpp"
#include "../geomhelper/geomhelper.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/debugdraw.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Pelican {
namespace {

constexpr int kCircleSegments = 32;
constexpr float kProjectEpsilon = 1.0e-5f;
const glm::vec4 kColliderColor{0.1f, 0.95f, 0.85f, 1.0f};

glm::vec3 absVec(glm::vec3 value) {
    return {std::abs(value.x), std::abs(value.y), std::abs(value.z)};
}

glm::vec3 toGlmVec(vec3 value) {
    return {value.x, value.y, value.z};
}

vec3 fromGlmVec(glm::vec3 value) {
    return {value.x, value.y, value.z};
}

quat fromGlmQuat(glm::quat value) {
    return {value.x, value.y, value.z, value.w};
}

glm::quat normalized(glm::quat value) {
    const float len_sq = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    if (len_sq <= kProjectEpsilon * kProjectEpsilon) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    return glm::normalize(value);
}

PhysWorldTransform transformFromComponent(const TransformComponent &transform) {
    return PhysWorldTransform{
        .pos = fromGlmVec(transform.pos),
        .rotation = fromGlmQuat(transform.rotation),
        .scale = fromGlmVec(transform.scale),
    };
}

phys::Shape makeWorldShape(const ColliderComponent &collider, const PhysWorldTransform &transform) {
    collider.validate();

    const glm::vec3 world_pos = toGlmVec(transform.pos);
    const glm::quat world_rotation = normalized(to_glm(transform.rotation));
    const glm::vec3 world_scale = absVec(toGlmVec(transform.scale));
    const glm::vec3 local_pos = toGlmVec(collider.pos) * world_scale;
    const glm::vec3 center = world_pos + world_rotation * local_pos;
    const glm::quat shape_rotation = normalized(world_rotation * to_glm(collider.rotation));

    if (collider.shape == "sphere") {
        const float radius_scale = std::max({world_scale.x, world_scale.y, world_scale.z});
        return phys::Sphere{fromGlmVec(center), collider.radius * radius_scale};
    }

    if (collider.shape == "box") {
        const glm::vec3 half_extents = toGlmVec(collider.half_extents) * world_scale;
        return phys::Box{fromGlmVec(center), fromGlmQuat(shape_rotation), fromGlmVec(half_extents)};
    }

    if (collider.shape == "capsule") {
        const float radius_scale = std::max(world_scale.x, world_scale.z);
        return phys::Capsule{fromGlmVec(center), fromGlmQuat(shape_rotation),
                             collider.half_height * world_scale.y, collider.radius * radius_scale};
    }

    throw std::runtime_error("Unknown collider shape: " + collider.shape);
}

glm::quat shapeRotation(const phys::Box &box) {
    return normalized(to_glm(box.rotation));
}

glm::quat shapeRotation(const phys::Capsule &capsule) {
    return normalized(to_glm(capsule.rotation));
}

bool projectPoint(const glm::mat4 &view_projection, glm::vec3 world, glm::vec3 &out_ndc) {
    const glm::vec4 clip = view_projection * glm::vec4{world, 1.0f};
    if (std::abs(clip.w) <= kProjectEpsilon || clip.w < 0.0f) {
        return false;
    }
    out_ndc = glm::vec3{clip} / clip.w;
    return true;
}

void lineWorld(DebugDraw &debug_draw, const glm::mat4 &view_projection, glm::vec3 from, glm::vec3 to) {
    glm::vec3 from_ndc;
    glm::vec3 to_ndc;
    if (!projectPoint(view_projection, from, from_ndc) || !projectPoint(view_projection, to, to_ndc)) {
        return;
    }
    debug_draw.line(from_ndc, to_ndc, kColliderColor);
}

void circleWorld(DebugDraw &debug_draw, const glm::mat4 &view_projection, glm::vec3 center,
                 glm::vec3 axis_a, glm::vec3 axis_b, float radius) {
    glm::vec3 previous = center + axis_a * radius;
    for (int i = 1; i <= kCircleSegments; ++i) {
        const float angle = static_cast<float>(i) * 6.28318530717958647692f / static_cast<float>(kCircleSegments);
        const glm::vec3 next = center + axis_a * (std::cos(angle) * radius) + axis_b * (std::sin(angle) * radius);
        lineWorld(debug_draw, view_projection, previous, next);
        previous = next;
    }
}

void drawSphere(DebugDraw &debug_draw, const glm::mat4 &view_projection, const phys::Sphere &sphere) {
    const glm::vec3 center = toGlmVec(sphere.center);
    circleWorld(debug_draw, view_projection, center, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, sphere.radius);
    circleWorld(debug_draw, view_projection, center, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, sphere.radius);
    circleWorld(debug_draw, view_projection, center, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, sphere.radius);
}

void drawBox(DebugDraw &debug_draw, const glm::mat4 &view_projection, const phys::Box &box) {
    const glm::vec3 center = toGlmVec(box.center);
    const glm::quat rotation = shapeRotation(box);
    const glm::vec3 ex = rotation * glm::vec3{box.half_extents.x, 0.0f, 0.0f};
    const glm::vec3 ey = rotation * glm::vec3{0.0f, box.half_extents.y, 0.0f};
    const glm::vec3 ez = rotation * glm::vec3{0.0f, 0.0f, box.half_extents.z};

    const std::array<glm::vec3, 8> corners{
        center - ex - ey - ez,
        center + ex - ey - ez,
        center + ex + ey - ez,
        center - ex + ey - ez,
        center - ex - ey + ez,
        center + ex - ey + ez,
        center + ex + ey + ez,
        center - ex + ey + ez,
    };

    constexpr std::array<std::pair<int, int>, 12> edges{
        std::pair{0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    for (const auto &[from, to] : edges) {
        lineWorld(debug_draw, view_projection, corners[from], corners[to]);
    }
}

void drawCapsule(DebugDraw &debug_draw, const glm::mat4 &view_projection, const phys::Capsule &capsule) {
    const glm::vec3 center = toGlmVec(capsule.center);
    const glm::quat rotation = shapeRotation(capsule);
    const glm::vec3 axis = rotation * glm::vec3{0.0f, 1.0f, 0.0f};
    const glm::vec3 side_a = rotation * glm::vec3{1.0f, 0.0f, 0.0f};
    const glm::vec3 side_b = rotation * glm::vec3{0.0f, 0.0f, 1.0f};
    const glm::vec3 bottom = center - axis * capsule.half_height;
    const glm::vec3 top = center + axis * capsule.half_height;

    circleWorld(debug_draw, view_projection, bottom, side_a, side_b, capsule.radius);
    circleWorld(debug_draw, view_projection, top, side_a, side_b, capsule.radius);
    lineWorld(debug_draw, view_projection, bottom + side_a * capsule.radius, top + side_a * capsule.radius);
    lineWorld(debug_draw, view_projection, bottom - side_a * capsule.radius, top - side_a * capsule.radius);
    lineWorld(debug_draw, view_projection, bottom + side_b * capsule.radius, top + side_b * capsule.radius);
    lineWorld(debug_draw, view_projection, bottom - side_b * capsule.radius, top - side_b * capsule.radius);
    circleWorld(debug_draw, view_projection, bottom, axis, side_a, capsule.radius);
    circleWorld(debug_draw, view_projection, top, axis, side_a, capsule.radius);
    circleWorld(debug_draw, view_projection, bottom, axis, side_b, capsule.radius);
    circleWorld(debug_draw, view_projection, top, axis, side_b, capsule.radius);
}

void drawCollider(DebugDraw &debug_draw, const glm::mat4 &view_projection, const phys::Collider &collider) {
    std::visit([&](const auto &shape) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(shape)>, phys::Sphere>) {
            drawSphere(debug_draw, view_projection, shape);
        } else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(shape)>, phys::Box>) {
            drawBox(debug_draw, view_projection, shape);
        } else {
            drawCapsule(debug_draw, view_projection, shape);
        }
    }, collider.shape);
}

} // namespace

std::vector<phys::Collider> buildPhysColliders(std::span<const PhysWorldColliderInput> inputs) {
    std::vector<phys::Collider> colliders;
    colliders.reserve(inputs.size());

    for (const auto &input : inputs) {
        if (input.name.empty()) {
            throw std::runtime_error("Collider object requires a name");
        }
        colliders.push_back(phys::Collider{
            input.name,
            makeWorldShape(input.collider, input.transform),
        });
    }

    std::sort(colliders.begin(), colliders.end(), [](const phys::Collider &lhs, const phys::Collider &rhs) {
        return lhs.id < rhs.id;
    });
    return colliders;
}

void PhysWorld::clear() {
    bindings.clear();
}

void PhysWorld::bindCollider(std::string name, const ColliderComponent &collider,
                             const TransformComponent *transform, PhysWorldTransform static_transform) {
    if (name.empty()) {
        throw std::runtime_error("Collider object requires a name");
    }
    if (std::any_of(bindings.begin(), bindings.end(), [&](const Binding &binding) {
            return binding.name == name;
        })) {
        throw std::runtime_error("Duplicate collider object name: " + name);
    }

    collider.validate();
    bindings.push_back(Binding{
        .name = std::move(name),
        .collider = collider,
        .transform = transform,
        .static_transform = static_transform,
    });
}

std::vector<phys::Collider> PhysWorld::collectColliders() const {
    std::vector<PhysWorldColliderInput> inputs;
    inputs.reserve(bindings.size());
    for (const auto &binding : bindings) {
        inputs.push_back(PhysWorldColliderInput{
            .name = binding.name,
            .collider = binding.collider,
            .transform = binding.transform != nullptr ? transformFromComponent(*binding.transform)
                                                      : binding.static_transform,
        });
    }
    return buildPhysColliders(inputs);
}

std::optional<phys::ObjectRaycastHit> PhysWorld::raycastClosest(const phys::Ray &ray) const {
    const auto colliders = collectColliders();
    return phys::raycastClosest(ray, colliders);
}

std::vector<std::string> PhysWorld::overlapAll(const phys::Shape &shape) const {
    const auto colliders = collectColliders();
    return phys::overlapAll(shape, colliders);
}

void PhysWorld::enqueueDebugDraw(DebugDraw &debug_draw, const Camera &camera) const {
    const auto colliders = collectColliders();
    const glm::mat4 view_projection = camera.getVPMatrix();
    for (const auto &collider : colliders) {
        drawCollider(debug_draw, view_projection, collider);
    }
}

} // namespace Pelican
