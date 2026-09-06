#include "meshphysicsprovider.hpp"
#include "trianglemesh.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <vector>

namespace Pelican::physics_internal {

Physics::Status builtinMeshRaycastAll(
    void *, const Physics::ProviderMeshRaycastQueryV3 *query,
    Physics::ProviderRaycastHitV1 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    using namespace Physics;
    if (query == nullptr || out_count == nullptr ||
        (capacity != 0 && hits == nullptr) || query->reserved != 0 ||
        (query->collider_count != 0 && query->colliders == nullptr))
        return Status::invalid_argument;
    *out_count = 0;
    try {
        std::vector<ProviderRaycastHitV1> result;
        for (std::uint32_t index = 0; index < query->collider_count; ++index) {
            const auto &source = query->colliders[index];
            if ((source.vertex_count != 0 && source.vertices == nullptr) ||
                (source.index_count != 0 && source.indices == nullptr) ||
                source.reserved0 != 0 || source.reserved1 != 0)
                return Status::invalid_argument;
            std::vector<vec3> vertices;
            vertices.reserve(source.vertex_count);
            for (std::uint32_t vertex = 0; vertex < source.vertex_count; ++vertex) {
                const auto value = source.vertices[vertex];
                vertices.push_back({value.x, value.y, value.z});
            }
            const phys::TriangleMesh mesh(vertices, {source.indices, source.index_count});
            glm::mat4 transform;
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    transform[column][row] = source.local_to_world[column * 4 + row];
            const auto &ray = query->ray;
            const auto hit = mesh.raycast(
                phys::Ray{{ray.origin.x, ray.origin.y, ray.origin.z},
                          {ray.direction.x, ray.direction.y, ray.direction.z}, ray.max_distance},
                transform);
            if (hit) result.push_back(ProviderRaycastHitV1{
                index, 0, hit->distance, {hit->normal.x, hit->normal.y, hit->normal.z}});
        }
        *out_count = static_cast<std::uint32_t>(result.size());
        if (capacity < result.size()) return Status::buffer_too_small;
        if (!result.empty()) std::copy(result.begin(), result.end(), hits);
        return Status::ok;
    } catch (const std::invalid_argument &) {
        return Status::invalid_argument;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace Pelican::physics_internal
