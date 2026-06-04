#pragma once

#include "rendertargetdefinition.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

std::vector<RenderTargetDefinition> parseRenderTargetDefinitionsFromJson(const nlohmann::json &data,
                                                                         vk::Extent2D base_extent);

} // namespace Pelican
