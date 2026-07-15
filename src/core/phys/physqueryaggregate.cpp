#include "physquery.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <string_view>

namespace Pelican::phys {
namespace {

constexpr float kTieEpsilon = 1.0e-5f;
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

ColliderId compatibilityColliderId(std::string_view name, std::uint32_t shape_ordinal) {
    std::uint64_t value = kFnvOffsetBasis;
    for (const unsigned char character : name) {
        value ^= character;
        value *= kFnvPrime;
    }
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value ^= static_cast<unsigned char>((shape_ordinal >> shift) & 0xffU);
        value *= kFnvPrime;
    }
    if (value == invalid_collider_id.value) {
        value = 1;
    }
    return ColliderId{value};
}

bool isIgnored(const ColliderIdentity &identity, const QueryFilter &filter) {
    if (filter.self && *filter.self == identity.collider_id) {
        return true;
    }
    if (std::find(filter.ignored.begin(), filter.ignored.end(), identity.collider_id) !=
        filter.ignored.end()) {
        return true;
    }
    if (!identity.entity) {
        return false;
    }
    if (filter.self_entity && *filter.self_entity == *identity.entity) {
        return true;
    }
    return std::find(filter.ignored_entities.begin(), filter.ignored_entities.end(),
                     *identity.entity) != filter.ignored_entities.end();
}

bool passesFilter(const Collider &collider, const ColliderIdentity &identity,
                  const QueryFilter &filter) {
    if ((filter.mask & collider.metadata.layer) == 0U ||
        (collider.metadata.mask & filter.layer) == 0U) {
        return false;
    }
    if (!filter.include_triggers && collider.metadata.trigger) {
        return false;
    }
    if (!filter.include_one_way && collider.metadata.one_way) {
        return false;
    }
    return !isIgnored(identity, filter);
}

bool hitIdentityLess(const RaycastQueryHit &lhs, const RaycastQueryHit &rhs) {
    return colliderIdentityLess(lhs.identity, rhs.identity);
}

void orderRaycastHits(std::vector<RaycastQueryHit> &hits) {
    std::sort(hits.begin(), hits.end(), [](const RaycastQueryHit &lhs,
                                           const RaycastQueryHit &rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        return hitIdentityLess(lhs, rhs);
    });

    // Preserve the original epsilon tie contract while making its result
    // independent of collider iteration order. Each cluster is anchored to its
    // closest raw distance, then ordered by the shared identity key.
    size_t cluster_begin = 0;
    while (cluster_begin < hits.size()) {
        const float anchor = hits[cluster_begin].distance;
        size_t cluster_end = cluster_begin + 1;
        while (cluster_end < hits.size() &&
               hits[cluster_end].distance <= anchor + kTieEpsilon) {
            ++cluster_end;
        }
        std::sort(hits.begin() + static_cast<std::ptrdiff_t>(cluster_begin),
                  hits.begin() + static_cast<std::ptrdiff_t>(cluster_end),
                  hitIdentityLess);
        cluster_begin = cluster_end;
    }
}

} // namespace

ColliderIdentity effectiveColliderIdentity(const Collider &collider) {
    ColliderIdentity identity = collider.identity;
    if (identity.name.empty()) {
        identity.name = collider.id;
    }
    if (!identity.collider_id.valid()) {
        identity.collider_id = compatibilityColliderId(identity.name, identity.shape_ordinal);
    }
    return identity;
}

bool colliderIdentityLess(const ColliderIdentity &lhs, const ColliderIdentity &rhs) {
    if (lhs.collider_id != rhs.collider_id) {
        return lhs.collider_id < rhs.collider_id;
    }
    if (lhs.entity != rhs.entity) {
        return lhs.entity < rhs.entity;
    }
    if (lhs.shape_ordinal != rhs.shape_ordinal) {
        return lhs.shape_ordinal < rhs.shape_ordinal;
    }
    return lhs.name < rhs.name;
}

std::vector<RaycastQueryHit> raycastAll(const Ray &ray,
                                        std::span<const Collider> colliders,
                                        const QueryFilter &filter) {
    std::vector<RaycastQueryHit> hits;
    hits.reserve(colliders.size());

    for (const Collider &collider : colliders) {
        const ColliderIdentity identity = effectiveColliderIdentity(collider);
        if (!passesFilter(collider, identity, filter)) {
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

    orderRaycastHits(hits);
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
        const bool better_distance = !best || hit->distance < best->distance - kTieEpsilon;
        const bool better_legacy_tie = best &&
            std::abs(hit->distance - best->distance) <= kTieEpsilon &&
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
        if (!passesFilter(collider, identity, filter) ||
            !overlaps(shape, collider.shape)) {
            continue;
        }
        hits.push_back(OverlapHit{identity.name, identity, collider.metadata});
    }

    std::sort(hits.begin(), hits.end(), [](const OverlapHit &lhs, const OverlapHit &rhs) {
        return colliderIdentityLess(lhs.identity, rhs.identity);
    });
    return hits;
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
