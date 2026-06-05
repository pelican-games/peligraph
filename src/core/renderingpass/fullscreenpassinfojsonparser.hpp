#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace Pelican {

FullscreenPassInfo parseFullscreenPassInfoFromJson(const nlohmann::json &pass_json,
                                                   const std::string &pass_name);

} // namespace Pelican
