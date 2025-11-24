#pragma once

#include "../container.hpp"
#include "../ecs/component.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

DECLARE_MODULE(ComponentLoader) {
  public:
    void initByJson(void *dst_ptr, const nlohmann::json &hint) const;
    void deinit(void *ptr) const;
};

} // namespace Pelican
