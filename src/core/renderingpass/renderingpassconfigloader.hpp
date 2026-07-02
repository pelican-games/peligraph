#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace Pelican {

nlohmann::json loadRenderingPassConfigJson(const std::string &json_path);
nlohmann::json loadRenderingPassConfigJsonFromString(std::string_view json_data, std::string_view source_name);

} // namespace Pelican
