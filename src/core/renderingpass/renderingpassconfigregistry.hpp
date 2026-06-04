#pragma once

#include "../container.hpp"
#include <string>

namespace Pelican {

DECLARE_MODULE(RenderingPassConfigRegistry) {
  public:
    void registerFromJson(const std::string &json_path) const;
};

} // namespace Pelican
