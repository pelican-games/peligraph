#include "physqueryinternal.hpp"

#include <algorithm>
#include <cmath>

namespace Pelican::phys {

std::vector<RaycastQueryHit> raycastAll(const Ray &ray,
                                        std::span<const Collider> colliders,
                                        const QueryFilter &filter) {
    std::vector<RaycastQueryHit> hits;
    hits.reserve(colliders.size());

    for (const Collider &collider : colliders) {
        const ColliderIdentity identity = effectiveColliderIdentity(collider);
        if (!internal::passesQueryFilter(collider, identity, filter)) {
            continue;
        }

        const std::optional<RaycastHit> hit = raycast(ray, collider.shape);
        if (!hit) {
            continue;
        }

        hits.push_back(RaycastQueryHit{
            identity.name,
            hit->distance,
            hit->position,
            hit->normal,
            identity,
            collider.metadata,
        });
    }

    internal::orderRaycastHits(hits);
    return hits;
}

std::optional<ObjectRaycastHit> raycastClosest(const Ray &ray,
                                               std::span<const Collider> colliders) {
    std::optional<ObjectRaycastHit> best;
    for (const Collider &collider : colliders) {
        const auto hit = raycast(ray, collider.shape);
        if (!hit) {
            continue;
        }
        const bool better_distance =
            !best || hit->distance < best->distance - internal::queryTieEpsilon;
        const bool better_legacy_tie = best &&
            std::abs(hit->distance - best->distance) <= internal::queryTieEpsilon &&
            collider.id < best->id;
        if (better_distance || better_legacy_tie) {
            best = ObjectRaycastHit{collider.id, hit->distance, hit->position, hit->normal};
        }
    }
    return best;
}

std::optional<RaycastQueryHit> raycastClosest(const Ray &ray,
                                              std::span<const Collider> colliders,
                                              const QueryFilter &filter) {
    auto hits = raycastAll(ray, colliders, filter);
    if (hits.empty()) {
        return std::nullopt;
    }
    return std::move(hits.front());
}

std::vector<OverlapHit> overlapAllHits(const Shape &shape,
                                       std::span<const Collider> colliders,
                                       const QueryFilter &filter) {
    std::vector<OverlapHit> hits;
    hits.reserve(colliders.size());

    for (const Collider &collider : colliders) {
        const ColliderIdentity identity = effectiveColliderIdentity(collider);
        if (!internal::passesQueryFilter(collider, identity, filter) ||
            !overlaps(shape, collider.shape)) {
            continue;
        }
        hits.push_back(OverlapHit{identity.name, identity, collider.metadata});
    }

    internal::orderOverlapHits(hits);
    return hits;
}

std::vector<ShapeCastQueryHit> shapeCastAll(
    const Shape &moving_shape, vec3 delta, std::span<const Collider> colliders,
    const QueryFilter &filter) {
    std::vector<ShapeCastQueryHit> hits;
    hits.reserve(colliders.size());

    for (const Collider &collider : colliders) {
        const ColliderIdentity identity = effectiveColliderIdentity(collider);
        if (!internal::passesQueryFilter(collider, identity, filter)) {
            continue;
        }

        const auto hit = shapeCast(moving_shape, delta, collider.shape);
        if (!hit) {
            continue;
        }
        hits.push_back(ShapeCastQueryHit{
            identity.name,
            hit->time_of_impact,
            hit->penetration_depth,
            hit->position,
            hit->normal,
            hit->initial_overlap,
            identity,
            collider.metadata,
        });
    }

    internal::orderShapeCastHits(hits);
    return hits;
}

std::optional<ShapeCastQueryHit> shapeCastClosest(
    const Shape &moving_shape, vec3 delta, std::span<const Collider> colliders,
    const QueryFilter &filter) {
    auto hits = shapeCastAll(moving_shape, delta, colliders, filter);
    if (hits.empty()) {
        return std::nullopt;
    }
    return std::move(hits.front());
}

std::vector<std::string> overlapAll(const Shape &shape,
                                    std::span<const Collider> colliders) {
    std::vector<std::string> ids;
    for (const Collider &collider : colliders) {
        if (overlaps(shape, collider.shape)) {
            ids.push_back(collider.id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<std::string> overlapAll(const Shape &shape,
                                    std::span<const Collider> colliders,
                                    const QueryFilter &filter) {
    const auto hits = overlapAllHits(shape, colliders, filter);
    std::vector<std::string> ids;
    ids.reserve(hits.size());
    for (const auto &hit : hits) {
        ids.push_back(hit.id);
    }
    return ids;
}

} // namespace Pelican::phys
