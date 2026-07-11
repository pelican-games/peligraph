#pragma once

#include "../../container.hpp"
#include <components/animation.hpp>
#include <components/modelview.hpp>
#include <details/ecs/coretemplate.hpp>

namespace Pelican {

DECLARE_MODULE(AnimationSystem) {
  public:
    using QueryComponents = std::tuple<AnimationComponent *, SimpleModelViewComponent *>;
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
