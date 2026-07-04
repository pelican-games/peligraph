#pragma once

#include <string>

namespace Pelican {

class PelicanCore {
    std::string settings_str;

  public:
    PelicanCore();
    PelicanCore(std::string settings);
    PelicanCore(std::string settings, bool reserve_stdout_for_protocol);
    void run();
};

} // namespace Pelican
