#pragma once

#include "rendertargetdefinition.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

std::vector<RenderTargetDefinition> parseRenderTargetDefinitionsFromJson(const nlohmann::json &data);

} // namespace Pelican
