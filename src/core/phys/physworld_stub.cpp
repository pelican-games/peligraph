#include "physworld.hpp"
#include "../loader/sceneload.hpp"

#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

[[noreturn]] void throwPhysicsUnavailable() {
    throw std::runtime_error(
        "PELICAN_WITH_PHYSICS=OFF: PhysWorld collider binding is unavailable");
}

} // namespace

std::vector<phys::Collider>
buildPhysColliders(std::span<const PhysWorldColliderInput> inputs) {
    if (!inputs.empty()) throwPhysicsUnavailable();
    return {};
}

phys::ColliderId PhysWorld::allocateColliderId() {
    throwPhysicsUnavailable();
}

PhysWorld::PreparedState PhysWorld::snapshotPrepared() const {
    return {};
}

PhysWorld::PreparedState
PhysWorld::prepareBindings(std::vector<Binding> next_bindings) const {
    return prepareBindings(std::move(next_bindings), {});
}

PhysWorld::PreparedState PhysWorld::prepareBindings(
    std::vector<Binding> next_bindings,
    std::span<const GameObjectId> unpublished_entities) const {
    (void)unpublished_entities;
    if (!next_bindings.empty()) throwPhysicsUnavailable();
    return {};
}

PhysWorld::PreparedState PhysWorld::prepareSceneBindings(
    std::vector<Binding> next_bindings,
    std::span<const GameObjectId> unpublished_entities) const {
    return prepareBindings(std::move(next_bindings), unpublished_entities);
}

void PhysWorld::publishPrepared(PreparedState &&prepared) noexcept {
    (void)prepared;
    bindings.clear();
    active_trigger_pairs.clear();
    next_collider_id_value = 1;
}

void PhysWorld::publishPrepared(PreparedState &&prepared,
                                SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::publish);
    publishPrepared(std::move(prepared));
}

void PhysWorld::clear() {
    bindings.clear();
    active_trigger_pairs.clear();
    next_collider_id_value = 1;
}

void PhysWorld::bindCollider(
    std::string name, const ColliderComponent &collider, GameObjectId object_id,
    std::optional<phys::ColliderQueryMetadata> metadata) {
    (void)name;
    (void)collider;
    (void)object_id;
    (void)metadata;
    throwPhysicsUnavailable();
}

void PhysWorld::bindCollider(
    std::string name, const ColliderComponent &collider,
    PhysWorldTransform static_transform,
    std::optional<phys::ColliderQueryMetadata> metadata) {
    (void)name;
    (void)collider;
    (void)static_transform;
    (void)metadata;
    throwPhysicsUnavailable();
}

std::vector<phys::Collider> PhysWorld::collectColliders() const {
    return {};
}

Physics::Status PhysWorld::queryRaycastAll(
    const phys::Ray &, const phys::QueryFilter *,
    std::vector<phys::RaycastQueryHit> &hits) const {
    hits.clear();
    return Physics::Status::unavailable;
}

std::vector<phys::RaycastQueryHit> PhysWorld::raycastAll(
    const phys::Ray &ray, const phys::QueryFilter &filter) const {
    (void)ray;
    (void)filter;
    return {};
}

std::optional<phys::RaycastQueryHit> PhysWorld::raycastClosest(
    const phys::Ray &ray, const phys::QueryFilter &filter) const {
    (void)ray;
    (void)filter;
    return std::nullopt;
}

Physics::Status PhysWorld::queryOverlapAll(const phys::Shape &,
    const phys::QueryFilter *, std::vector<phys::OverlapHit> &hits) const {
    hits.clear();
    return Physics::Status::unavailable;
}

std::vector<phys::OverlapHit> PhysWorld::overlapAllHits(
    const phys::Shape &shape, const phys::QueryFilter &filter) const {
    (void)shape;
    (void)filter;
    return {};
}

Physics::Status PhysWorld::queryShapeCastAll(const phys::Shape &, vec3,
    const phys::QueryFilter *, std::vector<phys::ShapeCastQueryHit> &hits) const {
    hits.clear();
    return Physics::Status::unavailable;
}

std::vector<phys::ShapeCastQueryHit> PhysWorld::shapeCastAll(
    const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter) const {
    (void)moving_shape;
    (void)delta;
    (void)filter;
    return {};
}

std::optional<phys::ShapeCastQueryHit> PhysWorld::shapeCastClosest(
    const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter) const {
    (void)moving_shape;
    (void)delta;
    (void)filter;
    return std::nullopt;
}

std::vector<std::string> PhysWorld::overlapAll(
    const phys::Shape &shape, const phys::QueryFilter &filter) const {
    (void)shape;
    (void)filter;
    return {};
}

void PhysWorld::updateTriggers(GameContext &ctx) {
    (void)ctx;
}

void PhysWorld::enqueueDebugDraw(
    DebugDraw &debug_draw, const glm::mat4 &view_projection) const {
    (void)debug_draw;
    (void)view_projection;
}

} // namespace Pelican
