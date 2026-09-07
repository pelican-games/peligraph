#include "meshphysicsprovider.hpp"
#include "trianglemesh.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <vector>

namespace Pelican::physics_internal {

namespace {
template<class Query, class Hit, class Evaluate>
Physics::Status builtinMeshQuery(
    void *context, const Query *query, Hit *hits, std::uint32_t capacity,
    std::uint32_t *out_count, Evaluate evaluate) noexcept {
    using namespace Physics;
    if (query == nullptr || out_count == nullptr ||
        (capacity != 0 && hits == nullptr) || query->reserved != 0 ||
        (query->collider_count != 0 && query->colliders == nullptr))
        return Status::invalid_argument;
    *out_count = 0;
    try {
        std::vector<Hit> result;
        for (std::uint32_t index = 0; index < query->collider_count; ++index) {
            const auto &source = query->colliders[index];
            if ((source.vertex_count != 0 && source.vertices == nullptr) ||
                (source.index_count != 0 && source.indices == nullptr) ||
                source.reserved0 != 0 || source.reserved1 != 0)
                return Status::invalid_argument;
            const auto *cached = static_cast<const BuiltinMeshQueryContext *>(context);
            const phys::TriangleMesh *mesh = cached && index < cached->acceleration.size()
                ? cached->acceleration[index] : nullptr;
            std::optional<phys::TriangleMesh> temporary;
            if (mesh == nullptr) {
                std::vector<vec3> vertices;
                vertices.reserve(source.vertex_count);
                for (std::uint32_t vertex = 0; vertex < source.vertex_count; ++vertex) {
                    const auto value = source.vertices[vertex];
                    vertices.push_back({value.x, value.y, value.z});
                }
                temporary.emplace(vertices, std::span{source.indices, source.index_count});
                mesh = &*temporary;
            }
            glm::mat4 transform;
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    transform[column][row] = source.local_to_world[column * 4 + row];
            if (auto hit = evaluate(*mesh, transform, *query, index)) result.push_back(*hit);
        }
        *out_count = static_cast<std::uint32_t>(result.size());
        if (capacity < result.size()) return Status::buffer_too_small;
        if (!result.empty()) std::copy(result.begin(), result.end(), hits);
        return Status::ok;
    } catch (const std::domain_error &) {
        return Status::unavailable;
    } catch (const std::invalid_argument &) {
        return Status::invalid_argument;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace

Physics::Status builtinMeshRaycastAll(
    void *context, const Physics::ProviderMeshRaycastQueryV3 *query,
    Physics::ProviderRaycastHitV1 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    return builtinMeshQuery(context, query, hits, capacity, out_count,
        [](const phys::TriangleMesh &mesh, const glm::mat4 &transform,
           const Physics::ProviderMeshRaycastQueryV3 &query, std::uint32_t index)
            -> std::optional<Physics::ProviderRaycastHitV1> {
            const auto &ray = query.ray;
            const auto hit = mesh.raycast(
                phys::Ray{{ray.origin.x, ray.origin.y, ray.origin.z},
                          {ray.direction.x, ray.direction.y, ray.direction.z}, ray.max_distance},
                transform);
            if (!hit) return std::nullopt;
            return Physics::ProviderRaycastHitV1{
                index, 0, hit->distance, {hit->normal.x, hit->normal.y, hit->normal.z}};
        });
}

Physics::Status builtinMeshSphereOverlapAll(
    void *context, const Physics::ProviderMeshSphereOverlapQueryV3 *query,
    Physics::ProviderOverlapHitV1 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    return builtinMeshQuery(context, query, hits, capacity, out_count,
        [](const phys::TriangleMesh &mesh, const glm::mat4 &transform,
           const Physics::ProviderMeshSphereOverlapQueryV3 &query, std::uint32_t index)
            -> std::optional<Physics::ProviderOverlapHitV1> {
            if (!mesh.overlaps(phys::Sphere{{query.center.x, query.center.y, query.center.z},
                                           query.radius}, transform)) return std::nullopt;
            return Physics::ProviderOverlapHitV1{index, 0};
        });
}

Physics::Status builtinMeshBoxOverlapAll(
    void *context, const Physics::ProviderMeshBoxOverlapQueryV3 *query,
    Physics::ProviderOverlapHitV1 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    return builtinMeshQuery(context, query, hits, capacity, out_count,
        [](const phys::TriangleMesh &mesh, const glm::mat4 &transform,
           const Physics::ProviderMeshBoxOverlapQueryV3 &query, std::uint32_t index)
            -> std::optional<Physics::ProviderOverlapHitV1> {
            if (!mesh.overlaps(phys::Box{{query.center.x, query.center.y, query.center.z},
                    {query.rotation.x, query.rotation.y, query.rotation.z, query.rotation.w},
                    {query.half_extents.x, query.half_extents.y, query.half_extents.z}}, transform)) return std::nullopt;
            return Physics::ProviderOverlapHitV1{index, 0};
        });
}

Physics::Status builtinMeshCapsuleOverlapAll(
    void *context, const Physics::ProviderMeshCapsuleOverlapQueryV3 *query,
    Physics::ProviderOverlapHitV1 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    return builtinMeshQuery(context, query, hits, capacity, out_count,
        [](const phys::TriangleMesh &mesh, const glm::mat4 &transform,
           const Physics::ProviderMeshCapsuleOverlapQueryV3 &query, std::uint32_t index)
            -> std::optional<Physics::ProviderOverlapHitV1> {
            if (!mesh.overlaps(phys::Capsule{{query.center.x, query.center.y, query.center.z},
                    {query.rotation.x, query.rotation.y, query.rotation.z, query.rotation.w},
                    query.half_height, query.radius}, transform)) return std::nullopt;
            return Physics::ProviderOverlapHitV1{index, 0};
        });
}

Physics::Status builtinMeshSphereShapeCastAll(
    void *context, const Physics::ProviderMeshSphereShapeCastQueryV3 *query,
    Physics::ProviderShapeCastHitV2 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    return builtinMeshQuery(context, query, hits, capacity, out_count,
        [](const phys::TriangleMesh &mesh, const glm::mat4 &transform,
           const Physics::ProviderMeshSphereShapeCastQueryV3 &query, std::uint32_t index)
            -> std::optional<Physics::ProviderShapeCastHitV2> {
            const auto hit = mesh.shapeCast(
                phys::Sphere{{query.center.x, query.center.y, query.center.z}, query.radius},
                {query.delta.x, query.delta.y, query.delta.z}, transform);
            if (!hit) return std::nullopt;
            Physics::ProviderShapeCastHitV2 result;
            result.collider_index = index;
            result.time_of_impact = hit->time_of_impact;
            result.penetration_depth = hit->penetration_depth;
            result.position = {hit->position.x, hit->position.y, hit->position.z};
            result.normal = {hit->normal.x, hit->normal.y, hit->normal.z};
            result.flags = (hit->initial_overlap ? Physics::shape_cast_initial_overlap : 0U) |
                (hit->approximate_depenetration ? Physics::shape_cast_approximate_depenetration : 0U);
            return result;
        });
}

Physics::Status builtinMeshBoxShapeCastAll(
    void *context, const Physics::ProviderMeshBoxShapeCastQueryV3 *query,
    Physics::ProviderShapeCastHitV2 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    return builtinMeshQuery(context, query, hits, capacity, out_count,
        [](const phys::TriangleMesh &mesh, const glm::mat4 &transform,
           const Physics::ProviderMeshBoxShapeCastQueryV3 &query, std::uint32_t index)
            -> std::optional<Physics::ProviderShapeCastHitV2> {
            const auto hit = mesh.shapeCast(
                phys::Box{{query.center.x, query.center.y, query.center.z},
                    {query.rotation.x, query.rotation.y, query.rotation.z, query.rotation.w},
                    {query.half_extents.x, query.half_extents.y, query.half_extents.z}},
                {query.delta.x, query.delta.y, query.delta.z}, transform);
            if (!hit) return std::nullopt;
            Physics::ProviderShapeCastHitV2 result;
            result.collider_index = index;
            result.time_of_impact = hit->time_of_impact;
            result.penetration_depth = hit->penetration_depth;
            result.position = {hit->position.x, hit->position.y, hit->position.z};
            result.normal = {hit->normal.x, hit->normal.y, hit->normal.z};
            result.flags = (hit->initial_overlap ? Physics::shape_cast_initial_overlap : 0U) |
                (hit->approximate_depenetration ? Physics::shape_cast_approximate_depenetration : 0U);
            return result;
        });
}

Physics::Status builtinMeshCapsuleShapeCastAll(
    void *context, const Physics::ProviderMeshCapsuleShapeCastQueryV3 *query,
    Physics::ProviderShapeCastHitV2 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    return builtinMeshQuery(context, query, hits, capacity, out_count,
        [](const phys::TriangleMesh &mesh, const glm::mat4 &transform,
           const Physics::ProviderMeshCapsuleShapeCastQueryV3 &query, std::uint32_t index)
            -> std::optional<Physics::ProviderShapeCastHitV2> {
            const auto hit = mesh.shapeCast(
                phys::Capsule{{query.center.x, query.center.y, query.center.z},
                    {query.rotation.x, query.rotation.y, query.rotation.z, query.rotation.w},
                    query.half_height, query.radius},
                {query.delta.x, query.delta.y, query.delta.z}, transform);
            if (!hit) return std::nullopt;
            Physics::ProviderShapeCastHitV2 result;
            result.collider_index = index;
            result.time_of_impact = hit->time_of_impact;
            result.penetration_depth = hit->penetration_depth;
            result.position = {hit->position.x, hit->position.y, hit->position.z};
            result.normal = {hit->normal.x, hit->normal.y, hit->normal.z};
            result.flags = (hit->initial_overlap ? Physics::shape_cast_initial_overlap : 0U) |
                (hit->approximate_depenetration ? Physics::shape_cast_approximate_depenetration : 0U);
            return result;
        });
}

Physics::Status builtinMeshMeshOverlapAll(
    void *context, const Physics::ProviderMeshMeshOverlapQueryV3 *query,
    Physics::ProviderOverlapHitV1 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    const auto *cached = static_cast<const BuiltinMeshQueryContext *>(context);
    const phys::TriangleMesh *queryMesh = cached ? cached->query_acceleration : nullptr;
    std::optional<phys::TriangleMesh> temporary;
    return builtinMeshQuery(context,query,hits,capacity,out_count,
        [&](const phys::TriangleMesh &mesh, const glm::mat4 &transform,
            const Physics::ProviderMeshMeshOverlapQueryV3 &query, std::uint32_t index)
            -> std::optional<Physics::ProviderOverlapHitV1> {
            const auto &source = query.shape;
            if (queryMesh == nullptr) {
                if (source.reserved0 != 0 || source.reserved1 != 0 ||
                    (source.vertex_count != 0 && source.vertices == nullptr) ||
                    (source.index_count != 0 && source.indices == nullptr))
                    throw std::invalid_argument("Invalid mesh overlap query geometry");
                std::vector<vec3> vertices;
                vertices.reserve(source.vertex_count);
                for (std::uint32_t i = 0; i < source.vertex_count; ++i)
                    vertices.push_back({source.vertices[i].x,source.vertices[i].y,source.vertices[i].z});
                temporary.emplace(vertices,std::span{source.indices,source.index_count});
                queryMesh = &*temporary;
            }
            glm::mat4 queryTransform;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) queryTransform[c][r] = source.local_to_world[c*4+r];
            if (!queryMesh->overlaps(mesh,queryTransform,transform)) return std::nullopt;
            return Physics::ProviderOverlapHitV1{index,0};
        });
}

} // namespace Pelican::physics_internal
