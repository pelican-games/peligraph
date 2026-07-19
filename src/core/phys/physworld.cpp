#include "physworld.hpp"
#include "physicsruntime.hpp"
#include "physqueryinternal.hpp"

#include "../ecs/predefined/transform.hpp"
#include "../ecs/core.hpp"
#include "../geomhelper/geomhelper.hpp"
#include "../renderer/debugdraw.hpp"
#include "../userpublic/details/system/registerer.hpp"
#include "../userpublic/gamecontext.hpp"
#include <components/predefined.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

constexpr int kCircleSegments = 32;
constexpr float kProjectEpsilon = 1.0e-5f;
const glm::vec4 kColliderColor{0.1f, 0.95f, 0.85f, 1.0f};

struct PhysicsTriggerSystem {
    void update(GameContext &ctx) {
        if (auto *world = FastModuleContainer::tryGet<PhysWorld>()) {
            world->updateTriggers(ctx);
        }
    }
};

struct PhysicsTriggerSystemRegistration {
    PhysicsTriggerSystemRegistration() {
        internal::getGameSystemRegisterer().registerSystem<PhysicsTriggerSystem>(
            "PhysicsTriggerSystem", std::numeric_limits<int>::max(), {});
    }
};

const PhysicsTriggerSystemRegistration physics_trigger_system_registration;

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

bool finite(vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(quat value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

void validateWorldTransform(const PhysWorldTransform &transform) {
    if (!finite(transform.pos) || !finite(transform.rotation) ||
        !finite(transform.scale)) {
        throw std::runtime_error("Collider world transform must contain finite values");
    }
}

phys::ColliderQueryMetadata metadataFromComponent(
    const ColliderComponent &collider) {
    return phys::ColliderQueryMetadata{
        .layer = collider.layer,
        .mask = collider.mask,
        .trigger = collider.trigger,
        .one_way = collider.one_way,
    };
}

phys::Shape makeWorldShape(const ColliderComponent &collider, const PhysWorldTransform &transform) {
    collider.validate();
    validateWorldTransform(transform);

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
            input.identity,
            input.metadata.value_or(metadataFromComponent(input.collider)),
        });
        colliders.back().identity = phys::effectiveColliderIdentity(colliders.back());
    }

    std::sort(colliders.begin(), colliders.end(), [](const phys::Collider &lhs, const phys::Collider &rhs) {
        return lhs.id < rhs.id;
    });
    return colliders;
}

phys::ColliderId PhysWorld::allocateColliderId() {
    if (next_collider_id_value == phys::invalid_collider_id.value) {
        throw std::overflow_error("PhysWorld collider id space exhausted");
    }
    return phys::ColliderId{next_collider_id_value++};
}

PhysWorld::PreparedState PhysWorld::snapshotPrepared() const {
    return PreparedState{
        .bindings = bindings,
        .next_collider_id_value = next_collider_id_value,
    };
}

PhysWorld::PreparedState
PhysWorld::prepareBindings(std::vector<Binding> next_bindings) const {
    return prepareBindings(std::move(next_bindings), {});
}

PhysWorld::PreparedState PhysWorld::prepareBindings(
    std::vector<Binding> next_bindings,
    std::span<const GameObjectId> unpublished_entities) const {
    PreparedState prepared{
        .bindings = std::move(next_bindings),
        .next_collider_id_value = next_collider_id_value,
    };
    std::unordered_set<std::string> names;
    names.reserve(prepared.bindings.size());
    auto *ecs = FastModuleContainer::tryGet<ECSCore>();
    for (auto &binding : prepared.bindings) {
        if (binding.identity.name.empty()) {
            throw std::runtime_error("Collider object requires a name");
        }
        if (!names.insert(binding.identity.name).second) {
            throw std::runtime_error("Duplicate collider object name: " +
                                     binding.identity.name);
        }
        binding.collider.validate();
        if (const auto *object_id =
                std::get_if<GameObjectId>(&binding.transform_source)) {
            const bool unpublished =
                std::find(unpublished_entities.begin(),
                          unpublished_entities.end(), *object_id) !=
                unpublished_entities.end();
            if (!unpublished &&
                (ecs == nullptr ||
                 ecs->getTemplatePublicModule()
                         .tryComponent<TransformComponent>(*object_id) == nullptr)) {
                throw std::runtime_error(
                    "Collider object has no live transform: " +
                    binding.identity.name + " (" + toString(*object_id) + ")");
            }
            binding.identity.entity = *object_id;
        } else {
            validateWorldTransform(
                std::get<PhysWorldTransform>(binding.transform_source));
            binding.identity.entity.reset();
        }
        if (binding.identity.collider_id == phys::invalid_collider_id) {
            if (prepared.next_collider_id_value ==
                phys::invalid_collider_id.value) {
                throw std::overflow_error("PhysWorld collider id space exhausted");
            }
            binding.identity.collider_id =
                phys::ColliderId{prepared.next_collider_id_value++};
        } else if (binding.identity.collider_id.value >=
                   prepared.next_collider_id_value) {
            if (binding.identity.collider_id.value ==
                std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("PhysWorld collider id space exhausted");
            }
            prepared.next_collider_id_value =
                binding.identity.collider_id.value + 1;
        }
    }
    return prepared;
}

void PhysWorld::publishPrepared(PreparedState &&prepared) noexcept {
    bindings.swap(prepared.bindings);
    std::swap(next_collider_id_value, prepared.next_collider_id_value);
}

void PhysWorld::clear() {
    bindings.clear();
    // A full scene reset is a domain boundary: old EntityIds are no longer
    // addressable, so it deliberately produces no synthetic OverlapExit.
    active_trigger_pairs.clear();
}

void PhysWorld::bindCollider(std::string name, const ColliderComponent &collider,
                             GameObjectId object_id,
                             std::optional<phys::ColliderQueryMetadata> metadata) {
    if (name.empty()) {
        throw std::runtime_error("Collider object requires a name");
    }
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&](const Binding &binding) {
        const auto *bound_id = std::get_if<GameObjectId>(&binding.transform_source);
        return binding.identity.name == name && bound_id != nullptr &&
               ecs.tryComponent<TransformComponent>(*bound_id) == nullptr;
    }), bindings.end());
    if (ecs.tryComponent<TransformComponent>(object_id) == nullptr) {
        throw std::runtime_error("Collider object has no live transform: " + name + " (" +
                                 toString(object_id) + ")");
    }
    if (std::any_of(bindings.begin(), bindings.end(), [&](const Binding &binding) {
            return binding.identity.name == name;
        })) {
        throw std::runtime_error("Duplicate collider object name: " + name);
    }

    collider.validate();
    bindings.push_back(Binding{
        .identity = phys::ColliderIdentity{
            .collider_id = allocateColliderId(),
            .name = std::move(name),
            .entity = object_id,
        },
        .metadata = metadata.value_or(metadataFromComponent(collider)),
        .collider = collider,
        .transform_source = object_id,
    });
}

void PhysWorld::bindCollider(std::string name, const ColliderComponent &collider,
                             PhysWorldTransform static_transform,
                             std::optional<phys::ColliderQueryMetadata> metadata) {
    if (name.empty()) {
        throw std::runtime_error("Collider object requires a name");
    }
    if (std::any_of(bindings.begin(), bindings.end(), [&](const Binding &binding) {
            return binding.identity.name == name;
        })) {
        throw std::runtime_error("Duplicate collider object name: " + name);
    }
    collider.validate();
    validateWorldTransform(static_transform);
    bindings.push_back(Binding{
        .identity = phys::ColliderIdentity{
            .collider_id = allocateColliderId(),
            .name = std::move(name),
        },
        .metadata = metadata.value_or(metadataFromComponent(collider)),
        .collider = collider,
        .transform_source = static_transform,
    });
}

std::vector<phys::Collider> PhysWorld::collectColliders() const {
    std::vector<PhysWorldColliderInput> inputs;
    inputs.reserve(bindings.size());
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    for (const auto &binding : bindings) {
        PhysWorldTransform transform;
        if (const auto *object_id = std::get_if<GameObjectId>(&binding.transform_source)) {
            const auto *component = ecs.tryComponent<TransformComponent>(*object_id);
            if (component == nullptr) {
                continue;
            }
            transform = transformFromComponent(*component);
        } else {
            transform = std::get<PhysWorldTransform>(binding.transform_source);
        }
        inputs.push_back(PhysWorldColliderInput{
            .name = binding.identity.name,
            .collider = binding.collider,
            .transform = transform,
            .identity = binding.identity,
            .metadata = binding.metadata,
        });
    }
    return buildPhysColliders(inputs);
}

std::vector<phys::RaycastQueryHit> PhysWorld::raycastAll(
    const phys::Ray &ray, const phys::QueryFilter &filter) const {
    const auto colliders = collectColliders();
    std::vector<phys::RaycastQueryHit> hits;
    const auto status = physics_internal::raycastAll(ray, colliders, &filter, hits);
    if (status == Physics::Status::unavailable) return {};
    if (status != Physics::Status::ok) {
        throw std::runtime_error("Physics query provider raycast failed with status " +
                                 std::to_string(static_cast<std::uint32_t>(status)));
    }
    return hits;
}

std::optional<phys::ObjectRaycastHit> PhysWorld::raycastClosest(
    const phys::Ray &ray) const {
    const auto colliders = collectColliders();
    std::vector<phys::RaycastQueryHit> hits;
    const auto status = physics_internal::raycastAll(ray, colliders, nullptr, hits);
    if (status == Physics::Status::unavailable) return std::nullopt;
    if (status != Physics::Status::ok) {
        throw std::runtime_error("Physics query provider raycast failed with status " +
                                 std::to_string(static_cast<std::uint32_t>(status)));
    }

    std::optional<phys::ObjectRaycastHit> best;
    for (const auto &hit : hits) {
        const bool better_distance =
            !best || hit.distance < best->distance - phys::internal::queryTieEpsilon;
        const bool better_legacy_tie =
            best && std::abs(hit.distance - best->distance) <= phys::internal::queryTieEpsilon &&
            hit.id < best->id;
        if (better_distance || better_legacy_tie) {
            best = phys::ObjectRaycastHit{hit.id, hit.distance, hit.position, hit.normal};
        }
    }
    return best;
}

std::optional<phys::RaycastQueryHit> PhysWorld::raycastClosest(
    const phys::Ray &ray, const phys::QueryFilter &filter) const {
    auto hits = raycastAll(ray, filter);
    if (hits.empty()) return std::nullopt;
    return std::move(hits.front());
}

std::vector<phys::OverlapHit> PhysWorld::overlapAllHits(
    const phys::Shape &shape, const phys::QueryFilter &filter) const {
    const auto colliders = collectColliders();
    std::vector<phys::OverlapHit> hits;
    const auto status = physics_internal::overlapAll(shape, colliders, &filter, hits);
    if (status == Physics::Status::unavailable) return {};
    if (status != Physics::Status::ok) {
        throw std::runtime_error("Physics query provider overlap failed with status " +
                                 std::to_string(static_cast<std::uint32_t>(status)));
    }
    return hits;
}

std::vector<phys::ShapeCastQueryHit> PhysWorld::shapeCastAll(
    const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter) const {
    const auto colliders = collectColliders();
    std::vector<phys::ShapeCastQueryHit> hits;
    const auto status = physics_internal::shapeCastAll(
        moving_shape, delta, colliders, &filter, hits);
    if (status == Physics::Status::unavailable) return {};
    if (status != Physics::Status::ok) {
        throw std::runtime_error("Physics query provider shape cast failed with status " +
                                 std::to_string(static_cast<std::uint32_t>(status)));
    }
    return hits;
}

std::optional<phys::ShapeCastQueryHit> PhysWorld::shapeCastClosest(
    const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter) const {
    auto hits = shapeCastAll(moving_shape, delta, filter);
    if (hits.empty()) return std::nullopt;
    return std::move(hits.front());
}

std::vector<std::string> PhysWorld::overlapAll(
    const phys::Shape &shape) const {
    const auto colliders = collectColliders();
    std::vector<phys::OverlapHit> hits;
    const auto status = physics_internal::overlapAll(shape, colliders, nullptr, hits);
    if (status == Physics::Status::unavailable) return {};
    if (status != Physics::Status::ok) {
        throw std::runtime_error("Physics query provider overlap failed with status " +
                                 std::to_string(static_cast<std::uint32_t>(status)));
    }
    std::vector<std::string> ids;
    ids.reserve(hits.size());
    for (const auto &hit : hits) ids.push_back(hit.id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<std::string> PhysWorld::overlapAll(
    const phys::Shape &shape, const phys::QueryFilter &filter) const {
    const auto hits = overlapAllHits(shape, filter);
    std::vector<std::string> ids;
    ids.reserve(hits.size());
    for (const auto &hit : hits) ids.push_back(hit.id);
    return ids;
}

void PhysWorld::updateTriggers(GameContext &ctx) {
    const auto colliders = collectColliders();
    std::vector<TriggerPair> current_pairs;

    for (const auto &trigger : colliders) {
        if (!trigger.metadata.trigger || !trigger.identity.entity.has_value()) {
            continue;
        }

        const phys::QueryFilter filter{
            .layer = trigger.metadata.layer,
            .mask = trigger.metadata.mask,
            .include_triggers = true,
            .include_one_way = true,
            .self = trigger.identity.collider_id,
            .self_entity = trigger.identity.entity,
        };
        std::vector<phys::OverlapHit> hits;
        const auto status = physics_internal::overlapAll(
            trigger.shape, colliders, &filter, hits);
        if (status == Physics::Status::unavailable) {
            // Provider unavailability says nothing about physical separation.
            // Preserve the last known set rather than manufacture Exit events.
            return;
        }
        if (status != Physics::Status::ok) {
            throw std::runtime_error(
                "Physics trigger overlap failed with status " +
                std::to_string(static_cast<std::uint32_t>(status)));
        }

        for (const auto &hit : hits) {
            if (!hit.identity.entity.has_value() ||
                *hit.identity.entity == *trigger.identity.entity) {
                continue;
            }
            const auto first = std::min(*trigger.identity.entity,
                                        *hit.identity.entity);
            const auto second = std::max(*trigger.identity.entity,
                                         *hit.identity.entity);
            current_pairs.emplace_back(first, second);
        }
    }

    std::sort(current_pairs.begin(), current_pairs.end());
    current_pairs.erase(
        std::unique(current_pairs.begin(), current_pairs.end()),
        current_pairs.end());

    // Merge the two sorted sets so mixed Enter/Exit frames also have one total
    // EntityId-pair order. Each canonical pair is then emitted from both views,
    // lower self first, through the ordinary E1 queue.
    std::size_t previous_index = 0;
    std::size_t current_index = 0;
    while (previous_index < active_trigger_pairs.size() ||
           current_index < current_pairs.size()) {
        if (previous_index < active_trigger_pairs.size() &&
            current_index < current_pairs.size() &&
            active_trigger_pairs[previous_index] == current_pairs[current_index]) {
            ++previous_index;
            ++current_index;
            continue;
        }

        const bool emit_exit =
            previous_index < active_trigger_pairs.size() &&
            (current_index == current_pairs.size() ||
             active_trigger_pairs[previous_index] < current_pairs[current_index]);
        if (emit_exit) {
            const auto &pair = active_trigger_pairs[previous_index++];
            ctx.emit(OverlapExit{.self = pair.first, .other = pair.second});
            ctx.emit(OverlapExit{.self = pair.second, .other = pair.first});
        } else {
            const auto &pair = current_pairs[current_index++];
            ctx.emit(OverlapEnter{.self = pair.first, .other = pair.second});
            ctx.emit(OverlapEnter{.self = pair.second, .other = pair.first});
        }
    }

    active_trigger_pairs = std::move(current_pairs);
}

void PhysWorld::enqueueDebugDraw(DebugDraw &debug_draw,
                                 const glm::mat4 &view_projection) const {
    const auto colliders = collectColliders();
    for (const auto &collider : colliders) {
        drawCollider(debug_draw, view_projection, collider);
    }
}

} // namespace Pelican
