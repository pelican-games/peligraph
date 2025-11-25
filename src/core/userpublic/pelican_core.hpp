#pragma once

#include <string>

namespace Pelican {

class PelicanCore {
    std::string settings_str;

  public:
    PelicanCore();
    PelicanCore(std::string settings);
    void run();
};

} // namespace Pelican
