#pragma once

#include "physquery.hpp"
#include "../container.hpp"
#include "../userpublic/components/collider.hpp"
#include <details/ecs/entity.hpp>

#include <cstdint>
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
    phys::ColliderIdentity identity{};
    std::optional<phys::ColliderQueryMetadata> metadata;
};

std::vector<phys::Collider> buildPhysColliders(std::span<const PhysWorldColliderInput> inputs);

DECLARE_MODULE(PhysWorld) {
    struct Binding {
        phys::ColliderIdentity identity;
        phys::ColliderQueryMetadata metadata;
        ColliderComponent collider;
        std::variant<GameObjectId, PhysWorldTransform> transform_source;
    };

    std::vector<Binding> bindings;
    std::uint64_t next_collider_id_value = 1;

    phys::ColliderId allocateColliderId();

  public:
    void clear();
    void bindCollider(std::string name, const ColliderComponent &collider, GameObjectId object_id,
                      std::optional<phys::ColliderQueryMetadata> metadata = std::nullopt);
    void bindCollider(std::string name, const ColliderComponent &collider,
                      PhysWorldTransform static_transform = {},
                      std::optional<phys::ColliderQueryMetadata> metadata = std::nullopt);

    std::vector<phys::Collider> collectColliders() const;
    std::vector<phys::RaycastQueryHit> raycastAll(
        const phys::Ray &ray, const phys::QueryFilter &filter = {}) const;
    std::optional<phys::ObjectRaycastHit> raycastClosest(const phys::Ray &ray) const;
    std::optional<phys::RaycastQueryHit> raycastClosest(
        const phys::Ray &ray, const phys::QueryFilter &filter) const;
    std::vector<phys::OverlapHit> overlapAllHits(
        const phys::Shape &shape, const phys::QueryFilter &filter = {}) const;
    std::vector<phys::ShapeCastQueryHit> shapeCastAll(
        const phys::Shape &moving_shape, vec3 delta,
        const phys::QueryFilter &filter = {}) const;
    std::optional<phys::ShapeCastQueryHit> shapeCastClosest(
        const phys::Shape &moving_shape, vec3 delta,
        const phys::QueryFilter &filter = {}) const;
    std::vector<std::string> overlapAll(const phys::Shape &shape) const;
    std::vector<std::string> overlapAll(const phys::Shape &shape,
                                        const phys::QueryFilter &filter) const;
    void enqueueDebugDraw(DebugDraw &debug_draw, const Camera &camera) const;

    size_t colliderCountForTesting() const { return bindings.size(); }
};

} // namespace Pelican

