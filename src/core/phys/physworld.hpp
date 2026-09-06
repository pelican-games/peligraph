#pragma once

#include "physquery.hpp"
#include "../userpublic/physics/abi_v3.hpp"
#include "../container.hpp"
#include "../userpublic/components/collider.hpp"
#include <details/ecs/entity.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace Pelican {
struct SceneLoadPhaseTrace;
namespace phys { class TriangleMesh; }

class DebugDraw;
class GameContext;

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
  public:
    struct MeshGeometry {
        std::vector<Physics::Vec3V1> vertices;
        std::vector<std::uint32_t> indices;
    };

    struct Binding {
        phys::ColliderIdentity identity;
        phys::ColliderQueryMetadata metadata;
        ColliderComponent collider;
        std::variant<GameObjectId, PhysWorldTransform> transform_source;
        bool runtime_only = false;
        std::shared_ptr<const MeshGeometry> mesh;
        std::shared_ptr<const MeshGeometry> accelerated_source;
        std::shared_ptr<const phys::TriangleMesh> mesh_acceleration;
    };

    struct PreparedState {
        std::vector<Binding> bindings;
        std::vector<std::pair<EntityId, EntityId>> active_trigger_pairs;
        std::uint64_t next_collider_id_value = 1;
    };

  private:

    using TriggerPair = std::pair<EntityId, EntityId>;

    std::vector<Binding> bindings;
    std::vector<TriggerPair> active_trigger_pairs;
    std::uint64_t next_collider_id_value = 1;

    phys::ColliderId allocateColliderId();

  public:
    PreparedState snapshotPrepared() const;
    PreparedState prepareBindings(std::vector<Binding> next_bindings) const;
    PreparedState prepareBindings(
        std::vector<Binding> next_bindings,
        std::span<const GameObjectId> unpublished_entities) const;
    PreparedState prepareSceneBindings(
        std::vector<Binding> next_bindings,
        std::span<const GameObjectId> unpublished_entities) const;
    void publishPrepared(PreparedState &&prepared) noexcept;
    void publishPrepared(PreparedState &&prepared,
                         SceneLoadPhaseTrace &phase_trace) noexcept;
    void clear();
    void bindCollider(std::string name, const ColliderComponent &collider, GameObjectId object_id,
                      std::optional<phys::ColliderQueryMetadata> metadata = std::nullopt);
    void bindCollider(std::string name, const ColliderComponent &collider,
                      PhysWorldTransform static_transform = {},
                      std::optional<phys::ColliderQueryMetadata> metadata = std::nullopt);

    std::vector<phys::Collider> collectColliders() const;
    Physics::Status queryRaycastAll(const phys::Ray &ray,
        const phys::QueryFilter *filter,
        std::vector<phys::RaycastQueryHit> &hits) const;
    std::vector<phys::RaycastQueryHit> raycastAll(
        const phys::Ray &ray, const phys::QueryFilter &filter = {}) const;
    std::optional<phys::ObjectRaycastHit> raycastClosest(const phys::Ray &ray) const;
    std::optional<phys::RaycastQueryHit> raycastClosest(
        const phys::Ray &ray, const phys::QueryFilter &filter) const;
    Physics::Status queryOverlapAll(const phys::Shape &shape,
        const phys::QueryFilter *filter, std::vector<phys::OverlapHit> &hits) const;
    std::vector<phys::OverlapHit> overlapAllHits(
        const phys::Shape &shape, const phys::QueryFilter &filter = {}) const;
    Physics::Status queryShapeCastAll(const phys::Shape &shape, vec3 delta,
        const phys::QueryFilter *filter, std::vector<phys::ShapeCastQueryHit> &hits) const;
    std::vector<phys::ShapeCastQueryHit> shapeCastAll(
        const phys::Shape &moving_shape, vec3 delta,
        const phys::QueryFilter &filter = {}) const;
    std::optional<phys::ShapeCastQueryHit> shapeCastClosest(
        const phys::Shape &moving_shape, vec3 delta,
        const phys::QueryFilter &filter = {}) const;
    std::vector<std::string> overlapAll(const phys::Shape &shape) const;
    std::vector<std::string> overlapAll(const phys::Shape &shape,
                                        const phys::QueryFilter &filter) const;
    void updateTriggers(GameContext &ctx);
    void enqueueDebugDraw(DebugDraw &debug_draw, const glm::mat4 &view_projection) const;

    size_t colliderCountForTesting() const { return bindings.size(); }
    size_t activeTriggerPairCountForTesting() const noexcept {
        return active_trigger_pairs.size();
    }
};

} // namespace Pelican

