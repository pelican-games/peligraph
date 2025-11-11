#include "polygoninstancecontainer.hpp"

namespace Pelican {

PolygonInstanceContainer::PolygonInstanceContainer(DependencyContainer &con) {}

ModelInstanceId PolygonInstanceContainer::placeModelInstance(ModelTemplate &model) { return {}; }
void PolygonInstanceContainer::removeModelInstance(ModelInstanceId id) {}
void PolygonInstanceContainer::triggerOptimize() {}
const std::vector<PolygonInstance> &PolygonInstanceContainer::getPolygons() const { return {}; }

} // namespace Pelican
