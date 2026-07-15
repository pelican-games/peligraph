#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <span>

#include "modelview.hpp"
#include "transform.hpp"

namespace Pelican {

class PolygonInstanceContainer;

DECLARE_MODULE(SimpleModelViewTransformSystem) {
    PolygonInstanceContainer *instances = nullptr;

  public:
    using QueryComponents = std::tuple<TransformComponent *, SimpleModelViewComponent *>;
    void prepareEcsWorkerDependencies(bool has_matching_chunks);
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
