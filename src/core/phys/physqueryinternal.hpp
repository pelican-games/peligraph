#pragma once

#include "physquery.hpp"

namespace Pelican::phys::internal {

inline constexpr float queryTieEpsilon = shapeCastTieEpsilon;

[[nodiscard]] bool passesQueryFilter(const Collider &collider,
                                     const ColliderIdentity &identity,
                                     const QueryFilter &filter);
void orderRaycastHits(std::vector<RaycastQueryHit> &hits);
void orderOverlapHits(std::vector<OverlapHit> &hits);
void orderShapeCastHits(std::vector<ShapeCastQueryHit> &hits);

} // namespace Pelican::phys::internal
