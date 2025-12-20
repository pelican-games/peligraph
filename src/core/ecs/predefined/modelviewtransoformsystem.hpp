#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <span>

#include "modelview.hpp"
#include "transform.hpp"

namespace Pelican {

DECLARE_MODULE(SimpleModelViewTransformSystem) {
  public:
    using QueryComponents = std::tuple<TransformComponent *, SimpleModelViewComponent *>;
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
