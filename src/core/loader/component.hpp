#pragma once

#include "../container.hpp"
#include "../ecs/component.hpp"
#include <nlohmann/json.hpp>
#include <string_view>

namespace Pelican {

DECLARE_MODULE(ComponentLoader) {
  public:
    ComponentId componentIdFromName(std::string_view name) const;
    void initByJson(void *dst_ptr, const nlohmann::json &hint) const;
    void deinit(void *ptr) const;
};

} // namespace Pelican
