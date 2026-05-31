#pragma once

#include "../container.hpp"
#include <string>

namespace Pelican {

DECLARE_MODULE(RenderingPassJsonLoader) {
  public:
    void registerRenderingPassesFromJson(const std::string &json_path) const;
};

} // namespace Pelican
