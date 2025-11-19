#pragma once

#include "../container.hpp"

namespace Pelican {

DECLARE_MODULE(Scene) {
  public:
    Scene();
    ~Scene();

    void load();
};

} // namespace Pelican
