#pragma once

#include "renderingpass.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace Pelican {

GenericRasterPassInfo parseGenericRasterPassInfoFromJson(
    const nlohmann::json &pass_json,
    const std::string &pass_name,
    std::size_t color_attachment_count,
    bool has_depth_attachment);

} // namespace Pelican
