#pragma once

#include "../container.hpp"

namespace Pelican {

class Loop {
    DependencyContainer &con;

  public:
    Loop(DependencyContainer &_con);
    void run();
};

} // namespace Pelican
