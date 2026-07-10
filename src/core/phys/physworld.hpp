#pragma once

#include "physquery.hpp"
#include "../container.hpp"
#include "../userpublic/components/collider.hpp"
#include <details/ecs/entity.hpp>

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Pelican {

class Camera;
class DebugDraw;

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
        std::variant<GameObjectId, PhysWorldTransform> transform_source;
    };

    std::vector<Binding> bindings;

  public:
    void clear();
    void bindCollider(std::string name, const ColliderComponent &collider, GameObjectId object_id);
    void bindCollider(std::string name, const ColliderComponent &collider,
                      PhysWorldTransform static_transform = {});

    std::vector<phys::Collider> collectColliders() const;
    std::optional<phys::ObjectRaycastHit> raycastClosest(const phys::Ray &ray) const;
    std::vector<std::string> overlapAll(const phys::Shape &shape) const;
    void enqueueDebugDraw(DebugDraw &debug_draw, const Camera &camera) const;

    size_t colliderCountForTesting() const { return bindings.size(); }
};

} // namespace Pelican

