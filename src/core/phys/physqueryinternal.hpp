#pragma once

#include "physquery.hpp"

namespace Pelican::phys::internal {

inline constexpr float queryTieEpsilon = 1.0e-5F;

[[nodiscard]] bool passesQueryFilter(const Collider &collider,
                                     const ColliderIdentity &identity,
                                     const QueryFilter &filter);
void orderRaycastHits(std::vector<RaycastQueryHit> &hits);
void orderOverlapHits(std::vector<OverlapHit> &hits);

} // namespace Pelican::phys::internal
