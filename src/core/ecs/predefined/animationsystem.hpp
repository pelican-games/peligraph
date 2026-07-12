#pragma once

#include "../../container.hpp"
#include <components/animation.hpp>
#include <components/modelview.hpp>
#include <details/ecs/coretemplate.hpp>
#include <memory>

namespace Pelican {

struct AnimationSystemState;

DECLARE_MODULE(AnimationSystem) {
    std::unique_ptr<AnimationSystemState> state;

  public:
    using QueryComponents = std::tuple<AnimationComponent *, SimpleModelViewComponent *>;
    AnimationSystem();
    ~AnimationSystem();
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
