#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

class RenderTargetContainer;

std::vector<RenderingPassDefinition>
parseRenderingPassDefinitionsFromConfigJson(const nlohmann::json &rendering_pass_data,
                                            RenderTargetContainer &rt_container);

} // namespace Pelican
