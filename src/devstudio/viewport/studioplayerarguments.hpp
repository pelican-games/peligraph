#pragma once

#include <string>
#include <vector>

namespace PelicanStudio {

std::vector<std::string> studioPlayerArgumentStrings(
    std::vector<std::string> configured,
    const std::string &project_root);

} // namespace PelicanStudio
