#include "physqueryinternal.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace Pelican::phys {
namespace {

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

namespace internal {

bool passesQueryFilter(const Collider &collider, const ColliderIdentity &identity,
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

void orderRaycastHits(std::vector<RaycastQueryHit> &hits) {
    std::sort(hits.begin(), hits.end(), [](const RaycastQueryHit &lhs,
                                           const RaycastQueryHit &rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        return colliderIdentityLess(lhs.identity, rhs.identity);
    });

    // An epsilon cluster is anchored at its closest raw distance. This keeps
    // the result independent of provider/collider iteration order.
    std::size_t cluster_begin = 0;
    while (cluster_begin < hits.size()) {
        const float anchor = hits[cluster_begin].distance;
        std::size_t cluster_end = cluster_begin + 1;
        while (cluster_end < hits.size() &&
               hits[cluster_end].distance <= anchor + queryTieEpsilon) {
            ++cluster_end;
        }
        std::sort(hits.begin() + static_cast<std::ptrdiff_t>(cluster_begin),
                  hits.begin() + static_cast<std::ptrdiff_t>(cluster_end),
                  [](const RaycastQueryHit &lhs, const RaycastQueryHit &rhs) {
                      return colliderIdentityLess(lhs.identity, rhs.identity);
                  });
        cluster_begin = cluster_end;
    }
}

void orderOverlapHits(std::vector<OverlapHit> &hits) {
    std::sort(hits.begin(), hits.end(), [](const OverlapHit &lhs, const OverlapHit &rhs) {
        return colliderIdentityLess(lhs.identity, rhs.identity);
    });
}

} // namespace internal
} // namespace Pelican::phys
