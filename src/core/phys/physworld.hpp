#pragma once

#include "physquery.hpp"
#include "../container.hpp"
#include "../userpublic/components/collider.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

class Camera;
class DebugDraw;
struct TransformComponent;

struct PhysWorldTransform {
    vec3 pos{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    vec3 scale{1.0f, 1.0f, 1.0f};
};

struct PhysWorldColliderInput {
    std::string name;
    ColliderComponent collider;
    PhysWorldTransform transform;
};

std::vector<phys::Collider> buildPhysColliders(std::span<const PhysWorldColliderInput> inputs);

DECLARE_MODULE(PhysWorld) {
    struct Binding {
        std::string name;
        ColliderComponent collider;
        const TransformComponent *transform = nullptr;
        PhysWorldTransform static_transform;
    };

    std::vector<Binding> bindings;

  public:
    void clear();
    void bindCollider(std::string name, const ColliderComponent &collider,
                      const TransformComponent *transform = nullptr,
                      PhysWorldTransform static_transform = {});

    std::vector<phys::Collider> collectColliders() const;
    std::optional<phys::ObjectRaycastHit> raycastClosest(const phys::Ray &ray) const;
    std::vector<std::string> overlapAll(const phys::Shape &shape) const;
    void enqueueDebugDraw(DebugDraw &debug_draw, const Camera &camera) const;

    size_t colliderCountForTesting() const { return bindings.size(); }
};

} // namespace Pelican

