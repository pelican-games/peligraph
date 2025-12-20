#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <span>

#include "modelview.hpp"
#include <components/modelview.hpp>

namespace Pelican {

DECLARE_MODULE(SimpleModelViewUpdateSystem) {
  public:
    using QueryComponents = std::tuple<SimpleModelViewComponent *, SimpleModelViewUpdateComponent *>;
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
