#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace Pelican {

nlohmann::json loadRenderingPassConfigJson(const std::string &json_path);

} // namespace Pelican
